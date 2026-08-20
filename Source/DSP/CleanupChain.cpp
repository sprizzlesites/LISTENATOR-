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
    lateAccum.assign (numBins, 0.0f);
    historyPos = 0;
    prevGain.assign    (numBins, 1.0f);
    mag.assign         (numBins, 0.0f);
    env.assign         (numBins, 0.0f);
    gains.assign       (numBins, 1.0f);
    smoothGains.assign (numBins, 1.0f);

    calibrateOla();
    reset();
}

void SpectralEngine::reset()
{
    std::fill (inputRing.begin(),  inputRing.end(),  0.0f);
    std::fill (outputRing.begin(), outputRing.end(), 0.0f);
    std::fill (noiseMin.begin(), noiseMin.end(), 1.0e9f);
    std::fill (lateAccum.begin(), lateAccum.end(), 0.0f);
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

    // Decay across ONE HOP, because that is how often the accumulator is
    // advanced. Computing it over the history delay instead (a dozen hops) made
    // the estimate collapse an order of magnitude too fast, so by the time a gap
    // arrived there was nothing left to subtract.
    const float hopSeconds = (float) hop / (float) sr;
    deverbDecayPerHop = juce::jlimit (0.05f, 0.995f,
                                      std::pow (10.0f, -3.0f * hopSeconds / rt60));

    // How deep the per-bin subtraction may go, and it follows the room the
    // OTHER way round from intuition.
    //
    // The depth the stage needs is not the question -- it is the depth the
    // estimate can be trusted to. In a moderate room the late field is sparse
    // enough that a per-bin estimate localises it, and a deep cut tells a tail
    // from a vowel: against a synthetic 0.9 s room, going from half the bin to
    // nine tenths took the tail from untouched to 4 dB down. In a 2 s booth
    // with the singer moving, the field is dense and non-stationary, the
    // per-bin estimate is mostly wrong, and the same depth measured 2 dB WORSE
    // on the tail and 1.9 dB of extra spectral chatter. Deep subtraction is a
    // reward for a reliable estimate, not a response to a bad room.
    deverbMaxCut = juce::jlimit (0.45f, 0.92f, 0.92f - (rt60 - 0.8f) * 0.30f);
}

void SpectralEngine::setTailDb (float directToReverbDb) noexcept
{
    // The reverberant level during delivery, as an amplitude fraction. At
    // -6 dB that is about half the signal, and subtracting half is what
    // actually dries a room out. Passing the decayed tail figure instead makes
    // this 0.18 and the stage does effectively nothing.
    deverbGamma = juce::jlimit (0.05f, 0.75f,
                                dbToGain (juce::jlimit (-24.0f, -3.0f, directToReverbDb)));
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

        int binsThisFrame = 0;
        for (int b = 1; b < numBins; ++b)
        {
            if (env[(size_t) b] <= 1.0e-9f) continue;
            const float excessDb = gainToDb (mag[(size_t) b] / env[(size_t) b]);
            if (excessDb > kResonanceThreshDb)
            {
                const float cut = juce::jmax (
                    -(excessDb - kResonanceThreshDb) * resonanceDepth, -12.0f);
                gains[(size_t) b] *= dbToGain (cut);
                worstResonanceDb = juce::jmin (worstResonanceDb, cut);
                ++binsThisFrame;
            }
        }

        resonanceBins = juce::jmax (resonanceBins, binsThisFrame);
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

        // A real tail is everything the room is still radiating from ALL past
        // excitation, decaying exponentially -- not one delayed frame. A single
        // tap under-estimates a dense passage badly, because the energy still
        // ringing there came from many syllables, not just the last one.
        const float decay = deverbDecayPerHop;
        const float norm  = 1.0f - decay;

        // The cap has to leave room for the stage to DIFFERENTIATE.
        //
        // At half the bin it was binding everywhere: during speech the estimate
        // wants ~40% removed, in a tail it wants ~90%, and a flat 50% ceiling
        // turns both into the same broadband attenuation. Measured against a
        // synthetic 0.9 s room, that version left the tail-to-speech ratio
        // exactly where it found it while costing 2.8 dB of level -- the tone
        // stage then gave the level back and the net effect was nothing.
        //
        // What actually keeps per-bin subtraction from warbling is the gain
        // smoothing across frequency and time further down, not a low ceiling.
        const float maxCut = deverbMaxCut;
        const float floorG = 1.0f - maxCut;

        for (int b = 0; b < numBins; ++b)
        {
            lateAccum[(size_t) b] = decay * lateAccum[(size_t) b] + past[(size_t) b];

            const float m = mag[(size_t) b];
            if (m <= 1.0e-9f) continue;

            const float late = lateAccum[(size_t) b] * norm * deverbGamma;

            // Subtraction runs during sustained delivery too. An earlier
            // version gated it to bins that were falling away from their own
            // recent level, on the theory that only decays contain room -- but
            // the reverberant field is loudest DURING speech, which is exactly
            // where boxiness is heard. Gating it there left the stage doing
            // nothing except in gaps the expander already handles.
            //
            // Onsets are protected instead: a bin rising sharply is new direct
            // sound, and the room has not caught up with it yet.
            const float ref = past[(size_t) b];
            const float rising = ref > 1.0e-9f
                               ? juce::jlimit (0.0f, 1.0f, (m / ref - 1.0f) * 0.7f)
                               : 0.0f;
            const float protect = 1.0f - rising;

            const float subtract = juce::jmin (late * deverb * protect, m * maxCut);
            gains[(size_t) b] *= juce::jmax (m - subtract, m * floorG) / m;
        }
    }

    // push this frame into the history ring
    {
        historyPos = (historyPos + 1) % (int) magHistory.size();
        auto& slot = magHistory[(size_t) historyPos];
        for (int b = 0; b < numBins; ++b) slot[(size_t) b] = mag[(size_t) b];
    }

    // ---- smooth the gain curve ACROSS FREQUENCY ----------------------------
    // Musical noise is isolated bins being attenuated very differently from
    // their neighbours, then flickering between frames -- heard as warbling
    // around the voice. Smoothing over time alone does not fix it, because the
    // discontinuity is along the frequency axis. A few bins of averaging costs
    // nothing in correction and is the standard cure.
    //
    // The window widens with frequency so it stays roughly constant in
    // proportional terms rather than smearing narrow low-frequency detail.
    for (int b = 0; b < numBins; ++b)
    {
        // The window widens with how deeply the stage is cutting. Musical noise
        // is a function of the DEPTH of a per-bin gain, not just its existence:
        // a 3 dB cut that varies between neighbours is inaudible, a 20 dB one
        // warbles. Letting the de-verb cut deep enough to tell a tail from a
        // vowel is only safe if the smoothing scales with it.
        const int extra = juce::jlimit (0, 3, (int) ((deverbMaxCut - 0.5f) * 6.0f));
        const int span = juce::jlimit (1, 9, b / 48 + 1 + extra);
        const int a = juce::jmax (0, b - span);
        const int c = juce::jmin (numBins - 1, b + span);

        float sum = 0.0f;
        for (int j = a; j <= c; ++j) sum += gains[(size_t) j];
        smoothGains[(size_t) b] = sum / (float) (c - a + 1);
    }

    // ---- then smooth over time so bins don't chirp frame to frame ----------
    for (int b = 0; b < numBins; ++b)
    {
        const float tw = 0.55f - 0.20f * juce::jlimit (0.0f, 1.0f,
                                                       (deverbMaxCut - 0.5f) * 2.0f);
        const float g = tw * smoothGains[(size_t) b] + (1.0f - tw) * prevGain[(size_t) b];
        prevGain[(size_t) b] = smoothGains[(size_t) b];
        cplx[b] *= juce::jlimit (0.0f, 1.0f, g);
    }

    fft.performRealOnlyInverseTransform (frame.data());
    win.multiplyWithWindowingTable (frame.data(), (size_t) size);

    // Overlap-add at the positions the frame actually ANALYSED: this frame
    // covered inputs [pos - size, pos), and that is where its synthesis
    // belongs.
    //
    // Writing at `pos` instead -- a whole frame further ahead -- is safe but
    // costs an extra `size` of delay for nothing, which measured as 2048
    // samples of real latency against the 1024 the class reported. The
    // earliest position written here is `pos - size`, which is exactly the
    // next sample the reader will take, and by then every frame that overlaps
    // it (four of them at 75% overlap) has already contributed.
    const int writeStart = (pos - size) & mask;
    for (int i = 0; i < size; ++i)
        outputRing[(size_t) ((writeStart + i) & mask)] += frame[(size_t) i] * olaNorm;
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

    // A phrase-rate stage was tried here and removed. It added 5 dB of gain
    // reduction and tightened the short-term spread by nothing measurable
    // (6.8 -> 7.0 dB): once the take stage has both the quiet and loud passages
    // above its threshold, a second downward compressor scales them together
    // rather than closing the gap between them.
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
void PlosiveGuard::Detector::prepare (double sampleRate) noexcept
{
    // Peak followers, not one-pole smoothers. A 2 ms smoother on a 110 Hz
    // fundamental ripples at the waveform period, so the ratio it produces
    // swings by 6 dB on a perfectly steady note.
    fastAtk = timeCoef (1.0f,   sampleRate);
    fastRel = timeCoef (20.0f,  sampleRate);
    // The slow pair is a BACKGROUND level, so a 60 ms burst must not be able to
    // move it. At a 60 ms attack it caught up with a pop inside five
    // milliseconds and the ratio never rose past 2.9.
    slowAtk = timeCoef (250.0f, sampleRate);
    slowRel = timeCoef (600.0f, sampleRate);
    reset();
}

