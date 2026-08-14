#include "EffectsChain.h"
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
        return ms <= 0.0f ? 0.0f : std::exp (-1.0f / (float) (0.001 * ms * sr));
    }

    /** Semitone offsets from the root for each supported scale. */
    const std::vector<int>& scaleSteps (Scale s)
    {
        static const std::vector<int> chromatic { 0,1,2,3,4,5,6,7,8,9,10,11 };
        static const std::vector<int> major     { 0,2,4,5,7,9,11 };
        static const std::vector<int> minor     { 0,2,3,5,7,8,10 };
        static const std::vector<int> harmMinor { 0,2,3,5,7,8,11 };
        static const std::vector<int> pentMaj   { 0,2,4,7,9 };
        static const std::vector<int> pentMin   { 0,3,5,7,10 };

        switch (s)
        {
            case Scale::major:            return major;
            case Scale::minor:            return minor;
            case Scale::harmonicMinor:    return harmMinor;
            case Scale::pentatonicMajor:  return pentMaj;
            case Scale::pentatonicMinor:  return pentMin;
            case Scale::chromatic:
            default:                      return chromatic;
        }
    }
}

//==============================================================================
// PitchCorrector - TD-PSOLA
//==============================================================================
void PitchCorrector::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    tracker.prepare (sampleRate, frameSize);
    tracker.setRange (60.0f, 1200.0f);

    const int ringLen = juce::nextPowerOfTwo (std::max (16384, maxBlockSize * 4));
    inBuf.assign ((size_t) ringLen, 0.0f);
    outBuf.assign ((size_t) ringLen, 0.0f);
    reset();
}

void PitchCorrector::reset()
{
    std::fill (inBuf.begin(),  inBuf.end(),  0.0f);
    std::fill (outBuf.begin(), outBuf.end(), 0.0f);
    writePos = 0;
    readPos = 0;
    phase = 0.0f;
    currentRatio = 1.0f;
    lastDetected = lastTarget = 0.0f;
}

/** Nearest in-scale note to the detected pitch. */
float PitchCorrector::snapToScale (float hz) const noexcept
{
    if (hz <= 0.0f) return 0.0f;

    const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
    const auto& steps = scaleSteps (scale);

    float best = midi;
    float bestDist = 1.0e9f;

    // search a couple of octaves either side of the detected note
    const int baseOct = (int) std::floor ((midi - (float) root) / 12.0f);
    for (int oct = baseOct - 1; oct <= baseOct + 1; ++oct)
        for (int st : steps)
        {
            const float cand = (float) root + 12.0f * (float) oct + (float) st;
            const float d = std::abs (cand - midi);
            if (d < bestDist) { bestDist = d; best = cand; }
        }

    return 440.0f * std::pow (2.0f, (best - 69.0f) / 12.0f);
}

void PitchCorrector::process (float* mono, int numSamples)
{
    const int ringLen = (int) inBuf.size();
    const int mask = ringLen - 1;

    for (int n = 0; n < numSamples; ++n)
    {
        inBuf[(size_t) writePos] = mono[n];

        mono[n] = outBuf[(size_t) writePos];
        outBuf[(size_t) writePos] = 0.0f;

        writePos = (writePos + 1) & mask;

        // Re-estimate pitch once per frameSize/4 samples
        if ((writePos % (frameSize / 4)) == 0)
        {
            std::vector<float> frame ((size_t) frameSize);
            const int start = (writePos - frameSize + ringLen) & mask;
            for (int i = 0; i < frameSize; ++i)
                frame[(size_t) i] = inBuf[(size_t) ((start + i) & mask)];

            float conf = 0.0f;
            const float f0 = tracker.process (frame.data(), &conf);

            if (f0 > 0.0f && conf > 0.5f)
            {
                lastDetected = f0;
                lastTarget   = snapToScale (f0);

                const float fullRatio = lastTarget / f0;
                // strength blends between untouched and fully snapped
                const float wanted = std::pow (fullRatio, strength);

                // retune speed: how fast we slew toward the wanted ratio
                const float coef = timeCoef (retuneMs, sr / (frameSize / 4));
                currentRatio = coef * currentRatio + (1.0f - coef) * wanted;
            }
            else
            {
                // unvoiced: drift back to unity so consonants pass clean
                currentRatio = 0.85f * currentRatio + 0.15f * 1.0f;
            }
        }

        // --- PSOLA grain scheduling -----------------------------------------
        // Synthesis marks are spaced period/ratio apart; each grain is copied
        // verbatim from the input, so the spectral envelope (formants) is
        // untouched by construction.
        if (lastDetected > 0.0f)
        {
            const float period = (float) sr / lastDetected;
            const float synthPeriod = std::max (16.0f, period / std::max (currentRatio, 0.25f));

            phase += 1.0f;
            if (phase >= synthPeriod)
            {
                phase -= synthPeriod;

                const int grainLen = std::min ((int) (period * 2.0f), ringLen / 4);
                if (grainLen > 8)
                {
                    // grain centred one period back so it's fully buffered
                    const int centre = (writePos - (int) period + ringLen) & mask;
                    const int begin  = (centre - grainLen / 2 + ringLen) & mask;

                    for (int i = 0; i < grainLen; ++i)
                    {
                        const float w = 0.5f - 0.5f * std::cos (
                            2.0f * juce::MathConstants<float>::pi * (float) i / (float) (grainLen - 1));
                        const int src = (begin + i) & mask;
                        const int dst = (writePos + i - grainLen / 2 + ringLen) & mask;
                        outBuf[(size_t) dst] += inBuf[(size_t) src] * w;
                    }
                }
            }
        }
        else
        {
            // no pitch: pass through so breaths and consonants stay intact
            outBuf[(size_t) writePos] += inBuf[(size_t) writePos];
        }
    }
}

