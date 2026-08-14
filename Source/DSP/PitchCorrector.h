#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Analysis/PitchTracker.h"

namespace listenator
{

/** Musical scales the corrector can snap to. Key is chosen manually. */
enum class Scale { chromatic = 0, major, minor, harmonicMinor, pentatonicMajor, pentatonicMinor };

/** Time-domain PSOLA pitch corrector.

    Two instances exist in the plugin: one in the cleanup half configured for
    slow, inaudible intonation repair, and one in the effects half whose retune
    speed is derived from how unstable the performance actually was.

    Grains are copied verbatim from the input, so the spectral envelope --
    the formants -- is preserved by construction rather than by a correction
    step.

    Timing: output is delayed by `latency` samples relative to input. Grains
    are overlap-added *ahead* of the read pointer so every contribution lands
    before that sample is read out. Writing behind the read pointer (the
    obvious mistake) silently discards the processed signal.
*/
class PitchCorrector
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void setKey (int rootNote, Scale s) noexcept { root = rootNote; scale = s; }
    void setRetuneMs (float ms) noexcept         { retuneMs = juce::jmax (1.0f, ms); }
    void setStrength (float s) noexcept          { strength = juce::jlimit (0.0f, 1.0f, s); }

    /** Cents of deviation left uncorrected. Below this the corrector does
        nothing at all, which is what keeps the cleanup-half repair inaudible. */
    void setDeadZoneCents (float c) noexcept     { deadZoneCents = juce::jmax (0.0f, c); }

    void process (float* mono, int numSamples);

    float getDetectedHz() const noexcept { return lastDetected; }
    float getTargetHz()   const noexcept { return lastTarget; }
    int   getLatencySamples() const noexcept { return latency; }

private:
    float snapToScale (float hz) const noexcept;
    void  emitGrain (int centreOffsetFromWrite, int grainLen);
    void  updatePitch();

    static constexpr int analysisFrame = 1024;
    static constexpr int latency       = 2048;   // > half the longest grain
    static constexpr int maxGrain      = 2048;

    double sr = 44100.0;
    int    root = 0;
    Scale  scale = Scale::chromatic;
    float  retuneMs = 40.0f, strength = 1.0f, deadZoneCents = 0.0f;

    PitchTracker tracker;
    std::vector<float> inBuf, outBuf, grainWindow, analysisScratch;
    int   ringLen = 0, mask = 0;
    int   pos = 0;                 // running write index into inBuf
    int   samplesUntilPitchUpdate = 0;

    float currentRatio = 1.0f, lastDetected = 0.0f, lastTarget = 0.0f;
    float grainPhase = 0.0f;
    bool  voiced = false;
};

} // namespace listenator