void PlosiveGuard::Detector::reset() noexcept
{
    lfFast = lfSlow = hfFast = hfSlow = 0.0f;
}

PlosiveGuard::Detector::Result PlosiveGuard::Detector::push (float lo, float hi) noexcept
{
    auto follow = [] (float& state, float x, float atk, float rel)
    {
        const float c = x > state ? atk : rel;
        state = c * state + (1.0f - c) * x;
    };

    follow (lfFast, lo, fastAtk, fastRel);
    follow (lfSlow, lo, slowAtk, slowRel);
    follow (hfFast, hi, fastAtk, fastRel);
    follow (hfSlow, hi, slowAtk, slowRel);

    Result r;
    r.lfBoost = lfFast / juce::jmax (lfSlow, 1.0e-5f);
    r.hfBoost = hfFast / juce::jmax (hfSlow, 1.0e-5f);
    // Floored, or a burst arriving in a gap where the high band has decayed to
    // nothing divides by zero and every one of them reads as maximum severity.
    r.contrast = r.lfBoost / juce::jmax (r.hfBoost, 0.25f);
    return r;
}

void PlosiveGuard::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize,
                                  (juce::uint32) numChannels };
    for (auto* f : { &splitLow, &splitLowHi, &splitUp, &splitUpHi, &subAllpass })
        f->prepare (spec);

    splitLow.setType   (juce::dsp::LinkwitzRileyFilterType::lowpass);
    splitLowHi.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
    splitUp.setType    (juce::dsp::LinkwitzRileyFilterType::lowpass);
    splitUpHi.setType  (juce::dsp::LinkwitzRileyFilterType::highpass);
    subAllpass.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
    setBands (110.0f, 240.0f);

    subBuf.setSize (numChannels, maxBlockSize);
    midBuf.setSize (numChannels, maxBlockSize);
    topBuf.setSize (numChannels, maxBlockSize);

    attackCoef  = timeCoef (1.0f,  sampleRate);   // must catch the leading edge
    releaseCoef = timeCoef (60.0f, sampleRate);
    // A pop is 20-60 ms of blast. Releasing from the first sample lets the duck
    // decay while the burst is still going, which leaves the tail of the pop.
    holdSamples = (int) (0.030 * sampleRate);
    detector.prepare (sampleRate);
    reset();
}