//==============================================================================
// DuckedDelay
//==============================================================================
void DuckedDelay::prepare (double sampleRate, int, int numChannels)
{
    sr = sampleRate;
    buffer.setSize (std::max (2, numChannels), (int) (sampleRate * 2.5) + 4);
    juce::dsp::ProcessSpec spec { sampleRate, 512, 2 };
    for (auto& t : tone) { t.prepare (spec);
        *t.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, 4500.0f); }
    reset();
    setTempo (120.0, false);
}

void DuckedDelay::reset()
{
    buffer.clear();
    writeIdx = 0;
    duckEnv = 0.0f;
    for (auto& t : tone) t.reset();
}

void DuckedDelay::setTempo (double bpm, bool valid)
{
    const double beat = 60.0 / (valid && bpm > 20.0 ? bpm : 120.0);
    // dotted-eighth left, quarter right: the standard modern vocal throw
    delaySamplesL = (int) (beat * 0.75 * sr);
    delaySamplesR = (int) (beat * 0.5  * sr);
    const int maxD = buffer.getNumSamples() - 1;
    delaySamplesL = juce::jlimit (1, maxD, delaySamplesL);
    delaySamplesR = juce::jlimit (1, maxD, delaySamplesR);
}

void DuckedDelay::process (juce::AudioBuffer<float>& wet, const juce::AudioBuffer<float>& dry)
{
    const int numSamples = wet.getNumSamples();
    const int len = buffer.getNumSamples();
    const int numCh = std::min (2, wet.getNumChannels());
    const float attack = timeCoef (5.0f, sr), release = timeCoef (220.0f, sr);

    for (int i = 0; i < numSamples; ++i)
    {
        // duck from the DRY signal so the throw blooms in the gaps
        float d = 0.0f;
        for (int ch = 0; ch < dry.getNumChannels(); ++ch)
            d = std::max (d, std::abs (dry.getSample (ch, i)));

        const float coef = d > duckEnv ? attack : release;
        duckEnv = coef * duckEnv + (1.0f - coef) * d;
        const float duck = 1.0f - duckAmount * std::clamp (duckEnv * 3.0f, 0.0f, 0.85f);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const int dSamp = ch == 0 ? delaySamplesL : delaySamplesR;
            const int readIdx = (writeIdx - dSamp + len) % len;

            float delayed = buffer.getSample (ch, readIdx);
            delayed = tone[ch].processSample (delayed);

            const float in = wet.getSample (ch, i);
            buffer.setSample (ch, writeIdx, in + delayed * feedback);

            wet.setSample (ch, i, in + delayed * mix * duck);
        }

        writeIdx = (writeIdx + 1) % len;
    }
}

//==============================================================================
// DuckedReverb
//==============================================================================
void DuckedReverb::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    reverb.setSampleRate (sampleRate);
    scratch.setSize (std::max (2, numChannels), std::max (512, maxBlockSize));
    reset();
    setTempo (120.0, false);
}

void DuckedReverb::reset()
{
    reverb.reset();
    scratch.clear();
    duckEnv = 0.0f;
}

