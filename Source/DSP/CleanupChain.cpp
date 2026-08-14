#include "CleanupChain.h"
#include <cmath>
#include <algorithm>

namespace listenator
{

namespace
{
    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }
    inline float gainToDb (float g)  noexcept { return g > 1.0e-9f ? 20.0f * std::log10 (g) : -180.0f; }

    /** One-pole smoothing coefficient for a given time constant. */
    inline float timeCoef (float ms, double sr) noexcept
    {
        if (ms <= 0.0f) return 0.0f;
        return std::exp (-1.0f / (float) (0.001 * ms * sr));
    }
}

//==============================================================================
// SpectralEngine
//==============================================================================
void SpectralEngine::prepare (double sampleRate)
{
    sr = sampleRate;
    inputRing.assign ((size_t) size * 2, 0.0f);
    outputRing.assign ((size_t) size * 2, 0.0f);
    frame.assign ((size_t) size * 2, 0.0f);
    noiseMag.assign ((size_t) size / 2 + 1, 0.0f);
    revEstimate.assign ((size_t) size / 2 + 1, 0.0f);
    prevMag.assign ((size_t) size / 2 + 1, 0.0f);
    reset();
}

void SpectralEngine::reset()
{
    std::fill (inputRing.begin(),  inputRing.end(),  0.0f);
    std::fill (outputRing.begin(), outputRing.end(), 0.0f);
    std::fill (revEstimate.begin(), revEstimate.end(), 0.0f);
    std::fill (prevMag.begin(), prevMag.end(), 0.0f);
    writeIdx = 0;
    samplesUntilFrame = hop;
}

void SpectralEngine::setNoiseProfile (const float* magPerBin)
{
    if (magPerBin != nullptr)
        std::copy (magPerBin, magPerBin + noiseMag.size(), noiseMag.begin());
}

void SpectralEngine::setDeverbDecay (float rt60) noexcept
{
    // Per-hop energy decay of the late field. Larger RT60 -> slower decay ->
    // the running estimate carries further, so we subtract more tail.
    if (rt60 <= 0.01f) { deverbAlpha = 0.0f; return; }
    const float hopSeconds = (float) hop / (float) sr;
    deverbAlpha = std::clamp (std::pow (10.0f, -3.0f * hopSeconds / rt60), 0.0f, 0.95f);
}

void SpectralEngine::process (float* block, int numSamples)
{
    if (denoise <= 0.0f && deverb <= 0.0f && resonanceDepth <= 0.0f)
        return;

    for (int n = 0; n < numSamples; ++n)
    {
        inputRing[(size_t) writeIdx] = block[n];
        block[n] = outputRing[(size_t) writeIdx];
        outputRing[(size_t) writeIdx] = 0.0f;

        writeIdx = (writeIdx + 1) % (size * 2);

        if (--samplesUntilFrame == 0)
        {
            samplesUntilFrame = hop;
            processFrame();
        }
    }
}

