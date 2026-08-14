#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Analysis/AnalysisResult.h"
#include "PitchCorrector.h"

namespace listenator
{

/** Per-module bypass flags for the corrective half. */
struct CleanupBypass
{
    bool highPass = false, deNoise = false, deVerb = false, gate = false;
    bool surgicalEq = false, resonance = false, deEss = false;
    bool compressor = false, toneMatch = false, pitchRepair = false, limiter = false;

    bool operator== (const CleanupBypass& o) const noexcept
    {
        return highPass == o.highPass && deNoise == o.deNoise && deVerb == o.deVerb
            && gate == o.gate && surgicalEq == o.surgicalEq && resonance == o.resonance
            && deEss == o.deEss && compressor == o.compressor && toneMatch == o.toneMatch
            && pitchRepair == o.pitchRepair && limiter == o.limiter;
    }
    bool operator!= (const CleanupBypass& o) const noexcept { return ! (*this == o); }
};

/** Trim controls: these scale the auto-derived values, they don't replace them. */
struct CleanupTrims
{
    float eqAmount      = 1.0f;   // 0..2, scales toneMatch + surgical cuts
    float compAmount    = 1.0f;   // 0..2, scales both compressors
    float deEssAmount   = 1.0f;   // 0..2
    float cleanupAmount = 1.0f;   // 0..2, scales de-noise + de-verb depth

    bool operator== (const CleanupTrims& o) const noexcept
    {
        return eqAmount == o.eqAmount && compAmount == o.compAmount
            && deEssAmount == o.deEssAmount && cleanupAmount == o.cleanupAmount;
    }
    bool operator!= (const CleanupTrims& o) const noexcept { return ! (*this == o); }
};

//==============================================================================
/** Spectral processor shared by de-noise, de-verb and resonance suppression.

    One STFT for all three: it saves two FFT round trips per block and keeps
    the three gain computations consistent with each other.

    Overlap-add, Hann, 75% overlap, latency = one frame. Synthesis frames are
    added *ahead* of the read pointer; adding behind it would silently discard
    everything this class does.
*/
class SpectralEngine
{
public:
    static constexpr int order = 10;             // 1024
    static constexpr int size  = 1 << order;
    static constexpr int hop   = size / 4;

    void prepare (double sampleRate);
    void reset();

    void setDenoiseAmount (float a) noexcept  { denoise = juce::jlimit (0.0f, 1.0f, a); }
    void setDeverbAmount  (float a) noexcept  { deverb  = juce::jlimit (0.0f, 1.0f, a); }
    void setDeverbDecay   (float rt60) noexcept;
    /** 0..1 measure of how much tail is audible between words. */
    void setTailRatio     (float ratio) noexcept;
    /** Widens the resonance envelope so a harmonic series isn't flattened. */
    void setHarmonicSpacing (float f0Hz) noexcept;
    void setResonanceDepth (float d) noexcept { resonanceDepth = juce::jmax (0.0f, d); }

    bool isActive() const noexcept
    { return denoise > 0.0f || deverb > 0.0f || resonanceDepth > 0.0f; }

    void process (float* block, int numSamples);

    static constexpr int getLatencySamples() noexcept { return size; }

private:
    void processFrame();
    /** Measures the analysis/synthesis round-trip gain once, so the overlap-add
        normalisation never depends on a framework's internal FFT scaling. */
    void calibrateOla();

    double sr = 44100.0;
    juce::dsp::FFT fft { order };
    juce::dsp::WindowingFunction<float> win { (size_t) size,
                                              juce::dsp::WindowingFunction<float>::hann };

    // All scratch is preallocated: nothing here allocates on the audio thread.
    std::vector<float> inputRing, outputRing, frame;
    std::vector<float> noiseMin, prevGain, mag, env, gains;

    // Ring of past magnitude frames. Late reverberation is estimated from a
    // DELAYED spectrum rather than a running envelope of the current one: on
    // continuous delivery an envelope tracks the direct sound almost exactly,
    // so subtracting it removes the voice instead of the room.
    std::vector<std::vector<float>> magHistory;
    int   historyPos = 0, historyDelay = 12;
    float deverbGamma = 0.0f;

