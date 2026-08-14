#include "CleanupChain.h"
#include <cmath>
#include <algorithm>

namespace listenator
{

namespace
{
    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }
    inline float gainToDb (float g)  noexcept { return g > 1.0e-9f ? 20.0f * std::log10 (g) : -180.0f; }

    inline float timeCoef (float ms, double sr) noexcept
    {
        if (ms <= 0.0f) return 0.0f;
        return std::exp (-1.0f / (float) (0.001 * ms * sr));
    }

    /** Q for a peaking filter whose -3 dB bandwidth spans `octaves`.
        A 1/3-octave band needs Q ~ 4.3; using anything near 1 makes each
        filter far wider than its band and the 31 of them stack into a curve
        several times the intended depth. */
    inline float qForBandwidth (float octaves) noexcept
    {
        const float p = std::pow (2.0f, octaves);
        return std::sqrt (p) / (p - 1.0f);
    }
}

//==============================================================================
// SpectralEngine
//==============================================================================
void SpectralEngine::prepare (double sampleRate)
{
    sr = sampleRate;
    ringLen = size * 4;
    mask    = ringLen - 1;

    const size_t numBins = (size_t) size / 2 + 1;

    inputRing.assign  ((size_t) ringLen, 0.0f);
    outputRing.assign ((size_t) ringLen, 0.0f);
    frame.assign      ((size_t) size * 2, 0.0f);

    noiseMin.assign    (numBins, 1.0e9f);

    // ~64 ms of history: long enough that the delayed frame is genuinely past
    // excitation rather than the same syllable.
    historyDelay = juce::jlimit (4, 40, (int) (0.064 * sampleRate / hop));
    magHistory.assign ((size_t) historyDelay + 1, std::vector<float> (numBins, 0.0f));
    historyPos = 0;
    prevGain.assign    (numBins, 1.0f);
    mag.assign         (numBins, 0.0f);
    env.assign         (numBins, 0.0f);
    gains.assign       (numBins, 1.0f);

    calibrateOla();
    reset();
}

void SpectralEngine::reset()
{
    std::fill (inputRing.begin(),  inputRing.end(),  0.0f);
    std::fill (outputRing.begin(), outputRing.end(), 0.0f);
    std::fill (noiseMin.begin(), noiseMin.end(), 1.0e9f);
    std::fill (prevGain.begin(), prevGain.end(), 1.0f);
    pos = 0;
    samplesUntilFrame = hop;
}

/** Hann-squared at 75% overlap should sum to 1.5, but that assumes a
    particular FFT normalisation convention. Measuring the real round-trip
    removes the assumption -- and an error here is a flat level offset on
    everything the STFT touches, which is easy to mistake for a DSP problem
    elsewhere. */
void SpectralEngine::calibrateOla()
{
    const int testLen = size * 8;
    std::vector<float> in ((size_t) testLen), out ((size_t) testLen, 0.0f);
    std::vector<float> f ((size_t) size * 2);

    juce::Random rng (1234);
    for (auto& v : in) v = rng.nextFloat() * 2.0f - 1.0f;

    for (int start = 0; start + size <= testLen; start += hop)
    {
        std::fill (f.begin(), f.end(), 0.0f);
        std::copy (in.begin() + start, in.begin() + start + size, f.begin());

        win.multiplyWithWindowingTable (f.data(), (size_t) size);
        fft.performRealOnlyForwardTransform (f.data(), true);
        fft.performRealOnlyInverseTransform (f.data());
        win.multiplyWithWindowingTable (f.data(), (size_t) size);

        for (int i = 0; i < size; ++i)
            out[(size_t) (start + i)] += f[(size_t) i];
    }

    // steady state only: the first and last frame are partially overlapped
    double si = 0.0, so = 0.0;
    for (int i = size; i < testLen - size; ++i)
    {
        si += (double) in[(size_t) i]  * in[(size_t) i];
        so += (double) out[(size_t) i] * out[(size_t) i];
    }

    olaNorm = (so > 1.0e-12) ? (float) std::sqrt (si / so) : 2.0f / 3.0f;
}