void PlosiveGuard::setBands (float cornerHz, float upperHz) noexcept
{
    const float f1 = juce::jlimit (70.0f, 190.0f, cornerHz);
    const float f2 = juce::jlimit (f1 * 1.4f, 400.0f, upperHz);
    splitLow.setCutoffFrequency   (f1);
    splitLowHi.setCutoffFrequency (f1);
    splitUp.setCutoffFrequency    (f2);
    splitUpHi.setCutoffFrequency  (f2);
    subAllpass.setCutoffFrequency (f2);
}

void PlosiveGuard::setParams (const AnalysisResult& a) noexcept
{
    setBands (a.plosiveCornerHz, a.plosiveUpperHz);
    depthLin    = dbToGain (juce::jlimit (-30.0f, -3.0f, a.plosiveDepthDb));
    sensitivity = juce::jlimit (1.4f, 6.0f, a.plosiveSensitivity);
}

void PlosiveGuard::reset()
{
    splitLow.reset(); splitLowHi.reset(); splitUp.reset(); splitUpHi.reset();
    subAllpass.reset();
    subBuf.clear(); midBuf.clear(); topBuf.clear();
    detector.reset();
    gain = 1.0f;
    holdLeft = 0;
    lastReductionDb = 0.0f;
    peakLfBoost = 0.0f;
}

void PlosiveGuard::process (juce::AudioBuffer<float>& buffer)
{
    if (amount <= 0.0f) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), subBuf.getNumChannels());
    const int n = juce::jmin (buffer.getNumSamples(), subBuf.getNumSamples());
    if (numCh <= 0 || n <= 0) return;

    // sub = below f1, mid = f1..f2, top = above f2
    for (int ch = 0; ch < numCh; ++ch)
    {
        subBuf.copyFrom (ch, 0, buffer, ch, 0, n);
        midBuf.copyFrom (ch, 0, buffer, ch, 0, n);
    }

    auto run = [n] (juce::dsp::LinkwitzRileyFilter<float>& f, juce::AudioBuffer<float>& b)
    {
        juce::dsp::AudioBlock<float> blk (b);
        auto sub = blk.getSubBlock (0, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (sub);
        f.process (ctx);
    };

    run (splitLow,   subBuf);   // < f1
    run (splitLowHi, midBuf);   // > f1
    for (int ch = 0; ch < numCh; ++ch)
        topBuf.copyFrom (ch, 0, midBuf, ch, 0, n);
    run (splitUp,   midBuf);    // f1 .. f2
    run (splitUpHi, topBuf);    // > f2
    run (subAllpass, subBuf);   // phase-align the sub band with the other two

    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float lo = 0.0f, hi = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            // The detector keys off the whole plosive region, not just the sub
            // band: the burst spreads across both, and looking only under f1
            // means the guard fires on whatever the high-pass was about to
            // remove anyway.
            lo = juce::jmax (lo, std::abs (subBuf.getSample (ch, i))
                                 + std::abs (midBuf.getSample (ch, i)));
            hi = juce::jmax (hi, std::abs (topBuf.getSample (ch, i)));
        }

        const auto det = detector.push (lo, hi);

        // A sung note lifts both bands together. A plosive, a footstep or a
        // knock on the boom arm lifts only the bottom, and the size of that
        // imbalance -- not the size of the jump -- is how bad it is.
        float target = 1.0f;
        if (lo > 3.0e-4f && det.lfBoost > sensitivity && det.contrast > 1.8f)
        {
            peakLfBoost = juce::jmax (peakLfBoost, det.contrast);
            const float severity = juce::jlimit (0.0f, 1.0f,
                                                 (gainToDb (det.contrast) - 5.0f) / 12.0f);
            target = 1.0f - (1.0f - depthLin) * severity * amount;
        }

        if (target < gain)
        {
            gain = attackCoef * gain + (1.0f - attackCoef) * target;
            holdLeft = holdSamples;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            gain = releaseCoef * gain + (1.0f - releaseCoef) * target;
        }

        worst = juce::jmin (worst, gainToDb (gain));

        // Full depth under f1, half of it in dB over the octave above: the
        // cheap approximation to sweeping a high-pass up and back down.
        const float midGain = std::sqrt (gain);
        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i,
                              topBuf.getSample (ch, i)
                              + midBuf.getSample (ch, i) * midGain
                              + subBuf.getSample (ch, i) * gain);
    }

    lastReductionDb = worst;
}

//==============================================================================
// UpwardExpander
//==============================================================================
void UpwardExpander::prepare (double sampleRate, int)
{
    sr = sampleRate;
    // Detector: quick enough to have settled inside a syllable, slow enough on
    // release to describe the syllable rather than the waveform.
    envAtkCoef  = timeCoef (5.0f,   sampleRate);
    envRelCoef  = timeCoef (200.0f, sampleRate);
    // Quick attack, as asked for: the lift is on the punch, not after it.
    gainAtkCoef = timeCoef (3.0f,   sampleRate);
    gainRelCoef = timeCoef (120.0f, sampleRate);
    // The reference is the take average, not the phrase average. Anything fast
    // enough to follow phrases would cancel exactly the differences this stage
    // exists to remove.
    refCoef     = timeCoef (10000.0f, sampleRate);
    reset();
}

void UpwardExpander::reset()
{
    envSq = 0.0f;
    refLin = seedLin;
    gain = 1.0f;
    lastBoostDb = peakBoostDb = 0.0f;
}

