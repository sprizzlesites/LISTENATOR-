#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Analysis/AnalysisResult.h"
#include "Analysis/PitchTracker.h"

namespace listenator
{

/** Per-module bypass flags for the corrective half. */
struct CleanupBypass
{
    bool highPass = false, deNoise = false, deVerb = false, gate = false;
    bool surgicalEq = false, resonance = false, deEss = false;
    bool compressor = false, toneMatch = false, pitchRepair = false, limiter = false;
};

/** Trim controls: these scale the auto-derived values, they don't replace them. */
struct CleanupTrims
{
    float eqAmount   = 1.0f;   // 0..2, scales toneMatch + surgical cuts
    float compAmount = 1.0f;   // 0..2, scales both compressors' gain reduction
    float deEssAmount= 1.0f;   // 0..2
    float cleanupAmount = 1.0f;// 0..2, scales de-noise + de-verb depth
};

//==============================================================================
/** Spectral processor shared by de-noise, de-verb and the resonance suppressor.

    One STFT for all three saves two full FFT round trips per block and keeps
    them phase-consistent with each other. Overlap-add, Hann, 75% overlap.
*/
class SpectralEngine
{
public:
    static constexpr int order = 10;             // 1024
    static constexpr int size  = 1 << order;
    static constexpr int hop   = size / 4;

    void prepare (double sampleRate);
    void reset();

    void setNoiseProfile (const float* magPerBin);
    void setDenoiseAmount (float a) noexcept  { denoise = a; }
    void setDeverbAmount  (float a) noexcept  { deverb  = a; }
    void setDeverbDecay   (float rt60) noexcept;
    void setResonanceDepth (float d) noexcept { resonanceDepth = d; }

    void process (float* block, int numSamples);

    int getLatencySamples() const noexcept { return size; }

private:
    void processFrame();

    double sr = 44100.0;
    juce::dsp::FFT fft { order };
    juce::dsp::WindowingFunction<float> win { (size_t) size,
                                              juce::dsp::WindowingFunction<float>::hann };

    std::vector<float> inputRing, outputRing, frame, noiseMag, revEstimate, prevMag;
    int writeIdx = 0, samplesUntilFrame = hop;

    float denoise = 0.0f, deverb = 0.0f, resonanceDepth = 0.0f;
    float deverbAlpha = 0.6f;   // late-reverb decay coefficient from RT60
};

//==============================================================================
/** Two-stage compressor: slow leveller followed by fast peak control. */
class DualCompressor
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset();
    void setParams (const AnalysisResult&, float amount);
    void process (juce::AudioBuffer<float>&);

    float getGainReductionDb() const noexcept { return lastGrDb; }

private:
    struct Stage
    {
        float threshDb = -20.0f, ratio = 2.0f;
        float attackCoef = 0.0f, releaseCoef = 0.0f;
        float env = 0.0f;
    };

    float applyStage (Stage&, float detectorDb) noexcept;

    double sr = 44100.0;
    Stage leveller, peak;
    float makeupDb = 0.0f, lastGrDb = 0.0f;
};

//==============================================================================
/** Split-band de-esser: only the sibilant band ducks, the rest passes through. */
class DeEsser
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset();
    void setParams (const AnalysisResult&, float amount);
    void process (juce::AudioBuffer<float>&);

    float getReductionDb() const noexcept { return lastReductionDb; }

private:
    double sr = 44100.0;
    juce::dsp::LinkwitzRileyFilter<float> lowBand, highBand;
    juce::dsp::IIR::Filter<float> bandIsolate[2];
    juce::AudioBuffer<float> sibBuffer, restBuffer;

    float thresholdDb = -28.0f, maxReductionDb = -8.0f;
    float attackCoef = 0.0f, releaseCoef = 0.0f, env = 0.0f;
    float lastReductionDb = 0.0f;
};

//==============================================================================
/** The whole corrective half, in the order the mixing literature agrees on:

    HPF -> de-noise -> de-verb -> gate -> surgical EQ -> resonance suppression
    -> compression -> de-ess -> tone match -> pitch repair -> limiter

    De-essing sits after compression (compression amplifies sibilance) and
    before any additive high-frequency move.
*/
class CleanupChain
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void applyAnalysis (const AnalysisResult&);
    void setBypass (const CleanupBypass& b) { bypass = b; }
    void setTrims  (const CleanupTrims& t)  { trims = t; }

    void process (juce::AudioBuffer<float>&);

    int   getLatencySamples() const noexcept;
    float getGainReductionDb() const noexcept { return comp.getGainReductionDb(); }
    float getDeEssReductionDb() const noexcept { return deEss.getReductionDb(); }
    float getGateGain() const noexcept { return gateGain; }

private:
    void updateFilters();

    double sr = 44100.0;
    int    channels = 2;
    bool   haveAnalysis = false;
    AnalysisResult analysis;
    CleanupBypass  bypass;
    CleanupTrims   trims;

    juce::dsp::IIR::Filter<float> hpf[2];
    std::vector<juce::dsp::IIR::Filter<float>> surgical[2];
    std::vector<juce::dsp::IIR::Filter<float>> toneBands[2];

    SpectralEngine spectral[2];
    DualCompressor comp;
    DeEsser        deEss;

    // gate
    float gateThreshLin = 0.0f, gateGain = 1.0f;
    float gateAttackCoef = 0.0f, gateReleaseCoef = 0.0f, gateRangeLin = 0.1f;
    float gateEnv = 0.0f;

    // pitch repair (slow, transparent)
    PitchTracker repairTracker;
    juce::AudioBuffer<float> monoScratch;

    // limiter
    juce::dsp::Limiter<float> limiter;

    float makeupLin = 1.0f;
};

} // namespace listenator