void SpectralEngine::setHarmonicSpacing (float f0Hz) noexcept
{
    // Span at least four harmonics of the detected fundamental, so the
    // envelope tracks the spectral shape rather than the comb.
    if (f0Hz <= 20.0f) { minEnvSpanBins = 8; return; }
    const float binHz = (float) (sr / size);
    minEnvSpanBins = juce::jlimit (4, size / 8, (int) (4.0f * f0Hz / binHz));
}

void SpectralEngine::setDeverbDecay (float rt60) noexcept
{
    if (rt60 <= 0.01f) { deverbDecayPerHop = 0.0f; return; }

    // How far the late field has decayed across the history delay.
    const float delaySeconds = (float) (historyDelay * hop) / (float) sr;
    deverbDecayPerHop = juce::jlimit (0.05f, 0.95f,
                                      std::pow (10.0f, -3.0f * delaySeconds / rt60));
}

void SpectralEngine::setTailRatio (float ratio) noexcept
{
    // Direct-to-reverberant scaling for the delayed estimate.
    deverbGamma = juce::jlimit (0.0f, 0.9f, ratio);
}

void SpectralEngine::process (float* block, int numSamples)
{
    if (! isActive())
        return;

    for (int n = 0; n < numSamples; ++n)
    {
        inputRing[(size_t) pos] = block[n];

        // Output trails the input by one frame; clear each slot after reading
        // so it is ready to accumulate again.
        const int readIdx = (pos - size) & mask;
        block[n] = outputRing[(size_t) readIdx];
        outputRing[(size_t) readIdx] = 0.0f;

        pos = (pos + 1) & mask;

        if (--samplesUntilFrame <= 0)
        {
            samplesUntilFrame = hop;
            processFrame();
        }
    }
}