void UpwardExpander::setParams (const AnalysisResult& a, float amt)
{
    amount = juce::jlimit (0.0f, 2.0f, amt);
    const float ratio = juce::jlimit (1.05f, 3.0f, a.upwardRatio);
    slope = 1.0f - 1.0f / ratio;
    thresholdOffsetDb = juce::jlimit (-12.0f, 0.0f,  a.upwardThresholdDb);
    floorOffsetDb     = juce::jmin (thresholdOffsetDb - 2.0f,
                                    juce::jlimit (-30.0f, -4.0f, a.upwardFloorDb));
    fadeDb            = juce::jlimit (2.0f, 18.0f, a.upwardFadeDb);
    maxBoostDb        = juce::jlimit (0.0f, 14.0f, a.upwardMaxBoostDb);
    seedLin           = dbToGain (juce::jlimit (-70.0f, 0.0f, a.upwardReferenceDb));
    if (refLin <= 0.0f) refLin = seedLin;
}

void UpwardExpander::process (juce::AudioBuffer<float>& buffer, const float* gateWeight)
{
    if (amount <= 0.0f || maxBoostDb <= 0.0f) return;

    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0) return;

    const float boostCap = maxBoostDb * juce::jmin (amount, 2.0f);
    // Below this the follower is looking at a gap, not at delivery; letting
    // silence into the average would drag the reference down until the stage
    // started lifting the room instead of the voice.
    const float refGateLin = dbToGain (floorOffsetDb - 6.0f);
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float sq = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float v = buffer.getSample (ch, i);
            sq = juce::jmax (sq, v * v);
        }

        const float c = sq > envSq ? envAtkCoef : envRelCoef;
        envSq = c * envSq + (1.0f - c) * sq;
        const float env = std::sqrt (envSq);

        if (env > refLin * refGateLin)
            refLin = refCoef * refLin + (1.0f - refCoef) * env;

        const float relDb = gainToDb (env / juce::jmax (refLin, 1.0e-9f));

        float boostDb = 0.0f;
        if (relDb < thresholdOffsetDb)
        {
            boostDb = juce::jmin ((thresholdOffsetDb - relDb) * slope, boostCap);

            // Taper to nothing under the floor. Without it the stage does its
            // deepest lifting on whatever is quietest, which is the room.
            if (relDb < floorOffsetDb)
                boostDb *= juce::jlimit (0.0f, 1.0f,
                                         (relDb - (floorOffsetDb - fadeDb)) / fadeDb);

            // Stand down wherever the gate is pulling down.
            if (gateWeight != nullptr)
                boostDb *= juce::jlimit (0.0f, 1.0f, gateWeight[i]);
        }

        const float target = dbToGain (boostDb);
        const float gc = target > gain ? gainAtkCoef : gainRelCoef;
        gain = gc * gain + (1.0f - gc) * target;

        worst = juce::jmax (worst, gainToDb (gain));

        for (int ch = 0; ch < numCh; ++ch)
            buffer.setSample (ch, i, buffer.getSample (ch, i) * gain);
    }

    lastBoostDb = gainToDb (gain);
    peakBoostDb = juce::jmax (peakBoostDb, worst);
}

//==============================================================================
// TransientSoftener
//==============================================================================
void TransientSoftener::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    numCh = juce::jmax (1, numChannels);

    // 2 ms. The reduction has to be fully in place before the attack arrives,
    // and the gain attack below is set to reach its target inside that window.
    lookahead = juce::jmax (8, (int) (0.002 * sampleRate));
    delayLine.setSize (numCh, lookahead + maxBlockSize + 4);

    fastAtk = timeCoef (0.5f,  sampleRate);
    fastRel = timeCoef (15.0f, sampleRate);
    // The reference has to reach the body of the word, and reach it quickly.
    // At a 120 ms attack it never caught up inside a syllable, so the ratio sat
    // above the trip point through most of every word and the stage worked as a
    // broadband compressor -- envelope modulation fell 0.399 to 0.368 while the
    // onsets it was aimed at got 0.3 dB worse. At 25 ms it tracks the body and
    // only a genuine attack outruns it.
    slowAtk = timeCoef (12.0f,  sampleRate);
    slowRel = timeCoef (120.0f, sampleRate);

    gainAtk = std::exp (-3.0f / (float) lookahead);
    // Short. A duck that outlasts the transient pulls the body of the word down
    // with the head, and the RATIO between them -- which is the whole point --
    // comes out unchanged. It has to be over before the vowel is.
    gainRel = timeCoef (10.0f, sampleRate);
    reset();
}

void TransientSoftener::reset()
{
    delayLine.clear();
    writeIdx = 0;
    fastEnv = slowEnv = 0.0f;
    gain = 1.0f;
    lastReductionDb = 0.0f;
}

void TransientSoftener::setParams (const AnalysisResult& a, float amount)
{
    const float amt = juce::jlimit (0.0f, 2.0f, amount);
    threshDb = juce::jlimit (2.0f, 12.0f, a.transientThreshDb);
    depthDb  = juce::jlimit (-12.0f, 0.0f, a.transientDepthDb) * amt;
    // Reaches full depth about 8 dB past the threshold, so an ordinary word
    // onset is barely touched and a hard consonant gets the lot.
    slope = 1.0f / 8.0f;
}

