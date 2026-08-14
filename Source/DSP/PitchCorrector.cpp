#include "PitchCorrector.h"
#include <cmath>
#include <algorithm>

namespace listenator
{

namespace
{
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
            case Scale::major:           return major;
            case Scale::minor:           return minor;
            case Scale::harmonicMinor:   return harmMinor;
            case Scale::pentatonicMajor: return pentMaj;
            case Scale::pentatonicMinor: return pentMin;
            case Scale::chromatic:
            default:                     return chromatic;
        }
    }
}

void PitchCorrector::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    tracker.prepare (sampleRate, analysisFrame);
    tracker.setRange (60.0f, 1200.0f);

    ringLen = juce::nextPowerOfTwo (juce::jmax (16384, maxBlockSize * 4, latency * 4));
    mask    = ringLen - 1;

    inBuf.assign  ((size_t) ringLen, 0.0f);
    outBuf.assign ((size_t) ringLen, 0.0f);
    analysisScratch.assign ((size_t) analysisFrame, 0.0f);

    // Precomputed Hann grain window at maximum length; shorter grains index
    // into it with a stride, so no windowing maths happens per sample.
    grainWindow.resize ((size_t) maxGrain);
    for (int i = 0; i < maxGrain; ++i)
        grainWindow[(size_t) i] = 0.5f - 0.5f * std::cos (
            2.0f * juce::MathConstants<float>::pi * (float) i / (float) (maxGrain - 1));

    reset();
}

void PitchCorrector::reset()
{
    std::fill (inBuf.begin(),  inBuf.end(),  0.0f);
    std::fill (outBuf.begin(), outBuf.end(), 0.0f);
    pos = 0;
    samplesUntilPitchUpdate = 0;
    currentRatio = 1.0f;
    lastDetected = lastTarget = 0.0f;
    grainPhase = 0.0f;
    voiced = false;
}

float PitchCorrector::snapToScale (float hz) const noexcept
{
    if (hz <= 0.0f) return 0.0f;

    const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
    const auto& steps = scaleSteps (scale);

    float best = midi, bestDist = 1.0e9f;
    const int baseOct = (int) std::floor ((midi - (float) root) / 12.0f);

    for (int oct = baseOct - 1; oct <= baseOct + 1; ++oct)
        for (int st : steps)
        {
            const float cand = (float) root + 12.0f * (float) oct + (float) st;
            const float d = std::abs (cand - midi);
            if (d < bestDist) { bestDist = d; best = cand; }
        }

    // Inside the dead zone the note counts as already in tune. This is what
    // makes the cleanup-half repair inaudible on a competent performance.
    if (bestDist * 100.0f < deadZoneCents)
        return hz;

    return 440.0f * std::pow (2.0f, (best - 69.0f) / 12.0f);
}

void PitchCorrector::updatePitch()
{
    const int start = (pos - analysisFrame) & mask;
    for (int i = 0; i < analysisFrame; ++i)
        analysisScratch[(size_t) i] = inBuf[(size_t) ((start + i) & mask)];

    float conf = 0.0f;
    const float f0 = tracker.process (analysisScratch.data(), &conf);

    // Slew rate is per pitch-update, not per sample.
    const double updateRate = sr / (double) (analysisFrame / 4);
    const float  coef = std::exp (-1.0f / (float) (0.001 * retuneMs * updateRate));

    if (f0 > 0.0f && conf > 0.5f)
    {
        voiced       = true;
        lastDetected = f0;
        lastTarget   = snapToScale (f0);

        const float full   = lastTarget / f0;
        const float wanted = std::pow (full, strength);
        currentRatio = coef * currentRatio + (1.0f - coef) * wanted;
    }
    else
    {
        // Unvoiced: relax to unity so consonants and breaths stay untouched.
        voiced = false;
        currentRatio = coef * currentRatio + (1.0f - coef) * 1.0f;
    }
}

void PitchCorrector::emitGrain (int centreOffsetFromWrite, int grainLen)
{
    grainLen = juce::jlimit (32, maxGrain, grainLen);
    const int half = grainLen / 2;

    // Source is centred one period behind the write head so the whole grain
    // is already buffered. Destination is `latency` ahead of the read point,
    // which guarantees every sample is written before it is read.
    const int srcCentre = (pos + centreOffsetFromWrite) & mask;
    const int dstCentre = (pos + latency) & mask;
    const int stride    = (maxGrain - 1) / juce::jmax (1, grainLen - 1);

    for (int i = 0; i < grainLen; ++i)
    {
        const float w = grainWindow[(size_t) juce::jmin (maxGrain - 1, i * stride)];
        const int src = (srcCentre - half + i) & mask;
        const int dst = (dstCentre - half + i) & mask;
        outBuf[(size_t) dst] += inBuf[(size_t) src] * w;
    }
}

void PitchCorrector::process (float* mono, int numSamples)
{
    for (int n = 0; n < numSamples; ++n)
    {
        inBuf[(size_t) pos] = mono[n];

        // Read `latency` behind the write head, then clear for reuse.
        const int readIdx = (pos - latency) & mask;
        mono[n] = outBuf[(size_t) readIdx];
        outBuf[(size_t) readIdx] = 0.0f;

        if (--samplesUntilPitchUpdate <= 0)
        {
            samplesUntilPitchUpdate = analysisFrame / 4;
            updatePitch();
        }

        // Grain scheduling. When unvoiced we still resynthesise at a fixed
        // pseudo-period with ratio 1: Hann grains of length 2P spaced P apart
        // satisfy COLA, so that path reconstructs the input exactly.
        const float period = (voiced && lastDetected > 0.0f)
                           ? (float) sr / lastDetected
                           : 256.0f;
        const float ratio  = voiced ? juce::jlimit (0.5f, 2.0f, currentRatio) : 1.0f;
        const float synthPeriod = juce::jmax (16.0f, period / ratio);

        grainPhase += 1.0f;
        if (grainPhase >= synthPeriod)
        {
            grainPhase -= synthPeriod;
            emitGrain (-(int) period, (int) (period * 2.0f));
        }

        pos = (pos + 1) & mask;
    }
}

} // namespace listenator