void SpectralEngine::processFrame()
{
    // Analyse the most recent `size` input samples.
    const int start = (pos - size) & mask;

    for (int i = 0; i < size; ++i)
        frame[(size_t) i] = inputRing[(size_t) ((start + i) & mask)];
    std::fill (frame.begin() + size, frame.end(), 0.0f);

    win.multiplyWithWindowingTable (frame.data(), (size_t) size);
    fft.performRealOnlyForwardTransform (frame.data(), true);

    auto* cplx = reinterpret_cast<juce::dsp::Complex<float>*> (frame.data());
    const int numBins = size / 2 + 1;

    for (int b = 0; b < numBins; ++b)
    {
        mag[(size_t) b] = std::abs (cplx[b]);
        gains[(size_t) b] = 1.0f;
    }

    // ---- resonance suppression: duck bins above their local envelope -------
    if (resonanceDepth > 0.0f)
    {
        // The envelope has to span SEVERAL harmonics. A window narrower than
        // the harmonic spacing makes every harmonic of a voiced note look like
        // a resonance, and the suppressor then flattens the singer's own
        // harmonic series -- which is most of the voice.
        for (int b = 1; b < numBins; ++b)
        {
            const int span = juce::jmax (minEnvSpanBins, b / 2);   // >= 1 octave
            const int a = juce::jmax (1, b - span);
            const int c = juce::jmin (numBins - 1, b + span);

            float sum = 0.0f;
            for (int j = a; j <= c; ++j) sum += mag[(size_t) j];
            env[(size_t) b] = sum / (float) (c - a + 1);
        }

        // Harmonics still sit above a smoothed envelope by a few dB, so the
        // trigger point has to clear that before anything is called resonant.
        constexpr float kResonanceThreshDb = 9.0f;

        for (int b = 1; b < numBins; ++b)
        {
            if (env[(size_t) b] <= 1.0e-9f) continue;
            const float excessDb = gainToDb (mag[(size_t) b] / env[(size_t) b]);
            if (excessDb > kResonanceThreshDb)
                gains[(size_t) b] *= dbToGain (juce::jmax (
                    -(excessDb - kResonanceThreshDb) * resonanceDepth, -12.0f));
        }
    }

    // ---- noise floor, tracked in THIS FFT's own units ----------------------
    // Minimum statistics per bin. Learning the floor here rather than importing
    // a figure measured with a different FFT size and window avoids having to
    // reconcile two magnitude scales -- a conversion that is easy to get wrong
    // by tens of dB and silently guts the signal.
    for (int b = 0; b < numBins; ++b)
    {
        const float m = mag[(size_t) b];
        if (m < noiseMin[(size_t) b])
            noiseMin[(size_t) b] = 0.7f * noiseMin[(size_t) b] + 0.3f * m;
        else
            noiseMin[(size_t) b] *= 1.0004f;    // creep up so it can't stick low
    }

    // ---- de-noise: spectral subtraction against the tracked floor ----------
    if (denoise > 0.0f)
    {
        const float over = 1.0f + 1.5f * denoise;    // over-subtraction factor
        for (int b = 0; b < numBins; ++b)
        {
            const float m = mag[(size_t) b];
            if (m <= 1.0e-9f) continue;
            // A spectral floor rather than a hard zero: full subtraction is
            // what turns residual noise into musical-noise chirping.
            const float clean = juce::jmax (m - noiseMin[(size_t) b] * over, m * 0.1f);
            gains[(size_t) b] *= clean / m;
        }
    }

    // ---- de-verb: late field estimated from a DELAYED spectrum -------------
    // What is still ringing now came from excitation ~64 ms ago, decayed by the
    // room. Estimating it from the CURRENT frame's own envelope is what made an
    // earlier version subtract the direct sound: on continuous delivery the
    // envelope and the signal are the same thing.
    if (deverb > 0.0f && deverbGamma > 0.0f)
    {
        const auto& past = magHistory[(size_t) ((historyPos + 1) % (int) magHistory.size())];

        for (int b = 0; b < numBins; ++b)
        {
            const float m = mag[(size_t) b];
            if (m <= 1.0e-9f) continue;

            const float late = past[(size_t) b] * deverbGamma * deverbDecayPerHop;

            // Never take out more than half the bin: over-subtraction on a
            // dense mid-word spectrum is what produces the watery artifact.
            const float subtract = juce::jmin (late * deverb, m * 0.5f);
            gains[(size_t) b] *= juce::jmax (m - subtract, m * 0.3f) / m;
        }
    }

    // push this frame into the history ring
    {
        historyPos = (historyPos + 1) % (int) magHistory.size();
        auto& slot = magHistory[(size_t) historyPos];
        for (int b = 0; b < numBins; ++b) slot[(size_t) b] = mag[(size_t) b];
    }

    // ---- smooth gains over time so bins don't chirp frame to frame ---------
    for (int b = 0; b < numBins; ++b)
    {
        const float g = 0.6f * gains[(size_t) b] + 0.4f * prevGain[(size_t) b];
        prevGain[(size_t) b] = gains[(size_t) b];
        cplx[b] *= juce::jlimit (0.0f, 1.0f, g);
    }

    fft.performRealOnlyInverseTransform (frame.data());
    win.multiplyWithWindowingTable (frame.data(), (size_t) size);

    // Overlap-add AHEAD of the read pointer. The read point is `pos - size`,
    // so writing at `pos` lands a full frame in the future and nothing is lost.
    for (int i = 0; i < size; ++i)
        outputRing[(size_t) ((pos + i) & mask)] += frame[(size_t) i] * olaNorm;
}

//==============================================================================
// DualCompressor
//==============================================================================
void DualCompressor::prepare (double sampleRate, int)
{
    sr = sampleRate;
    rmsCoef = timeCoef (12.0f, sampleRate);   // ~12 ms RMS window
    reset();
}

void DualCompressor::reset()
{
    leveller.env = peak.env = 0.0f;
    rmsSquared = 0.0f;
    lastGrDb = 0.0f;
}

void DualCompressor::setParams (const AnalysisResult& a, float amount)
{
    const float amt = juce::jlimit (0.0f, 2.0f, amount);

    leveller.threshDb    = a.compLevelThreshDb;
    leveller.ratio       = 1.0f + (a.compLevelRatio - 1.0f) * amt;
    leveller.kneeDb      = 8.0f;
    leveller.attackCoef  = timeCoef (a.compLevelAttackMs,  sr);
    leveller.releaseCoef = timeCoef (a.compLevelReleaseMs, sr);

    peak.threshDb    = a.compPeakThreshDb;
    peak.ratio       = 1.0f + (a.compPeakRatio - 1.0f) * amt;
    peak.kneeDb      = 4.0f;
    peak.attackCoef  = timeCoef (a.compPeakAttackMs,  sr);
    peak.releaseCoef = timeCoef (a.compPeakReleaseMs, sr);

    makeupDb = a.makeupGainDb * amt;
}