void TransientSoftener::process (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const int ch = juce::jmin (numCh, buffer.getNumChannels());
    const int len = delayLine.getNumSamples();
    if (n <= 0 || ch <= 0 || len <= 0) return;

    // Still has to run when it is doing nothing: the delay line is part of the
    // reported latency, so skipping it would shift the signal.
    float worst = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < ch; ++c)
            peak = juce::jmax (peak, std::abs (buffer.getSample (c, i)));

        auto follow = [] (float& e, float x, float atk, float rel)
        {
            const float c = x > e ? atk : rel;
            e = c * e + (1.0f - c) * x;
        };
        follow (fastEnv, peak, fastAtk, fastRel);
        follow (slowEnv, peak, slowAtk, slowRel);

        float target = 1.0f;
        if (depthDb < 0.0f && slowEnv > 1.0e-5f)
        {
            const float excessDb = gainToDb (fastEnv / slowEnv);
            if (excessDb > threshDb)
                target = dbToGain (juce::jmax (depthDb,
                                               (excessDb - threshDb) * slope * depthDb));
        }

        const float coef = target < gain ? gainAtk : gainRel;
        gain = coef * gain + (1.0f - coef) * target;
        worst = juce::jmin (worst, gainToDb (gain));

        const int readIdx = (writeIdx + len - lookahead) % len;
        for (int c = 0; c < ch; ++c)
        {
            const float delayed = delayLine.getSample (c, readIdx);
            delayLine.setSample (c, writeIdx, buffer.getSample (c, i));
            buffer.setSample (c, i, delayed * gain);
        }
        writeIdx = (writeIdx + 1) % len;
    }

    // Worst since the last reset, not since the last block: sampling a
    // per-block meter after the file has ended reads whatever the trailing
    // silence did, which is nothing.
    lastReductionDb = juce::jmin (lastReductionDb, worst);
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

        // Read exactly `lookahead` samples back. Reading the oldest slot in the
        // buffer instead delayed by its whole length -- lookahead plus the
        // block size -- so the gain computed from a peak was applied to audio
        // ten milliseconds ahead of it, and the peak itself arrived after the
        // release had started. It also made the plugin's true latency a
        // function of the host's buffer size while the reported figure stayed
        // put.
        const int readIdx = (writeIdx + len - lookahead) % len;

        for (int c = 0; c < ch; ++c)
        {
            const float delayed = delayLine.getSample (c, readIdx);
            delayLine.setSample (c, writeIdx, buffer.getSample (c, i));

            // Hard clip catches whatever the smoothed gain overshoots; it only
            // ever engages on the residual, so it stays inaudible.
            buffer.setSample (c, i, juce::jlimit (-thresholdLin, thresholdLin,
                                                  delayed * gain));
        }

        writeIdx = (writeIdx + 1) % len;
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

    // ~2 ms: long enough for the gain to be in place before the ess arrives,
    // short enough not to smear the consonant it is protecting.
    lookahead = juce::jmax (8, (int) (0.002 * sampleRate));
    // Both bands go down the line: delaying only the sibilant one would put
    // the two halves of the crossover out of step and the split would stop
    // reconstructing.
    sibDelay.setSize (numChannels * 2, lookahead + maxBlockSize + 4);
    reset();
}

void DeEsser::reset()
{
    sibDelay.clear();
    delayWrite = 0;
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

        // The gain is computed from the sibilant band as it is NOW and applied
        // to the band as it was `lookahead` samples ago, so the duck is already
        // in place when the ess arrives instead of chasing it.
        const float g = dbToGain (env);
        const int len = sibDelay.getNumSamples();
        const int readIdx = (delayWrite + len - lookahead) % len;

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float dRest = sibDelay.getSample (ch, readIdx);
            const float dSib  = sibDelay.getSample (numCh + ch, readIdx);

            sibDelay.setSample (ch,         delayWrite, restBuffer.getSample (ch, i));
            sibDelay.setSample (numCh + ch, delayWrite, sibBuffer.getSample (ch, i));

            buffer.setSample (ch, i, dRest + dSib * g);
        }
        delayWrite = (delayWrite + 1) % len;
    }

    lastReductionDb = worst;
}

//==============================================================================
// AdaptiveToneMatch
//==============================================================================
void AdaptiveToneMatch::prepare (double sampleRate, int, int numChannels)
{
    sr = sampleRate;
    channels = juce::jlimit (1, 2, numChannels);

    ring.assign ((size_t) fftSize, 0.0f);
    scratch.assign ((size_t) fftSize * 2, 0.0f);
    powerAccum.assign ((size_t) fftSize / 2, 0.0f);

    juce::dsp::ProcessSpec spec { sampleRate, 512, 1 };
    for (int ch = 0; ch < 2; ++ch)
        for (auto& f : filters[ch]) f.prepare (spec);

    const float hopSeconds = (float) hopSize / (float) sampleRate;
    // Measurement window ~2.5 s of speech. Long enough that a single loud
    // syllable does not move the curve, short enough that the loop is not
    // chasing a spectrum from ten seconds ago.
    accumLeak = std::exp (-hopSeconds / 2.5f);
    peakDecay = std::exp (-hopSeconds / 4.0f);
    framesPerUpdate = juce::jmax (1, (int) (0.5f / hopSeconds));

    reset();
}

void AdaptiveToneMatch::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (powerAccum.begin(), powerAccum.end(), 0.0f);
    ringPos = sinceHop = framesSeen = framesSinceUpdate = 0;
    progPeak = 0.0f;
    updates = 0;
    maxTrimDb = 0.0f;
    gainDb = base;
    for (int ch = 0; ch < 2; ++ch)
        for (auto& f : filters[ch]) f.reset();
    applyGains();
}

void AdaptiveToneMatch::setBase (const std::array<float, numToneBands>& baseDb, float amount)
{
    const float amt = juce::jlimit (0.0f, 2.0f, amount);
    std::array<float, numToneBands> scaled {};
    for (int b = 0; b < numToneBands; ++b) scaled[(size_t) b] = baseDb[(size_t) b] * amt;

    // Re-seeding on every call would throw away the loop's work whenever a trim
    // knob is nudged; only a real change to the static curve resets it.
    bool changed = ! seeded;
    for (int b = 0; b < numToneBands && ! changed; ++b)
        changed = std::abs (scaled[(size_t) b] - base[(size_t) b]) > 0.05f;

    base = scaled;
    if (changed) { gainDb = base; seeded = true; }
    applyGains();
}

void AdaptiveToneMatch::applyGains()
{
    for (int b = 0; b < numToneBands; ++b)
    {
        const float f0 = toneBandHz[(size_t) b];
        const float g  = gainDb[(size_t) b];
        active[(size_t) b] = std::abs (g) >= 0.15f && f0 >= 30.0f && f0 < sr * 0.45;
        if (! active[(size_t) b]) continue;

        // ArrayCoefficients returns by value, so no allocation happens here --
        // this runs several times a second on the audio thread.
        const auto c = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (
                           sr, f0, qForBandwidth (1.0f / 3.0f), dbToGain (g));
        for (int ch = 0; ch < channels; ++ch)
            *filters[ch][(size_t) b].coefficients = c;
    }
}