void DuckedReverb::setTempo (double bpm, bool valid)
{
    const double beat = 60.0 / (valid && bpm > 20.0 ? bpm : 120.0);
    // size the tail so it decays roughly within two beats
    const float decaySec = (float) (beat * 2.0);

    juce::Reverb::Parameters p;
    p.roomSize   = juce::jlimit (0.2f, 0.9f, decaySec / 3.0f);
    p.damping    = 0.45f;
    p.wetLevel   = 1.0f;
    p.dryLevel   = 0.0f;
    p.width      = 1.0f;
    p.freezeMode = 0.0f;
    reverb.setParameters (p);
}

void DuckedReverb::process (juce::AudioBuffer<float>& wet, const juce::AudioBuffer<float>& dry)
{
    const int numSamples = wet.getNumSamples();
    const int numCh = std::min (2, wet.getNumChannels());

    if (scratch.getNumSamples() < numSamples)
        scratch.setSize (scratch.getNumChannels(), numSamples, false, false, true);

    for (int ch = 0; ch < numCh; ++ch)
        scratch.copyFrom (ch, 0, wet, ch, 0, numSamples);

    if (numCh >= 2)
        reverb.processStereo (scratch.getWritePointer (0), scratch.getWritePointer (1), numSamples);
    else
        reverb.processMono (scratch.getWritePointer (0), numSamples);

    const float attack = timeCoef (8.0f, sr), release = timeCoef (300.0f, sr);

    for (int i = 0; i < numSamples; ++i)
    {
        float d = 0.0f;
        for (int ch = 0; ch < dry.getNumChannels(); ++ch)
            d = std::max (d, std::abs (dry.getSample (ch, i)));

        const float coef = d > duckEnv ? attack : release;
        duckEnv = coef * duckEnv + (1.0f - coef) * d;
        const float duck = 1.0f - duckAmount * std::clamp (duckEnv * 3.0f, 0.0f, 0.8f);

        for (int ch = 0; ch < numCh; ++ch)
            wet.setSample (ch, i, wet.getSample (ch, i)
                                 + scratch.getSample (ch, i) * mix * duck);
    }
}

//==============================================================================
// EffectsChain
//==============================================================================
void EffectsChain::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    channels = juce::jlimit (1, 2, numChannels);

    tuner.prepare (sampleRate, maxBlockSize);

    doubleBuf.setSize (2, (int) (sampleRate * 0.15) + 4);
    dryCopy.setSize (std::max (2, channels), std::max (512, maxBlockSize));

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
    for (int ch = 0; ch < 2; ++ch)
    {
        exciterHp[ch].prepare (spec);
        *exciterHp[ch].coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 7000.0f);

        charBand[ch].prepare (spec);
        *charBand[ch].coefficients =
            *juce::dsp::IIR::Coefficients<float>::makeBandPass (sampleRate, 1400.0f, 0.8f);
    }

    delay.prepare (sampleRate, maxBlockSize, 2);
    reverb.prepare (sampleRate, maxBlockSize, 2);

    reset();
}

void EffectsChain::reset()
{
    tuner.reset();
    doubleBuf.clear();
    dblWrite = 0;
    delay.reset();
    reverb.reset();
    for (int ch = 0; ch < 2; ++ch) { exciterHp[ch].reset(); charBand[ch].reset(); }
}

void EffectsChain::applyAnalysis (const AnalysisResult& a)
{
    analysis = a;
    haveAnalysis = a.valid;

    // Retune speed from how unstable the performance actually was: a singer
    // who is already close to pitch gets gentle correction, a wobbly one gets
    // tighter correction. This is the "pick a sensible value" call.
    const float instability = std::clamp (a.pitchStabilityCents / 50.0f, 0.0f, 1.0f);
    const float retune = juce::jmap (instability, 90.0f, 12.0f);
    tuner.setRetuneMs (retune);
    tuner.setStrength (juce::jmap (instability, 0.55f, 0.95f));
    tuner.setFormantPreserve (true);

    drive = a.saturationDrive;
    // dull sources get more air, already-bright ones get less
    exciterAmount = std::clamp ((-a.spectralTiltDbPerOct - 3.0f) / 6.0f, 0.0f, 0.8f);

    setTempo (a.tempoBpm, a.tempoValid);
    updateFromTrims();
}

void EffectsChain::setKey (int rootNote, Scale s) { tuner.setKey (rootNote, s); }

void EffectsChain::setTempo (double bpm, bool valid)
{
    delay.setTempo (bpm, valid);
    reverb.setTempo (bpm, valid);
}