/** Soft-knee static curve. Returns gain reduction in dB (<= 0). */
float DualCompressor::curve (const Stage& s, float detectorDb) noexcept
{
    const float over = detectorDb - s.threshDb;
    const float slope = 1.0f - 1.0f / s.ratio;

    if (over <= -s.kneeDb * 0.5f)
        return 0.0f;

    if (over >= s.kneeDb * 0.5f)
        return -slope * over;

    // quadratic interpolation across the knee
    const float x = over + s.kneeDb * 0.5f;
    return -slope * x * x / (2.0f * s.kneeDb);
}

float DualCompressor::applyStage (Stage& s, float detectorDb) noexcept
{
    const float target = curve (s, detectorDb);
    const float coef = target < s.env ? s.attackCoef : s.releaseCoef;
    s.env = coef * s.env + (1.0f - coef) * target;
    return s.env;
}

void DualCompressor::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    const float makeup = dbToGain (makeupDb);

    float worstGr = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        // Channel-linked detection keeps the stereo image stable.
        float peakAbs = 0.0f, sumSq = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float x = buffer.getSample (ch, i);
            peakAbs = juce::jmax (peakAbs, std::abs (x));
            sumSq += x * x;
        }
        sumSq /= (float) juce::jmax (1, numCh);

        rmsSquared = rmsCoef * rmsSquared + (1.0f - rmsCoef) * sumSq;

        // Stage 1 is threshold-matched to integrated loudness, so it must see
        // an RMS level. Feeding it peak would trigger a full crest factor early.
        const float rmsDb  = 10.0f * std::log10 (juce::jmax (rmsSquared, 1.0e-12f));
        const float peakDb = gainToDb (peakAbs);

        const float gr1 = applyStage (leveller, rmsDb);
        const float gr2 = applyStage (peak, peakDb + gr1);
        const float totalGr = gr1 + gr2;

        worstGr = juce::jmin (worstGr, totalGr);
        const float g = dbToGain (totalGr) * makeup;

        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
    }

    lastGrDb = worstGr;
}

//==============================================================================
// DeClipper
//==============================================================================
void DeClipper::prepare (double, int) { reset(); }
void DeClipper::reset() { repaired = 0; }

void DeClipper::process (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* d = buffer.getWritePointer (ch);

        for (int i = 1; i < n - 1; ++i)
        {
            if (std::abs (d[i]) < threshold) continue;

            // find the extent of the flat top
            int end = i;
            while (end + 1 < n - 1 && std::abs (d[end + 1]) >= threshold) ++end;

            const int run = end - i + 1;
            if (run > maxRun) { i = end; continue; }   // too long to guess at

            const int a = i - 1, b = std::min (end + 1, n - 1);
            const float ya = d[a], yb = d[b];
            const float sign = ya >= 0.0f ? 1.0f : -1.0f;

            // Arc over the gap rather than a straight line: a clipped peak was
            // going somewhere above full scale, and a chord across it leaves an
            // audible flat spot in its place.
            const float bulge = sign * threshold * 0.18f * (float) run / (float) maxRun;

            for (int k = i; k <= end; ++k)
            {
                const float t = (float) (k - a) / (float) std::max (1, b - a);
                const float lin = ya + (yb - ya) * t;
                const float arch = std::sin (t * juce::MathConstants<float>::pi);
                d[k] = juce::jlimit (-1.0f, 1.0f, lin + bulge * arch);
            }

            repaired += run;
            i = end;
        }
    }
}

//==============================================================================
// PlosiveGuard
//==============================================================================
void PlosiveGuard::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize,
                                  (juce::uint32) numChannels };
    lowBand.prepare (spec);
    highBand.prepare (spec);
    lowBand.setType  (juce::dsp::LinkwitzRileyFilterType::lowpass);
    highBand.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
    setCornerHz (150.0f);

    lowBuf.setSize (numChannels, maxBlockSize);
    highBuf.setSize (numChannels, maxBlockSize);

    attackCoef  = timeCoef (1.5f,  sampleRate);   // must catch the leading edge
    releaseCoef = timeCoef (90.0f, sampleRate);
    fastCoef    = timeCoef (2.0f,   sampleRate);
    slowCoef    = timeCoef (180.0f, sampleRate);
    reset();
}