void AdaptiveToneMatch::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = juce::jmin (channels, buffer.getNumChannels());
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0) return;

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int b = 0; b < numToneBands; ++b)
        {
            if (! active[(size_t) b]) continue;
            auto& f = filters[ch][(size_t) b];
            for (int i = 0; i < n; ++i) d[i] = f.processSample (d[i]);
        }
    }

}

void AdaptiveToneMatch::observe (const juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    if (n > 0 && buffer.getNumChannels() > 0)
        pushAnalysis (buffer.getReadPointer (0), n);
}

void AdaptiveToneMatch::pushAnalysis (const float* x, int n)
{
    for (int i = 0; i < n; ++i)
    {
        ring[(size_t) ringPos] = x[i];
        ringPos = (ringPos + 1) % fftSize;

        if (++sinceHop >= hopSize)
        {
            sinceHop = 0;
            analyseFrame();
        }
    }
}

void AdaptiveToneMatch::analyseFrame()
{
    // unwrap the ring, oldest sample first
    float framePeak = 0.0f;
    for (int i = 0; i < fftSize; ++i)
    {
        const float v = ring[(size_t) ((ringPos + i) % fftSize)];
        scratch[(size_t) i] = v;
        framePeak = juce::jmax (framePeak, std::abs (v));
    }
    std::fill (scratch.begin() + fftSize, scratch.end(), 0.0f);

    progPeak = juce::jmax (framePeak, progPeak * peakDecay);

    // Same gate the offline measurement uses: frames more than 34 dB under the
    // running peak are gaps, and averaging gaps into the spectrum pulls the
    // whole curve toward the shape of the room.
    if (framePeak < progPeak * 0.02f || framePeak < dbToGain (noiseFloorDb + 10.0f))
        return;

    win.multiplyWithWindowingTable (scratch.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (scratch.data());

    for (int i = 0; i < fftSize / 2; ++i)
    {
        const float p = scratch[(size_t) i] * scratch[(size_t) i];
        powerAccum[(size_t) i] = accumLeak * powerAccum[(size_t) i] + (1.0f - accumLeak) * p;
    }

    ++framesSeen;
    if (++framesSinceUpdate >= framesPerUpdate && framesSeen >= framesPerUpdate * 3)
    {
        framesSinceUpdate = 0;
        integrate();
    }
}

void AdaptiveToneMatch::integrate()
{
    for (int b = 0; b < numToneBands; ++b)
    {
        const float fc = toneBandHz[(size_t) b];
        const int lo = juce::jmax (1, (int) std::floor (fc / 1.122462f * fftSize / sr));
        const int hi = juce::jmin ((int) powerAccum.size() - 1,
                                   (int) std::ceil (fc * 1.122462f * fftSize / sr));
        float sum = 0.0f; int cnt = 0;
        for (int i = lo; i <= hi; ++i) { sum += powerAccum[(size_t) i]; ++cnt; }
        measuredDb[(size_t) b] = (cnt > 0 && sum > 0.0f)
                               ? 10.0f * std::log10 (sum / (float) cnt) : -120.0f;
    }

    float measRef = 0.0f, tgtRef = 0.0f; int refCount = 0;
    for (int b = 0; b < numToneBands; ++b)
    {
        const float f = toneBandHz[(size_t) b];
        if (f < 200.0f || f > 2000.0f) continue;
        measRef += measuredDb[(size_t) b];
        tgtRef  += kTargetLtasDb[(size_t) b];
        ++refCount;
    }
    if (refCount <= 0) return;
    measRef /= (float) refCount;
    tgtRef  /= (float) refCount;

    // Integrator gain. With a 2.5 s measurement lag and an update every half
    // second there are five updates inside one lag; anything much above 0.08
    // per update makes the loop ring instead of settle.
    constexpr float k = 0.08f;
    constexpr float slew = 0.5f;

    for (int b = 0; b < numToneBands; ++b)
    {
        const float f = toneBandHz[(size_t) b];
        // Under 100 Hz the high-pass owns the correction and this bank stays
        // where the LISTEN pass put it. The target falls 6 dB per third-octave
        // down there, which is steeper than a bank smoothed enough not to comb
        // can synthesise; letting the loop try anyway produced a -14 dB notch at
        // 63 Hz sitting next to a +0.5 dB bump at 50 Hz. A steep Butterworth is
        // simply the right tool for a slope, and it does not ring.
        if (f < 100.0f || f >= sr * 0.45) continue;

        const float meas = measuredDb[(size_t) b] - measRef;
        const float tgt  = kTargetLtasDb[(size_t) b] - tgtRef;
        const float err  = tgt - meas;

        gainDb[(size_t) b] += juce::jlimit (-slew, slew, k * err);

        // Bounds, in the same asymmetric spirit as the static solve: a cut can
        // only remove something measurably too loud, a boost lifts whatever
        // else lives in the band.
        const float maxBoost = f > 8000.0f ? 4.0f : 8.0f;
        // Room to actually finish the job under 100 Hz, where nothing a lead
        // vocal needs lives. Kept shallower below 40 Hz: a 1/3-octave peaking
        // filter that deep at 25 Hz puts its poles close enough to the unit
        // circle that single-precision state stops being trustworthy.
        const float maxCut = f < 40.0f ? -20.0f : (f < 100.0f ? -26.0f : -12.0f);
        gainDb[(size_t) b] = juce::jlimit (maxCut, maxBoost, gainDb[(size_t) b]);
        // ...and never far from what the LISTEN pass decided, so a pathological
        // loop cannot invent a curve of its own. Wider downward under 100 Hz:
        // that is where the largest correction is genuinely needed and where
        // over-cutting costs nothing, and the symmetric bound was leaving 5 dB
        // of measured rumble in place because it ran out of authority.
        const float slack = f < 100.0f ? 22.0f : 8.0f;
        gainDb[(size_t) b] = juce::jlimit (base[(size_t) b] - slack,
                                           base[(size_t) b] + 8.0f,
                                           gainDb[(size_t) b]);

    }

    // Regularisation, and it is not optional.
    //
    // Neighbouring 1/3-octave filters overlap heavily, so a whole family of
    // very different gain vectors produces almost the same response at the band
    // centres. Nothing in the error term distinguishes between them, and an
    // integrator left to itself walks off into the largest one: measured on
    // real material it settled at +6.9 dB on 200 Hz against -5.8 dB on 315 Hz,
    // which reads as a perfect match on a 1/3-octave meter and sounds like a
    // comb filter. Smoothing across bands removes precisely that mode -- it is
    // the highest spatial frequency the bank can express, and it is the one the
    // error term never pushes back on. Any shape the target genuinely asks for
    // is restored by the next update.
    {
        // Small pull back toward the static solve as well, so the loop stays
        // anchored to something the LISTEN pass can vouch for.
        constexpr float leak = 0.004f;

        std::array<float, numToneBands> sm = gainDb;
        for (int b = 0; b < numToneBands; ++b)
        {
            const int a = juce::jmax (0, b - 1);
            const int c = juce::jmin (numToneBands - 1, b + 1);
            const float avg = 0.25f * gainDb[(size_t) a]
                            + 0.50f * gainDb[(size_t) b]
                            + 0.25f * gainDb[(size_t) c];

            constexpr float smooth = 0.12f;
            sm[(size_t) b] = gainDb[(size_t) b] + smooth * (avg - gainDb[(size_t) b])
                                                + leak   * (base[(size_t) b] - gainDb[(size_t) b]);
        }
        gainDb = sm;
    }

    maxTrimDb = 0.0f;
    for (int b = 0; b < numToneBands; ++b)
        maxTrimDb = juce::jmax (maxTrimDb,
                                std::abs (gainDb[(size_t) b] - base[(size_t) b]));

    ++updates;
    applyGains();
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
        hpf2[ch].prepare (spec);
        hpf3[ch].prepare (spec);
        spectral[ch].prepare (sampleRate);
        for (auto& f : surgical[ch])  f.prepare ({ sampleRate, (juce::uint32) maxBlock, 1 });
    }

    tone.prepare (sampleRate, maxBlock, channels);

    upward.prepare (sampleRate, channels);
    gateTrace.assign ((size_t) maxBlock, 1.0f);
    comp.prepare (sampleRate, channels);
    deEss.prepare (sampleRate, maxBlock, channels);
    softener.prepare (sampleRate, maxBlock, channels);

    limiter.prepare (sampleRate, maxBlock, channels);
    limiter.setThresholdDb (-0.8f);

    reset();
}

