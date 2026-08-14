#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Analysis/AnalysisResult.h"
#include "Analysis/PitchTracker.h"

namespace listenator
{

struct EffectsBypass
{
    bool autoTune = false, doubler = false, saturation = false, exciter = false;
    bool character = false, width = false, delay = false, reverb = false;
};

struct EffectsTrims
{
    float tuneAmount   = 1.0f;   // 0..2 scales retune speed / strength
    float doublerMix   = 1.0f;
    float driveAmount  = 1.0f;
    float characterMix = 0.0f;   // off by default: pure taste
    float widthAmount  = 1.0f;
    float delayMix     = 1.0f;
    float reverbMix    = 1.0f;
    float duckAmount   = 1.0f;
};

/** Musical scales the autotune can snap to. Key is chosen manually. */
enum class Scale { chromatic = 0, major, minor, harmonicMinor, pentatonicMajor, pentatonicMinor };

//==============================================================================
/** Formant-preserving pitch correction.

    Two instances exist in the plugin: one in the cleanup half configured for
    slow, inaudible intonation repair, and one here in the effects half whose
    retune speed is derived from how unstable the performance actually was.

    PSOLA on the detected period, with a cepstral-envelope correction so the
    formants stay put and the voice doesn't turn into a chipmunk.
*/
class PitchCorrector
{
public:
    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void setKey (int rootNote, Scale s) noexcept { root = rootNote; scale = s; }
    void setRetuneMs (float ms) noexcept         { retuneMs = ms; }
    void setStrength (float s) noexcept          { strength = juce::jlimit (0.0f, 1.0f, s); }
    void setFormantPreserve (bool b) noexcept    { preserveFormants = b; }

    void process (float* mono, int numSamples);

    float getDetectedHz() const noexcept  { return lastDetected; }
    float getTargetHz()   const noexcept  { return lastTarget; }
    int   getLatencySamples() const noexcept { return frameSize; }

private:
    float snapToScale (float hz) const noexcept;

    static constexpr int frameSize = 1024;

    double sr = 44100.0;
    int    root = 0;
    Scale  scale = Scale::chromatic;
    float  retuneMs = 40.0f, strength = 1.0f;
    bool   preserveFormants = true;

    PitchTracker tracker;
    std::vector<float> inBuf, outBuf, window;
    int   writePos = 0, readPos = 0;
    float currentRatio = 1.0f, lastDetected = 0.0f, lastTarget = 0.0f;
    float phase = 0.0f;
};

//==============================================================================
/** Tempo-locked delay with a duck triggered by the dry vocal. */
class DuckedDelay
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void setTempo (double bpm, bool valid);
    void setMix (float m) noexcept   { mix = m; }
    void setDuck (float d) noexcept  { duckAmount = d; }

    void process (juce::AudioBuffer<float>& wet, const juce::AudioBuffer<float>& dry);

private:
    double sr = 44100.0;
    juce::AudioBuffer<float> buffer;
    int writeIdx = 0, delaySamplesL = 0, delaySamplesR = 0;
    float feedback = 0.32f, mix = 0.18f, duckAmount = 1.0f, duckEnv = 0.0f;
    juce::dsp::IIR::Filter<float> tone[2];
};

//==============================================================================
/** Tempo-sized reverb with the same ducking behaviour. */
class DuckedReverb
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void setTempo (double bpm, bool valid);
    void setMix (float m) noexcept   { mix = m; }
    void setDuck (float d) noexcept  { duckAmount = d; }

    void process (juce::AudioBuffer<float>& wet, const juce::AudioBuffer<float>& dry);

private:
    double sr = 44100.0;
    juce::Reverb reverb;   // the plain juce::Reverb, not dsp::Reverb: we need
                           // processStereo/processMono on our own scratch buffer
    juce::AudioBuffer<float> scratch;
    float mix = 0.16f, duckAmount = 1.0f, duckEnv = 0.0f;
};

//==============================================================================
/** The bypassable effects half.

    autotune -> doubler -> saturation -> exciter -> character -> width
             -> delay (ducked) -> reverb (ducked)
*/
class EffectsChain
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void applyAnalysis (const AnalysisResult&);
    void setBypass (const EffectsBypass& b) { bypass = b; }
    void setTrims  (const EffectsTrims& t)  { trims = t; updateFromTrims(); }
    void setKey (int root, Scale s);
    void setTempo (double bpm, bool valid);

    void process (juce::AudioBuffer<float>&);

    int getLatencySamples() const noexcept;
    float getDetectedHz() const noexcept { return tuner.getDetectedHz(); }
    float getTargetHz()   const noexcept { return tuner.getTargetHz(); }

private:
    void updateFromTrims();

    double sr = 44100.0;
    int    channels = 2;
    bool   haveAnalysis = false;
    AnalysisResult analysis;
    EffectsBypass  bypass;
    EffectsTrims   trims;

    PitchCorrector tuner;

    // doubler: two short, slightly detuned taps panned apart
    juce::AudioBuffer<float> doubleBuf;
    int   dblWrite = 0;
    float dblPhase[2] { 0.0f, 1.7f };

    // saturation / exciter
    float drive = 0.0f, exciterAmount = 0.0f;
    juce::dsp::IIR::Filter<float> exciterHp[2];

    // character
    juce::dsp::IIR::Filter<float> charBand[2];

    DuckedDelay  delay;
    DuckedReverb reverb;

    juce::AudioBuffer<float> dryCopy;
};

} // namespace listenator