void SpectralEngine::processFrame()
{
    const int start = (writeIdx - size + size * 2) % (size * 2);

    std::fill (frame.begin(), frame.end(), 0.0f);
    for (int i = 0; i < size; ++i)
        frame[(size_t) i] = inputRing[(size_t) ((start + i) % (size * 2))];

    win.multiplyWithWindowingTable (frame.data(), (size_t) size);
    fft.performRealOnlyForwardTransform (frame.data(), true);

    auto* cplx = reinterpret_cast<juce::dsp::Complex<float>*> (frame.data());
    const int numBins = size / 2 + 1;

    // ---- spectral magnitudes -------------------------------------------
    std::vector<float> mag ((size_t) numBins);
    for (int b = 0; b < numBins; ++b)
        mag[(size_t) b] = std::abs (cplx[b]);

    // ---- resonance suppression: duck bins above their local envelope ----
    std::vector<float> gains ((size_t) numBins, 1.0f);

    if (resonanceDepth > 0.0f)
    {
        // envelope via moving average in log-frequency (~1/2 octave)
        std::vector<float> env ((size_t) numBins);
        for (int b = 1; b < numBins; ++b)
        {
            const int span = std::max (2, b / 6);
            const int a = std::max (1, b - span), c = std::min (numBins - 1, b + span);
            float sum = 0.0f;
            for (int j = a; j <= c; ++j) sum += mag[(size_t) j];
            env[(size_t) b] = sum / (float) (c - a + 1);
        }

        for (int b = 1; b < numBins; ++b)
        {
            if (env[(size_t) b] <= 1.0e-9f) continue;
            const float excessDb = gainToDb (mag[(size_t) b] / env[(size_t) b]);
            if (excessDb > 3.0f)
            {
                const float cutDb = -(excessDb - 3.0f) * resonanceDepth;
                gains[(size_t) b] *= dbToGain (std::max (cutDb, -18.0f));
            }
        }
    }

    // ---- de-noise: spectral subtraction against the measured profile ----
    if (denoise > 0.0f)
    {
        const float over = 1.0f + 2.0f * denoise;   // over-subtraction factor
        for (int b = 0; b < numBins; ++b)
        {
            const float noise = noiseMag[(size_t) b] * over;
            const float m = mag[(size_t) b];
            if (m <= 1.0e-9f) continue;
            // spectral floor keeps musical noise down instead of gating to zero
            const float clean = std::max (m - noise, m * 0.05f);
            gains[(size_t) b] *= clean / m;
        }
    }

    // ---- de-verb: subtract the running late-field estimate ---------------
    if (deverb > 0.0f && deverbAlpha > 0.0f)
    {
        for (int b = 0; b < numBins; ++b)
        {
            const float m = mag[(size_t) b];
            const float late = revEstimate[(size_t) b] * deverb;
            if (m > 1.0e-9f)
            {
                const float direct = std::max (m - late, m * 0.1f);
                gains[(size_t) b] *= direct / m;
            }
            // update the estimate from this frame's magnitude
            revEstimate[(size_t) b] = deverbAlpha * (revEstimate[(size_t) b] + m);
        }
    }

    // ---- smooth gains across time to avoid per-frame chirping ------------
    for (int b = 0; b < numBins; ++b)
    {
        const float g = 0.6f * gains[(size_t) b] + 0.4f * prevMag[(size_t) b];
        prevMag[(size_t) b] = gains[(size_t) b];
        cplx[b] *= std::clamp (g, 0.0f, 1.0f);
    }

    fft.performRealOnlyInverseTransform (frame.data());
    win.multiplyWithWindowingTable (frame.data(), (size_t) size);

    // Hann at 75% overlap sums to 1.5; normalise on the way out
    constexpr float norm = 2.0f / 3.0f;
    for (int i = 0; i < size; ++i)
        outputRing[(size_t) ((start + i) % (size * 2))] += frame[(size_t) i] * norm;
}

//==============================================================================
// DualCompressor
//==============================================================================
void DualCompressor::prepare (double sampleRate, int)
{
    sr = sampleRate;
    reset();
}

void DualCompressor::reset()
{
    leveller.env = peak.env = 0.0f;
    lastGrDb = 0.0f;
}

void DualCompressor::setParams (const AnalysisResult& a, float amount)
{
    const float amt = std::clamp (amount, 0.0f, 2.0f);

    leveller.threshDb    = a.compLevelThreshDb;
    leveller.ratio       = 1.0f + (a.compLevelRatio - 1.0f) * amt;
    leveller.attackCoef  = timeCoef (a.compLevelAttackMs,  sr);
    leveller.releaseCoef = timeCoef (a.compLevelReleaseMs, sr);

    peak.threshDb    = a.compPeakThreshDb;
    peak.ratio       = 1.0f + (a.compPeakRatio - 1.0f) * amt;
    peak.attackCoef  = timeCoef (a.compPeakAttackMs,  sr);
    peak.releaseCoef = timeCoef (a.compPeakReleaseMs, sr);

    makeupDb = a.makeupGainDb * amt;
}

float DualCompressor::applyStage (Stage& s, float detectorDb) noexcept
{
    // static curve (hard knee), then ballistics on the gain-reduction signal
    float targetGr = 0.0f;
    if (detectorDb > s.threshDb)
        targetGr = (s.threshDb - detectorDb) * (1.0f - 1.0f / s.ratio);

    const float coef = targetGr < s.env ? s.attackCoef : s.releaseCoef;
    s.env = coef * s.env + (1.0f - coef) * targetGr;
    return s.env;
}

void DualCompressor::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const float makeup = dbToGain (makeupDb);

    float maxGr = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        // linked detection across channels keeps the stereo image stable
        float detect = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            detect = std::max (detect, std::abs (buffer.getSample (ch, i)));

        const float detectDb = gainToDb (detect);

        const float gr1 = applyStage (leveller, detectDb);
        const float gr2 = applyStage (peak, detectDb + gr1);
        const float totalGr = gr1 + gr2;

        maxGr = std::min (maxGr, totalGr);
        const float g = dbToGain (totalGr) * makeup;

        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
    }

    lastGrDb = maxGr;
}