void CleanupChain::reset()
{
    for (int ch = 0; ch < channels; ++ch)
    {
        hpf[ch].reset();
        hpf2[ch].reset();
        hpf3[ch].reset();
        spectral[ch].reset();
        for (auto& f : surgical[ch])  f.reset();
    }
    tone.reset();
    deClip.reset();
    plosive.reset();
    upward.reset();
    comp.reset();
    deEss.reset();
    softener.reset();
    limiter.reset();
    gateEnv = 1.0f;
    gateGain = 1.0f;
    gateOpen = false;
    gateDetEnv = 0.0f;
    gateHoldLeft = 0;
    progEnv = 0.0f;
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

    for (int ch = 0; ch < channels; ++ch)
    {
        // Butterworth section Qs for sixth order: 0.5176, 0.7071, 1.9319.
        *hpf[ch].coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, analysis.highPassHz, 0.5176f);
        *hpf2[ch].coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, analysis.highPassHz, 0.7071f);
        *hpf3[ch].coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, analysis.highPassHz, 1.9319f);

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

        spectral[ch].setDenoiseAmount (bypass.deNoise ? 0.0f : analysis.denoiseAmount * clAmt);
        spectral[ch].setDeverbAmount  (bypass.deVerb  ? 0.0f : analysis.deverbAmount  * clAmt);
        spectral[ch].setDeverbDecay   (analysis.rt60Seconds);
        spectral[ch].setTailDb        (analysis.directToReverbDb);
        spectral[ch].setHarmonicSpacing (analysis.medianF0Hz);
        spectral[ch].setResonanceDepth (bypass.resonance ? 0.0f : 0.55f * eqAmt);
    }

    // Bands and depth are measured, not guessed -- see VocalAnalyzer's plosive
    // pass, which runs this exact detector over the capture.
    plosive.setParams (analysis);
    plosive.setAmount (bypass.plosive ? 0.0f : trims.cleanupAmount);

    upward.setParams (analysis, bypass.upward ? 0.0f : trims.compAmount);
    comp.setParams  (analysis, bypass.compressor ? 0.0f : trims.compAmount);
    deEss.setParams (analysis, bypass.deEss ? 0.0f : trims.deEssAmount);
    softener.setParams (analysis, bypass.deEss ? 0.0f : trims.deEssAmount);

    // Pick the solve that matches whether the notches are actually in circuit:
    // their skirts reach into neighbouring bands, so a curve solved around them
    // is wrong the moment they are switched off.
    tone.setBase (bypass.surgicalEq ? analysis.toneFilterGainDbNoNotch
                                    : analysis.toneFilterGainDb, eqAmt);
    tone.setNoiseFloorDb (analysis.noiseFloorDb);

    gateOpenLin     = dbToGain (analysis.gateThresholdDb);
    gateCloseLin    = dbToGain (analysis.gateThresholdDb - 6.0f);   // 6 dB hysteresis
    gateRangeLin    = dbToGain (analysis.gateRangeDb);
    gateAttackCoef  = timeCoef (analysis.gateAttackMs,  sr);
    gateReleaseCoef = timeCoef (analysis.gateReleaseMs, sr);

    // Detector: quick enough to catch a word onset, slow enough on release that
    // it describes the envelope rather than following the waveform down to zero
    // every half cycle.
    gateDetAtkCoef  = timeCoef (0.5f,  sr);
    gateDetRelCoef  = timeCoef (35.0f, sr);
    gateHoldSamples = (int) (0.001 * analysis.gateHoldMs * sr);

    // Relative threshold sits just above the measured tail, so what gets pulled
    // down is the room rather than the performance. tailDb is negative and is
    // relative to the level just before each gap.
    relativeOffsetLin = dbToGain (juce::jlimit (-24.0f, -8.0f, analysis.tailDb + 3.0f));
    progReleaseCoef   = timeCoef (1200.0f, sr);   // programme level, not syllables
    // Gentle: a high ratio turns the expander into a chattering gate.
    expanderRatio     = juce::jlimit (1.4f, 2.2f, 1.4f + analysis.reverbRatio * 0.8f);
}