void PlosiveGuard::setCornerHz (float hz) noexcept
{
    const float f = juce::jlimit (80.0f, 250.0f, hz);
    lowBand.setCutoffFrequency (f);
    highBand.setCutoffFrequency (f);
}

void PlosiveGuard::reset()
{
    lowBand.reset(); highBand.reset();
    lowBuf.clear(); highBuf.clear();
    lfFast = lfSlow = hfFast = hfSlow = 0.0f;
    gain = 1.0f;
    lastReductionDb = 0.0f;
    peakLfBoost = 0.0f;
}

void PlosiveGuard::process (juce::AudioBuffer<float>& buffer)
{
    if (amount <= 0.0f) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), lowBuf.getNumChannels());
    const int n = juce::jmin (buffer.getNumSamples(), lowBuf.getNumSamples());
    if (numCh <= 0 || n <= 0) return;

    for (int ch = 0; ch < numCh; ++ch)
    {
        lowBuf.copyFrom  (ch, 0, buffer, ch, 0, n);
        highBuf.copyFrom (ch, 0, buffer, ch, 0, n);
    }

    { juce::dsp::AudioBlock<float> b (lowBuf);
      auto sub = b.getSubBlock (0, (size_t) n);
      juce::dsp::ProcessContextReplacing<float> ctx (sub); lowBand.process (ctx); }
    { juce::dsp::AudioBlock<float> b (highBuf);
      auto sub = b.getSubBlock (0, (size_t) n);
      juce::dsp::ProcessContextReplacing<float> ctx (sub); highBand.process (ctx); }

    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float lo = 0.0f, hi = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            lo = juce::jmax (lo, std::abs (lowBuf.getSample (ch, i)));
            hi = juce::jmax (hi, std::abs (highBuf.getSample (ch, i)));
        }

        lfFast = fastCoef * lfFast + (1.0f - fastCoef) * lo;
        lfSlow = slowCoef * lfSlow + (1.0f - slowCoef) * lo;
        hfFast = fastCoef * hfFast + (1.0f - fastCoef) * hi;
        hfSlow = slowCoef * hfSlow + (1.0f - slowCoef) * hi;

        // How far each band has jumped above its own recent average.
        const float lfBoost = lfFast / juce::jmax (lfSlow, 1.0e-5f);
        const float hfBoost = hfFast / juce::jmax (hfSlow, 1.0e-5f);

        // A sung note lifts both bands together. A plosive, a footstep or a
        // knock on the boom arm lifts only the bottom.
        float target = 1.0f;
        if (lo > 3.0e-4f && lfBoost > 2.2f && lfBoost > hfBoost * 1.7f)
        {
            peakLfBoost = juce::jmax (peakLfBoost, lfBoost);
            const float severity = juce::jlimit (0.0f, 1.0f, (lfBoost - 2.2f) / 5.0f);
            target = juce::jmax (0.10f, 1.0f - severity * 0.9f * amount);
        }

        const float coef = target < gain ? attackCoef : releaseCoef;
        gain = coef * gain + (1.0f - coef) * target;
        worst = juce::jmin (worst, gainToDb (gain));

        // duck only the low band; the rest of the voice passes untouched
        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i,
                              highBuf.getSample (ch, i) + lowBuf.getSample (ch, i) * gain);
    }

    lastReductionDb = worst;
}

//==============================================================================
// BrickwallLimiter
//==============================================================================
void BrickwallLimiter::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    numCh = juce::jmax (1, numChannels);
    lookahead = juce::jmax (8, (int) (0.0015 * sampleRate));   // 1.5 ms

    delayLine.setSize (numCh, lookahead + maxBlockSize + 4);

    // Attack reaches the target within the lookahead window, so the gain is
    // already down by the time the offending sample arrives at the output.
    attackCoef  = std::exp (-3.0f / (float) lookahead);
    releaseCoef = timeCoef (80.0f, sampleRate);

    reset();
}

void BrickwallLimiter::reset()
{
    delayLine.clear();
    writeIdx = 0;
    gain = 1.0f;
}