    int   pos = 0, ringLen = 0, mask = 0;
    int   samplesUntilFrame = hop;
    float denoise = 0.0f, deverb = 0.0f, resonanceDepth = 0.0f;
    float deverbDecayPerHop = 0.0f;
    int   minEnvSpanBins = 8;
    float olaNorm = 2.0f / 3.0f;
};

//==============================================================================
/** Two-stage compressor: slow RMS leveller, then fast peak control.

    The detectors differ on purpose. Stage 1's threshold comes from the
    measured integrated loudness, so it must see an RMS-like level or it fires
    a full crest factor too early. Stage 2 catches transients, so it needs peak.
*/
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
        float threshDb = -20.0f, ratio = 2.0f, kneeDb = 6.0f;
        float attackCoef = 0.0f, releaseCoef = 0.0f;
        float env = 0.0f;
    };

    static float curve (const Stage&, float detectorDb) noexcept;
    float applyStage (Stage&, float detectorDb) noexcept;

    double sr = 44100.0;
    Stage leveller, peak;
    float makeupDb = 0.0f, lastGrDb = 0.0f;
    float rmsSquared = 0.0f, rmsCoef = 0.0f;
};

//==============================================================================
/** Lookahead brickwall limiter.

    juce::dsp::Limiter is deliberately not used here: it bolts on a fixed 4:1
    compressor at -10 dBFS and adds makeup gain equal to -threshold, so it
    colours the signal even when nothing is near clipping. A safety limiter
    has to be bit-transparent below its threshold.
*/
class BrickwallLimiter
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void setThresholdDb (float db) noexcept;

    void process (juce::AudioBuffer<float>&);

    int getLatencySamples() const noexcept { return lookahead; }

private:
    double sr = 44100.0;
    int    lookahead = 0, writeIdx = 0, numCh = 0;
    float  thresholdLin = 1.0f;
    float  gain = 1.0f, attackCoef = 0.0f, releaseCoef = 0.0f;
    juce::AudioBuffer<float> delayLine;
};

//==============================================================================
/** Split-band de-esser: only the sibilant band ducks. */
class DeEsser
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void setParams (const AnalysisResult&, float amount);
    void process (juce::AudioBuffer<float>&);

    float getReductionDb() const noexcept { return lastReductionDb; }

private:
    double sr = 44100.0;
    juce::dsp::LinkwitzRileyFilter<float> lowBand, highBand;
    juce::AudioBuffer<float> sibBuffer, restBuffer;

    float thresholdDb = -28.0f, maxReductionDb = -8.0f, ratio = 4.0f;
    float attackCoef = 0.0f, releaseCoef = 0.0f, env = 0.0f;
    float lastReductionDb = 0.0f;
};

//==============================================================================
/** The corrective half.

        HPF -> [de-noise + de-verb + resonance, one STFT] -> gate
             -> surgical EQ -> compression -> de-ess -> tone match
             -> pitch repair -> limiter

    De-essing sits after compression (compression amplifies sibilance) and
    before the tone-match stage, which is where any additive HF comes from.
*/
class CleanupChain
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    void applyAnalysis (const AnalysisResult&);
    void setBypass (const CleanupBypass&);
    void setTrims  (const CleanupTrims&);

    void process (juce::AudioBuffer<float>&);

    int   getLatencySamples() const noexcept;
    float getGainReductionDb() const noexcept  { return comp.getGainReductionDb(); }
    float getDeEssReductionDb() const noexcept { return deEss.getReductionDb(); }
    float getGateGain() const noexcept         { return gateGain; }

private:
    void updateFilters();

    double sr = 44100.0;
    int    channels = 2;
    int    maxBlock = 512;
    bool   haveAnalysis = false;
    AnalysisResult analysis;
    CleanupBypass  bypass;
    CleanupTrims   trims;

    static constexpr int maxSurgical = 8;

    juce::dsp::IIR::Filter<float> hpf[2];
    std::array<juce::dsp::IIR::Filter<float>, maxSurgical>    surgical[2];
    std::array<juce::dsp::IIR::Filter<float>, numToneBands>   toneBands[2];
    int numSurgical = 0, numTone = 0;

    SpectralEngine spectral[2];
    DualCompressor comp;
    DeEsser        deEss;
    PitchCorrector repair;

    // gate, with hysteresis so it can't chatter on the threshold
    float gateOpenLin = 0.0f, gateCloseLin = 0.0f, gateGain = 1.0f;
    float gateAttackCoef = 0.0f, gateReleaseCoef = 0.0f, gateRangeLin = 0.1f;
    float gateEnv = 1.0f;
    bool  gateOpen = false;

    BrickwallLimiter limiter;
};

} // namespace listenator