int CleanupChain::getLatencySamples() const noexcept
{
    if (! haveAnalysis) return 0;

    int latency = 0;
    // Whether the STFT is actually RUNNING, not whether its bypass switches are
    // off: on a clean source all three of its amounts can be zero, the engine
    // short-circuits, and reporting its frame anyway asks the host to
    // compensate for a delay the signal never incurred.
    if (spectral[0].isActive())
        latency += SpectralEngine::getLatencySamples();
    if (! bypass.deEss)
        latency += deEss.getLatencySamples() + softener.getLatencySamples();
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
                d[i] = hpf3[ch].processSample (
                           hpf2[ch].processSample (hpf[ch].processSample (d[i])));
        }

    // 2. de-noise + de-verb + resonance suppression, sharing one STFT
    for (int ch = 0; ch < numCh; ++ch)
        spectral[ch].process (buffer.getWritePointer (ch), numSamples);

    // 3. gate + programme-relative expander
    if (! bypass.gate)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));

            const float dc = peak > gateDetEnv ? gateDetAtkCoef : gateDetRelCoef;
            gateDetEnv = dc * gateDetEnv + (1.0f - dc) * peak;
            const float detect = gateDetEnv;

            // Programme level: rises immediately, forgets slowly. This is the
            // reference the relative threshold hangs off, so it has to track
            // takes rather than syllables.
            progEnv = juce::jmax (detect, progEnv * progReleaseCoef);

            // Whichever threshold is higher does the work.
            const float relThresh = progEnv * relativeOffsetLin;
            const float openAt    = juce::jmax (gateOpenLin, relThresh);
            const float closeAt   = openAt * 0.5f;             // 6 dB hysteresis

            if (! gateOpen && detect > openAt)
            {
                gateOpen = true;
                gateHoldLeft = gateHoldSamples;
            }
            else if (gateOpen && detect < closeAt)
            {
                // Hold before closing. Stops inside a word drop the level for
                // 20-60 ms; without a hold the gate closes into every one of
                // them and reopens on the other side.
                if (gateHoldLeft > 0) --gateHoldLeft;
                else                  gateOpen = false;
            }
            else if (gateOpen)
            {
                gateHoldLeft = gateHoldSamples;
            }

            float target = 1.0f;
            if (! gateOpen)
            {
                // Expand rather than switch: a hard gate on a reverberant take
                // chops the tail off mid-decay and sounds worse than the room.
                const float over = detect > 1.0e-7f ? gainToDb (detect / juce::jmax (closeAt, 1.0e-7f))
                                                    : -60.0f;

                // `over` is only negative below the close threshold. Inside the
                // hysteresis band the gate is still shut while the signal sits
                // ABOVE closeAt, which makes it positive -- and an expander that
                // acts on a positive overshoot amplifies instead of attenuating.
                // That put up to +12 dB of gain on the quiet passages between
                // words, which is to say on the room and the noise floor.
                const float reduction = juce::jmin (0.0f, over) * (expanderRatio - 1.0f);
                target = juce::jlimit (gateRangeLin, 1.0f, dbToGain (reduction));
            }

            const float coef = target > gateEnv ? gateAttackCoef : gateReleaseCoef;
            gateEnv = coef * gateEnv + (1.0f - coef) * target;

            gateTrace[(size_t) i] = gateEnv;

            for (int ch = 0; ch < numCh; ++ch)
                buffer.setSample (ch, i, buffer.getSample (ch, i) * gateEnv);
        }
        gateGain = gateEnv;
    }
    else
    {
        std::fill (gateTrace.begin(), gateTrace.begin() + numSamples, 1.0f);
    }

    // 4. upward expansion: lift the quiet delivery toward the take average
    if (! bypass.upward)
        upward.process (buffer, gateTrace.data());

    // 5. surgical notches
    if (! bypass.surgicalEq)
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int k = 0; k < numSurgical; ++k)
                for (int i = 0; i < numSamples; ++i)
                    d[i] = surgical[ch][(size_t) k].processSample (d[i]);
        }

    // 6. two-stage compression
    if (! bypass.compressor)
        comp.process (buffer);

    // 7. tone match to the universal target curve, closed loop
    if (! bypass.toneMatch)
        tone.process (buffer);

    // 8. de-ess AFTER the tone stage, not before it.
    //
    // The tone stage is where additive HF comes from -- on a dull source the
    // loop lifts 12.5 kHz and 16 kHz by several dB -- and de-essing upstream of
    // it means none of that addition is ever de-essed. Measured against the
    // source, the chain was making the leading edge of a word 1.1 dB spikier
    // above 5 kHz, and the tone stage on its own accounted for most of it.
    //
    // The loop still gets to see the result: its measurement is taken below,
    // from the finished signal, so it keeps matching the target rather than
    // matching its own output and letting the de-esser darken the result.
    if (! bypass.deEss)
        deEss.process (buffer);

    // 9. take the edge off what the chain sharpened
    if (! bypass.deEss)
        softener.process (buffer);

    // The loop matches the FINISHED signal, so it is measured last.
    if (! bypass.toneMatch)
        tone.observe (buffer);

    // 10. safety limiter
    if (! bypass.limiter)
        limiter.process (buffer);
}

} // namespace listenator