void BrickwallLimiter::setThresholdDb (float db) noexcept
{
    thresholdLin = dbToGain (db);
}

void BrickwallLimiter::process (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const int ch = juce::jmin (numCh, buffer.getNumChannels());
    const int len = delayLine.getNumSamples();
    if (len <= 0) return;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < ch; ++c)
            peak = juce::jmax (peak, std::abs (buffer.getSample (c, i)));

        // Below threshold the target is exactly 1.0, so the limiter is
        // transparent rather than merely gentle.
        const float target = peak > thresholdLin ? thresholdLin / peak : 1.0f;

        const float coef = target < gain ? attackCoef : releaseCoef;
        gain = coef * gain + (1.0f - coef) * target;

        const int readIdx = (writeIdx + 1) % len;

        for (int c = 0; c < ch; ++c)
        {
            const float delayed = delayLine.getSample (c, readIdx);
            delayLine.setSample (c, writeIdx, buffer.getSample (c, i));

            // Hard clip catches whatever the smoothed gain overshoots; it only
            // ever engages on the residual, so it stays inaudible.
            buffer.setSample (c, i, juce::jlimit (-thresholdLin, thresholdLin,
                                                  delayed * gain));
        }

        writeIdx = readIdx;
    }
}

//==============================================================================
// DeEsser
//==============================================================================
void DeEsser::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize,
                                  (juce::uint32) numChannels };

    lowBand.prepare (spec);
    highBand.prepare (spec);
    lowBand.setType  (juce::dsp::LinkwitzRileyFilterType::lowpass);
    highBand.setType (juce::dsp::LinkwitzRileyFilterType::highpass);

    // Sized once, here: resizing inside process() would allocate on the
    // audio thread.
    sibBuffer.setSize (numChannels, maxBlockSize);
    restBuffer.setSize (numChannels, maxBlockSize);
    reset();
}

void DeEsser::reset()
{
    lowBand.reset();
    highBand.reset();
    sibBuffer.clear();
    restBuffer.clear();
    env = 0.0f;
    lastReductionDb = 0.0f;
}

void DeEsser::setParams (const AnalysisResult& a, float amount)
{
    const float amt = juce::jlimit (0.0f, 2.0f, amount);

    // Split just below the measured sibilant centre, so the band that ducks is
    // the one this particular singer's esses actually occupy.
    const float splitHz = juce::jlimit (2500.0f, 9000.0f, a.deEssCentreHz * 0.72f);
    lowBand.setCutoffFrequency (splitHz);
    highBand.setCutoffFrequency (splitHz);

    thresholdDb    = a.deEssThresholdDb;
    maxReductionDb = a.deEssMaxReductionDb * amt;
    ratio          = 4.0f;

    attackCoef  = timeCoef (0.5f,  sr);   // fast enough to catch an ess onset
    releaseCoef = timeCoef (40.0f, sr);
}

void DeEsser::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = juce::jmin (buffer.getNumChannels(), sibBuffer.getNumChannels());
    const int numSamples = juce::jmin (buffer.getNumSamples(), sibBuffer.getNumSamples());
    if (numCh <= 0 || numSamples <= 0) return;

    for (int ch = 0; ch < numCh; ++ch)
    {
        sibBuffer.copyFrom  (ch, 0, buffer, ch, 0, numSamples);
        restBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
    }

    { juce::dsp::AudioBlock<float> b (restBuffer);
      auto sub = b.getSubBlock (0, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (sub);
      lowBand.process (ctx); }

    { juce::dsp::AudioBlock<float> b (sibBuffer);
      auto sub = b.getSubBlock (0, (size_t) numSamples);
      juce::dsp::ProcessContextReplacing<float> ctx (sub);
      highBand.process (ctx); }

    float worst = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float detect = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            detect = juce::jmax (detect, std::abs (sibBuffer.getSample (ch, i)));

        const float detectDb = gainToDb (detect);

        float target = 0.0f;
        if (detectDb > thresholdDb)
            target = juce::jmax (maxReductionDb,
                                 -(detectDb - thresholdDb) * (1.0f - 1.0f / ratio));

        const float coef = target < env ? attackCoef : releaseCoef;
        env = coef * env + (1.0f - coef) * target;
        worst = juce::jmin (worst, env);

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
    maxBlock = juce::jmax (32, maxBlockSize);

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock,
                                  (juce::uint32) channels };

    deClip.prepare (sampleRate, channels);
    plosive.prepare (sampleRate, maxBlock, channels);

    for (int ch = 0; ch < channels; ++ch)
    {
        hpf[ch].prepare (spec);
        spectral[ch].prepare (sampleRate);
        for (auto& f : surgical[ch])  f.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
        for (auto& f : toneBands[ch]) f.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
    }

    comp.prepare (sampleRate, channels);
    deEss.prepare (sampleRate, maxBlock, channels);

    // Cleanup-half pitch repair: slow, wide dead zone, partial strength. This
    // is intonation repair, not an effect -- it should never be audible as one.
    repair.prepare (sampleRate, maxBlock);
    repair.setRetuneMs (220.0f);
    repair.setStrength (0.5f);
    repair.setDeadZoneCents (22.0f);

    limiter.prepare (sampleRate, maxBlock, channels);
    limiter.setThresholdDb (-0.8f);

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
    deClip.reset();
    plosive.reset();
    comp.reset();
    deEss.reset();
    repair.reset();
    limiter.reset();
    gateEnv = 1.0f;
    gateGain = 1.0f;
    gateOpen = false;
}