//==============================================================================
// DeEsser
//==============================================================================
void DeEsser::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, 512, (juce::uint32) numChannels };

    lowBand.prepare (spec);
    highBand.prepare (spec);
    lowBand.setType  (juce::dsp::LinkwitzRileyFilterType::lowpass);
    highBand.setType (juce::dsp::LinkwitzRileyFilterType::highpass);

    sibBuffer.setSize (numChannels, 4096);
    restBuffer.setSize (numChannels, 4096);
    reset();
}

void DeEsser::reset()
{
    lowBand.reset();
    highBand.reset();
    for (auto& f : bandIsolate) f.reset();
    env = 0.0f;
    lastReductionDb = 0.0f;
}

void DeEsser::setParams (const AnalysisResult& a, float amount)
{
    const float amt = std::clamp (amount, 0.0f, 2.0f);

    // Split just below the measured sibilant centre so the band we duck is the
    // one this particular singer's esses actually live in.
    const float splitHz = std::clamp (a.deEssCentreHz * 0.72f, 2500.0f, 9000.0f);
    lowBand.setCutoffFrequency (splitHz);
    highBand.setCutoffFrequency (splitHz);

    thresholdDb    = a.deEssThresholdDb;
    maxReductionDb = a.deEssMaxReductionDb * amt;

    attackCoef  = timeCoef (0.5f, sr);    // fast enough to catch an ess onset
    releaseCoef = timeCoef (40.0f, sr);
}

void DeEsser::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (sibBuffer.getNumSamples() < numSamples)
    {
        sibBuffer.setSize (numCh, numSamples, false, false, true);
        restBuffer.setSize (numCh, numSamples, false, false, true);
    }

    for (int ch = 0; ch < numCh; ++ch)
    {
        sibBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
        restBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
    }

    { juce::dsp::AudioBlock<float> b (restBuffer);
      juce::dsp::AudioBlock<float> sub = b.getSubBlock (0, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (sub);
      lowBand.process (ctx); }

    { juce::dsp::AudioBlock<float> b (sibBuffer);
      juce::dsp::AudioBlock<float> sub = b.getSubBlock (0, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (sub);
      highBand.process (ctx); }

    float worst = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float detect = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            detect = std::max (detect, std::abs (sibBuffer.getSample (ch, i)));

        const float detectDb = gainToDb (detect);

        float target = 0.0f;
        if (detectDb > thresholdDb)
            target = std::max (maxReductionDb, (thresholdDb - detectDb) * 0.8f);

        const float coef = target < env ? attackCoef : releaseCoef;
        env = coef * env + (1.0f - coef) * target;
        worst = std::min (worst, env);

        const float g = dbToGain (env);
        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i,
                              restBuffer.getSample (ch, i) + sibBuffer.getSample (ch, i) * g);
    }

    lastReductionDb = worst;
}

//==============================================================================
// CleanupChain
//==============================================================================
void CleanupChain::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    channels = juce::jlimit (1, 2, numChannels);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize,
                                  (juce::uint32) channels };

    for (int ch = 0; ch < channels; ++ch)
    {
        hpf[ch].prepare (spec);
        spectral[ch].prepare (sampleRate);
    }

    comp.prepare (sampleRate, channels);
    deEss.prepare (sampleRate, channels);

    limiter.prepare (spec);
    limiter.setThreshold (-0.8f);
    limiter.setRelease (60.0f);

    repairTracker.prepare (sampleRate, 2048);
    monoScratch.setSize (1, maxBlockSize);

    reset();
}

void CleanupChain::reset()
{
    for (int ch = 0; ch < channels; ++ch)
    {
        hpf[ch].reset();
        spectral[ch].reset();
        for (auto& f : surgical[ch])  f.reset();
        for (auto& f : toneBands[ch]) f.reset();
    }
    comp.reset();
    deEss.reset();
    limiter.reset();
    gateEnv = 0.0f;
    gateGain = 1.0f;
}

void CleanupChain::applyAnalysis (const AnalysisResult& a)
{
    analysis = a;
    haveAnalysis = a.valid;
    updateFilters();
}