void EffectsChain::updateFromTrims()
{
    delay.setMix   (bypass.delay  ? 0.0f : 0.18f * trims.delayMix);
    delay.setDuck  (trims.duckAmount);
    reverb.setMix  (bypass.reverb ? 0.0f : 0.16f * trims.reverbMix);
    reverb.setDuck (trims.duckAmount);
}

int EffectsChain::getLatencySamples() const noexcept
{
    return bypass.autoTune ? 0 : tuner.getLatencySamples();
}

void EffectsChain::process (juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    if (dryCopy.getNumSamples() < numSamples)
        dryCopy.setSize (std::max (2, numCh), numSamples, false, false, true);

    for (int ch = 0; ch < std::min (dryCopy.getNumChannels(), numCh); ++ch)
        dryCopy.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    // ---- 1. autotune (mono source drives both channels) --------------------
    if (! bypass.autoTune)
        tuner.process (buffer.getWritePointer (0), numSamples);

    // mono -> stereo: after tuning, mirror into the right channel so the
    // stereo generators below have something to work with
    if (numCh >= 2)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    // ---- 2. doubler: two short detuned taps panned apart -------------------
    if (! bypass.doubler && numCh >= 2)
    {
        const int len = doubleBuf.getNumSamples();
        const float depth = 0.006f * (float) sr;   // ~6 ms of movement
        const float mixAmt = 0.35f * trims.doublerMix;

        for (int i = 0; i < numSamples; ++i)
        {
            doubleBuf.setSample (0, dblWrite, buffer.getSample (0, i));

            for (int ch = 0; ch < 2; ++ch)
            {
                dblPhase[ch] += 0.35f / (float) sr * (ch == 0 ? 1.0f : 1.31f);
                if (dblPhase[ch] > 1.0f) dblPhase[ch] -= 1.0f;

                const float mod = std::sin (2.0f * juce::MathConstants<float>::pi * dblPhase[ch]);
                const float baseDelay = (ch == 0 ? 0.019f : 0.027f) * (float) sr;
                const float dSamp = baseDelay + mod * depth;

                const int i0 = (dblWrite - (int) dSamp + len) % len;
                const int i1 = (i0 - 1 + len) % len;
                const float frac = dSamp - std::floor (dSamp);
                const float tap = doubleBuf.getSample (0, i0) * (1.0f - frac)
                                + doubleBuf.getSample (0, i1) * frac;

                buffer.setSample (ch, i, buffer.getSample (ch, i) + tap * mixAmt);
            }

            dblWrite = (dblWrite + 1) % len;
        }
    }

    // ---- 3. saturation, drive derived from the dry dynamics ----------------
    if (! bypass.saturation && drive > 0.0f)
    {
        const float d = 1.0f + drive * trims.driveAmount * 6.0f;
        const float comp = 1.0f / std::tanh (d);
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* p = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                p[i] = std::tanh (p[i] * d) * comp;
        }
    }

    // ---- 4. exciter: generate air rather than just boosting it -------------
    if (! bypass.exciter && exciterAmount > 0.0f)
    {
        for (int ch = 0; ch < std::min (2, numCh); ++ch)
        {
            auto* p = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const float hi = exciterHp[ch].processSample (p[i]);
                // asymmetric shaping adds even harmonics above the HP corner
                const float harm = hi * std::abs (hi) * 2.0f;
                p[i] += harm * exciterAmount * 0.5f;
            }
        }
    }

    // ---- 5. character / lo-fi (off unless dialled in) ----------------------
    if (! bypass.character && trims.characterMix > 0.001f)
    {
        const float m = std::clamp (trims.characterMix, 0.0f, 1.0f);
        for (int ch = 0; ch < std::min (2, numCh); ++ch)
        {
            auto* p = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const float band = charBand[ch].processSample (p[i]);
                const float crushed = std::round (band * 64.0f) / 64.0f;
                p[i] = p[i] * (1.0f - m) + crushed * m * 1.6f;
            }
        }
    }

    // ---- 6. width: mid/side, widen the sides only --------------------------
    if (! bypass.width && numCh >= 2)
    {
        const float w = 1.0f + std::clamp (trims.widthAmount, 0.0f, 2.0f) * 0.6f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float l = buffer.getSample (0, i), r = buffer.getSample (1, i);
            const float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * w;
            buffer.setSample (0, i, mid + side);
            buffer.setSample (1, i, mid - side);
        }
    }

    // ---- 7/8. ducked sends -------------------------------------------------
    if (! bypass.delay)  delay.process  (buffer, dryCopy);
    if (! bypass.reverb) reverb.process (buffer, dryCopy);
}

} // namespace listenator