void CleanupChain::applyAnalysis (const AnalysisResult& a)
{
    analysis = a;
    haveAnalysis = a.valid;
    updateFilters();
}

void CleanupChain::setBypass (const CleanupBypass& b)
{
    if (b == bypass) return;          // recomputing coefficients every block
    bypass = b;                       // would be pointless work on the audio thread
    if (haveAnalysis) updateFilters();
}

void CleanupChain::setTrims (const CleanupTrims& t)
{
    if (t == trims) return;
    trims = t;
    if (haveAnalysis) updateFilters();
}

void CleanupChain::updateFilters()
{
    if (! haveAnalysis) return;

    const float eqAmt = juce::jlimit (0.0f, 2.0f, trims.eqAmount);
    const float clAmt = juce::jlimit (0.0f, 2.0f, trims.cleanupAmount);

    // A 1/3-octave band: the filters must be this narrow or 31 of them stack
    // into a curve several times deeper than the one we measured.
    const float toneQ = qForBandwidth (1.0f / 3.0f);

    for (int ch = 0; ch < channels; ++ch)
    {
        *hpf[ch].coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, analysis.highPassHz, 0.707f);

        // Fixed-size arrays with a live count: updateFilters can be reached
        // from the audio thread when a trim knob moves, so it must not allocate.
        numSurgical = 0;
        for (const auto& r : analysis.resonances)
        {
            if (numSurgical >= maxSurgical) break;
            *surgical[ch][(size_t) numSurgical].coefficients =
                *juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                     sr, r.frequencyHz, r.q, dbToGain (r.gainDb * eqAmt));
            ++numSurgical;
        }

        numTone = 0;
        for (int b = 0; b < numToneBands; ++b)
        {
            // Use the solved gains, not the raw target: neighbouring 1/3-octave
            // filters overlap, so the two differ by several dB.
            const float g = analysis.toneFilterGainDb[(size_t) b] * eqAmt;
            if (std::abs (g) < 0.2f) continue;
            const float f0 = toneBandHz[(size_t) b];
            if (f0 < 30.0f || f0 > sr * 0.45) continue;

            *toneBands[ch][(size_t) numTone].coefficients =
                *juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                     sr, f0, toneQ, dbToGain (g));
            ++numTone;
        }

        spectral[ch].setDenoiseAmount (bypass.deNoise ? 0.0f : analysis.denoiseAmount * clAmt);
        spectral[ch].setDeverbAmount  (bypass.deVerb  ? 0.0f : analysis.deverbAmount  * clAmt);
        spectral[ch].setDeverbDecay   (analysis.rt60Seconds);
        spectral[ch].setTailRatio     (analysis.reverbRatio);
        spectral[ch].setHarmonicSpacing (analysis.medianF0Hz);
        spectral[ch].setResonanceDepth (bypass.resonance ? 0.0f : 0.55f * eqAmt);
    }

    // The guard's corner follows the voice: cutting at a fixed 150 Hz would
    // reach into the chest register of a low male voice.
    // Sit the split below the fundamental: plosive energy peaks well under it,
    // and crossing over on top of the voice makes the two indistinguishable.
    plosive.setCornerHz (juce::jlimit (80.0f, 160.0f,
                                       analysis.medianF0Hz > 60.0f
                                           ? analysis.medianF0Hz * 0.75f : 120.0f));
    plosive.setAmount (bypass.plosive ? 0.0f : trims.cleanupAmount);

    comp.setParams  (analysis, bypass.compressor ? 0.0f : trims.compAmount);
    deEss.setParams (analysis, bypass.deEss ? 0.0f : trims.deEssAmount);

    gateOpenLin     = dbToGain (analysis.gateThresholdDb);
    gateCloseLin    = dbToGain (analysis.gateThresholdDb - 6.0f);   // 6 dB hysteresis
    gateRangeLin    = dbToGain (analysis.gateRangeDb);
    gateAttackCoef  = timeCoef (analysis.gateAttackMs,  sr);
    gateReleaseCoef = timeCoef (analysis.gateReleaseMs, sr);
}

