#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Analysis/AnalysisResult.h"
#include "PitchCorrector.h"

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
    void setBypass (const EffectsBypass& b) { bypass = b; updateFromTrims(); }
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
    float baseRetuneMs = 40.0f, baseStrength = 0.8f;

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