void CleanupChain::updateFilters()
{
    if (! haveAnalysis) return;

    const float eqAmt = std::clamp (trims.eqAmount, 0.0f, 2.0f);
    const float clAmt = std::clamp (trims.cleanupAmount, 0.0f, 2.0f);

    for (int ch = 0; ch < channels; ++ch)
    {
        *hpf[ch].coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, analysis.highPassHz, 0.707f);

        // surgical notches from the resonance detector
        surgical[ch].clear();
        for (const auto& r : analysis.resonances)
        {
            juce::dsp::IIR::Filter<float> f;
            f.coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                                 sr, r.frequencyHz, r.q, dbToGain (r.gainDb * eqAmt));
            f.prepare ({ sr, 512, 1 });
            surgical[ch].push_back (std::move (f));
        }

        // tone match: one peaking filter per 1/3-octave band that needs a move
        toneBands[ch].clear();
        for (int b = 0; b < numToneBands; ++b)
        {
            const float g = analysis.toneMatchDb[(size_t) b] * eqAmt;
            if (std::abs (g) < 0.35f) continue;                 // skip no-ops
            const float f0 = toneBandHz[(size_t) b];
            if (f0 < 30.0f || f0 > sr * 0.45) continue;

            juce::dsp::IIR::Filter<float> f;
            f.coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                                 sr, f0, 1.6f, dbToGain (g));
            f.prepare ({ sr, 512, 1 });
            toneBands[ch].push_back (std::move (f));
        }

        spectral[ch].setDenoiseAmount (bypass.deNoise ? 0.0f
                                        : std::clamp (analysis.denoiseAmount * clAmt, 0.0f, 1.0f));
        spectral[ch].setDeverbAmount  (bypass.deVerb ? 0.0f
                                        : std::clamp (analysis.deverbAmount * clAmt, 0.0f, 1.0f));
        spectral[ch].setDeverbDecay   (analysis.rt60Seconds);
        spectral[ch].setResonanceDepth (bypass.resonance ? 0.0f : 0.55f * eqAmt);
    }

    comp.setParams (analysis, bypass.compressor ? 0.0f : trims.compAmount);
    deEss.setParams (analysis, bypass.deEss ? 0.0f : trims.deEssAmount);

    gateThreshLin   = dbToGain (analysis.gateThresholdDb);
    gateRangeLin    = dbToGain (analysis.gateRangeDb);
    gateAttackCoef  = timeCoef (analysis.gateAttackMs,  sr);
    gateReleaseCoef = timeCoef (analysis.gateReleaseMs, sr);
}

int CleanupChain::getLatencySamples() const noexcept
{
    return haveAnalysis ? SpectralEngine::size : 0;
}

void CleanupChain::process (juce::AudioBuffer<float>& buffer)
{
    if (! haveAnalysis)
        return;

    const int numSamples = buffer.getNumSamples();
    const int numCh = std::min (channels, buffer.getNumChannels());

    // 1. high-pass, placed below this singer's lowest sung note
    if (! bypass.highPass)
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = hpf[ch].processSample (d[i]);
        }

    // 2/3/7. de-noise + de-verb + resonance suppression share one STFT
    for (int ch = 0; ch < numCh; ++ch)
        spectral[ch].process (buffer.getWritePointer (ch), numSamples);

    // 4. gate, threshold set from the measured noise floor
    if (! bypass.gate)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float detect = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                detect = std::max (detect, std::abs (buffer.getSample (ch, i)));

            const float target = detect > gateThreshLin ? 1.0f : gateRangeLin;
            const float coef = target > gateEnv ? gateAttackCoef : gateReleaseCoef;
            gateEnv = coef * gateEnv + (1.0f - coef) * target;

            for (int ch = 0; ch < numCh; ++ch)
                buffer.setSample (ch, i, buffer.getSample (ch, i) * gateEnv);
        }
        gateGain = gateEnv;
    }

    // 5. surgical notches
    if (! bypass.surgicalEq)
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (auto& f : surgical[ch])
                for (int i = 0; i < numSamples; ++i)
                    d[i] = f.processSample (d[i]);
        }

    // 6. two-stage compression
    if (! bypass.compressor)
        comp.process (buffer);

    // 8. de-ess AFTER compression, BEFORE any additive HF
    if (! bypass.deEss)
        deEss.process (buffer);

    // 9. tone match to the universal target curve
    if (! bypass.toneMatch)
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (auto& f : toneBands[ch])
                for (int i = 0; i < numSamples; ++i)
                    d[i] = f.processSample (d[i]);
        }

    // 11. true-peak safety limiter
    if (! bypass.limiter)
    {
        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        limiter.process (ctx);
    }
}

} // namespace listenator