int CleanupChain::getLatencySamples() const noexcept
{
    if (! haveAnalysis) return 0;

    int latency = 0;
    if (! bypass.deNoise || ! bypass.deVerb || ! bypass.resonance)
        latency += SpectralEngine::getLatencySamples();
    if (! bypass.pitchRepair)
        latency += repair.getLatencySamples();
    if (! bypass.limiter)
        latency += limiter.getLatencySamples();
    return latency;
}

void CleanupChain::process (juce::AudioBuffer<float>& buffer)
{
    if (! haveAnalysis)
        return;

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (channels, buffer.getNumChannels());

    // 1. repair flat-topped peaks before anything measures them
    if (! bypass.deClip)
        deClip.process (buffer);

    // 2. duck low-frequency bursts that aren't the voice
    if (! bypass.plosive)
        plosive.process (buffer);

    // 3. high-pass, below this singer's lowest sung note
    if (! bypass.highPass)
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                d[i] = hpf[ch].processSample (d[i]);
        }

    // 2. de-noise + de-verb + resonance suppression, sharing one STFT
    for (int ch = 0; ch < numCh; ++ch)
        spectral[ch].process (buffer.getWritePointer (ch), numSamples);

    // 3. gate, threshold from the measured noise floor, with hysteresis
    if (! bypass.gate)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float detect = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                detect = juce::jmax (detect, std::abs (buffer.getSample (ch, i)));

            // Separate open/close thresholds: a single one chatters when the
            // signal sits right on it.
            if (! gateOpen && detect > gateOpenLin)  gateOpen = true;
            else if (gateOpen && detect < gateCloseLin) gateOpen = false;

            const float target = gateOpen ? 1.0f : gateRangeLin;
            const float coef = target > gateEnv ? gateAttackCoef : gateReleaseCoef;
            gateEnv = coef * gateEnv + (1.0f - coef) * target;

            for (int ch = 0; ch < numCh; ++ch)
                buffer.setSample (ch, i, buffer.getSample (ch, i) * gateEnv);
        }
        gateGain = gateEnv;
    }

    // 4. surgical notches
    if (! bypass.surgicalEq)
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int k = 0; k < numSurgical; ++k)
                for (int i = 0; i < numSamples; ++i)
                    d[i] = surgical[ch][(size_t) k].processSample (d[i]);
        }

    // 5. two-stage compression
    if (! bypass.compressor)
        comp.process (buffer);

    // 6. de-ess AFTER compression, BEFORE the tone stage's additive HF
    if (! bypass.deEss)
        deEss.process (buffer);

    // 7. tone match to the universal target curve
    if (! bypass.toneMatch)
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int k = 0; k < numTone; ++k)
                for (int i = 0; i < numSamples; ++i)
                    d[i] = toneBands[ch][(size_t) k].processSample (d[i]);
        }

    // 8. transparent intonation repair (mono source drives the chain)
    if (! bypass.pitchRepair)
    {
        repair.process (buffer.getWritePointer (0), numSamples);
        for (int ch = 1; ch < numCh; ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
    }

    // 9. safety limiter
    if (! bypass.limiter)
        limiter.process (buffer);
}

} // namespace listenator
