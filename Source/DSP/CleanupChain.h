#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Analysis/AnalysisResult.h"
#include "PitchCorrector.h"   // Scale enum only; the cleanup half does no pitch work

namespace listenator
{

/** Per-module bypass flags for the corrective half. */
struct CleanupBypass
{
    bool deClip = false, plosive = false;
    bool highPass = false, deNoise = false, deVerb = false, gate = false;
    bool upward = false;
    bool surgicalEq = false, resonance = false, deEss = false;
    bool compressor = false, toneMatch = false, limiter = false;

    bool operator== (const CleanupBypass& o) const noexcept
    {
        return deClip == o.deClip && plosive == o.plosive
            && highPass == o.highPass && deNoise == o.deNoise && deVerb == o.deVerb
            && gate == o.gate && upward == o.upward
            && surgicalEq == o.surgicalEq && resonance == o.resonance
            && deEss == o.deEss && compressor == o.compressor && toneMatch == o.toneMatch
            && limiter == o.limiter;
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
    /** Late-field level relative to direct, in dB (negative). This is an
        AMPLITUDE relationship, not a 0..1 severity score -- feeding a
        normalised liveness value here tells the subtractor that most of every
        sustained syllable is reverb, and it removes the voice. */
    void setTailDb        (float tailDb) noexcept;
    /** Widens the resonance envelope so a harmonic series isn't flattened. */
    void setHarmonicSpacing (float f0Hz) noexcept;
    void setResonanceDepth (float d) noexcept { resonanceDepth = juce::jmax (0.0f, d); }

    /** Worst per-bin cut the resonance suppressor applied, in dB. */
    float getResonanceReductionDb() const noexcept { return worstResonanceDb; }
    /** Number of bins it acted on in the last frame. */
    int   getResonanceBinCount()    const noexcept { return resonanceBins; }

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
    std::vector<float> noiseMin, prevGain, mag, env, gains, smoothGains;

    // Ring of past magnitude frames. Late reverberation is estimated from a
    // DELAYED spectrum rather than a running envelope of the current one: on
    // continuous delivery an envelope tracks the direct sound almost exactly,
    // so subtracting it removes the voice instead of the room.
    std::vector<std::vector<float>> magHistory;
    std::vector<float> lateAccum;      // running estimate of the decaying tail
    int   historyPos = 0, historyDelay = 12;
    float deverbGamma = 0.0f;

    int   pos = 0, ringLen = 0, mask = 0;
    int   samplesUntilFrame = hop;
    float denoise = 0.0f, deverb = 0.0f, resonanceDepth = 0.0f;
    float deverbDecayPerHop = 0.0f;
    float deverbMaxCut = 0.5f;   // deepest per-bin subtraction, set from RT60
    float worstResonanceDb = 0.0f;
    int   resonanceBins = 0;
    int   minEnvSpanBins = 8;
    float olaNorm = 2.0f / 3.0f;
};

//==============================================================================
/** Two-stage compressor, each stage working at a different rate.

    Splitting by rate is what lets it be consistent without sounding squashed:

      take    2.5 s release  reconciles punch-ins recorded at different levels
      peak    110 ms         catches transients only

    Nothing here runs at syllable rate (2-8 Hz) on purpose. A compressor whose
    release lands in that band flattens the envelope, and a flattened envelope
    fills the valleys between syllables -- perceptually the same thing
    reverberation does, which is the opposite of the job.

    The detectors differ too: the take and phrase stages have thresholds derived
    from loudness measurements, so they need RMS; the peak stage needs peak.
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
/** Repairs flat-topped samples before anything else sees them.

    A clipped peak is a horizontal run at full scale. Left alone it feeds
    harmonic junk into every later stage and makes the compressor read a
    transient that is not really there. Short runs are interpolated across from
    the surrounding waveform, which will not recover the original peak but does
    remove the corner that generates the distortion.
*/
class DeClipper
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset();
    void setThreshold (float lin) noexcept { threshold = lin; }

    void process (juce::AudioBuffer<float>&);

    int getRepairedCount() const noexcept { return repaired; }

private:
    float threshold = 0.985f;
    int   maxRun = 64;
    int   repaired = 0;
};

//==============================================================================
/** Ducks low-frequency bursts that are not part of the voice.

    Plosives, mic-stand knocks, footsteps and boom-arm bumps all look the same:
    a sudden spike of energy below ~150 Hz with no matching rise higher up. A
    sung note that is genuinely loud down there raises the whole spectrum, so
    the RATIO of low to full band is what separates them -- a fixed low cut
    would just thin every low note as well.
*/
class PlosiveGuard
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void setAmount (float a) noexcept { amount = juce::jlimit (0.0f, 1.0f, a); }
    void setParams (const AnalysisResult&) noexcept;
    /** Bands only; used by the analyser's measurement pass and by the tests. */
    void setBands (float cornerHz, float upperHz) noexcept;

    void process (juce::AudioBuffer<float>&);

    float getReductionDb() const noexcept { return lastReductionDb; }
    float getPeakBoost()   const noexcept { return peakLfBoost; }

    /** The transient detector, exposed so the analyser can run the identical
        thing over the capture instead of guessing how sensitive the guard
        should be.

        What identifies a pop is not that the low band jumped -- a sung note
        does that too -- but that it jumped and the band above it did NOT. How
        far a pop stands above the background depends entirely on how loud the
        singer was a moment earlier, so keying depth off that figure makes the
        guard's strength a function of the arrangement. The low-to-high contrast
        does not have that problem. */
    struct Detector
    {
        struct Result { float lfBoost, hfBoost, contrast; };

        void prepare (double sr) noexcept;
        void reset() noexcept;
        Result push (float lo, float hi) noexcept;

        float lfFast = 0.0f, lfSlow = 0.0f, hfFast = 0.0f, hfSlow = 0.0f;
        float fastAtk = 0.0f, fastRel = 0.0f, slowAtk = 0.0f, slowRel = 0.0f;
    };

private:
    double sr = 44100.0;
    // Two crossovers, three bands. A single split ducks either too little (the
    // corner sits under the high-pass, so the guard removes what was leaving
    // anyway) or too much (a full cut through the fundamental thins every word
    // that starts with a consonant).
    juce::dsp::LinkwitzRileyFilter<float> splitLow, splitLowHi, splitUp, splitUpHi;
    // The sub band never passes through the SECOND crossover, so its phase no
    // longer matches the two bands that did, and summing the three notches the
    // low-mids even with every gain at unity -- measured at 2.5 dB on a sung
    // note. An allpass at the upper corner puts the phases back together.
    juce::dsp::LinkwitzRileyFilter<float> subAllpass;
    juce::AudioBuffer<float> subBuf, midBuf, topBuf;

    float amount = 1.0f;
    float depthLin = 0.2f;      // deepest duck for the sub band
    float sensitivity = 2.0f;   // LF transient ratio that counts as a pop

    Detector detector;
    float gain = 1.0f, attackCoef = 0.0f, releaseCoef = 0.0f;
    int   holdSamples = 0, holdLeft = 0;
    float lastReductionDb = 0.0f;
    float peakLfBoost = 0.0f;
};

//==============================================================================
/** Upward expander: lifts quiet delivery toward the take average.

    Downward compression can only bring the loud parts down to meet the quiet
    ones. Past a certain amount that is exactly what makes a vocal sound choppy
    -- the loud syllables get flattened while the quiet ones stay where they
    were, so the ear hears the processing rather than the performance. Raising
    the floor instead gets to the same consistency for less gain reduction.

    Everything is relative to a running programme level rather than absolute,
    because the whole problem on a punch-in recording is that "quiet" means
    different things in different takes. The threshold, the floor below which
    the lift tapers off (so room tone and hiss are never lifted) and the maximum
    boost all hang off that follower.
*/
class UpwardExpander
{
public:
    void prepare (double sampleRate, int numChannels);
    void reset();
    void setParams (const AnalysisResult&, float amount);
    /** @param gateWeight  per-sample gate gain, 0..1. The lift is scaled by it so
                           the stage never puts back what the downward expander
                           just took out -- their working ranges overlap, and
                           left independent they spend most of a syllable decay
                           pulling against each other. Null means "wide open". */
    void process (juce::AudioBuffer<float>&, const float* gateWeight);

    float getBoostDb()    const noexcept { return lastBoostDb; }
    float getMaxBoostDb() const noexcept { return peakBoostDb; }

private:
    double sr = 44100.0;
    float envSq = 0.0f, envAtkCoef = 0.0f, envRelCoef = 0.0f;
    float refLin = 0.0f, refCoef = 0.0f, seedLin = 0.0f;
    float gain = 1.0f, gainAtkCoef = 0.0f, gainRelCoef = 0.0f;

    float thresholdOffsetDb = -4.0f;
    float floorOffsetDb     = -12.0f;
    float fadeDb            = 6.0f;
    float slope             = 0.375f;   // 1 - 1/ratio
    float maxBoostDb        = 6.0f;
    float amount            = 1.0f;

    float lastBoostDb = 0.0f, peakBoostDb = 0.0f;
};

//==============================================================================
/** Takes the edge off attack transients without touching the body of a word.

    A corrective chain adds transient emphasis whether or not anyone asked for
    it: the tone stage lifts the top end to meet the target curve, and a
    consonant is mostly top end, so it comes up more than the vowel behind it.
    Measured against the source, the chain was making the leading 15 ms of a
    word 0.8 dB louder relative to the word, and 1.1 dB above 5 kHz.

    A de-esser cannot fix that. It reduces the LEVEL of a sibilant, but it
    reduces the attack and the body together, so the ratio between them -- which
    is what "spitty" actually means -- survives. What flattens that ratio is a
    detector that compares a fast envelope against a slow one and acts only on
    the difference.

    Lookahead is not optional here. Without it the reduction can only start
    after the transient has arrived, which leaves the first millisecond -- the
    loudest part -- untouched.
*/
class TransientSoftener
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    void setParams (const AnalysisResult&, float amount);

    void process (juce::AudioBuffer<float>&);

    int   getLatencySamples() const noexcept { return lookahead; }
    float getReductionDb()    const noexcept { return lastReductionDb; }

private:
    double sr = 44100.0;
    int    numCh = 1, lookahead = 0, writeIdx = 0;
    juce::AudioBuffer<float> delayLine;

    float fastEnv = 0.0f, slowEnv = 0.0f;
    float fastAtk = 0.0f, fastRel = 0.0f, slowAtk = 0.0f, slowRel = 0.0f;
    float gain = 1.0f, gainAtk = 0.0f, gainRel = 0.0f;

    float threshDb = 4.0f, depthDb = 0.0f, slope = 0.5f;
    float lastReductionDb = 0.0f;
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
/** Tone matching as a closed loop rather than an open one.

    The static curve solved by the LISTEN pass is only ever a starting point.
    Two things make an open-loop curve wrong in practice:

      - it is derived from fifteen seconds of capture, and fifteen seconds of a
        three-minute take is not the take. Measured on real material the
        low-mids of the capture ran 3-5 dB hotter than the file average, and the
        tone stage happily cut a hole that only existed in the sample.
      - a singer who turns off-axis, leans in, or punches in from a different
        position brings a different spectrum with them. No single static curve
        is right for all of them.

    So the stage measures its own OUTPUT, compares it to the target curve and
    integrates the difference back into the filter gains. Filter overlap,
    whatever the stages upstream did, and drift over the take all fall out of
    the loop automatically, because they are all just error.

    Deliberately slow: the integrator gain and the measurement window are set so
    the loop settles over seconds. Anything quick enough to follow a phrase
    would flatten the tonal difference between phrases, which is performance,
    not a fault.
*/
class AdaptiveToneMatch
{
public:
    static constexpr int fftOrder = 12;          // 4096: the resolution the
    static constexpr int fftSize  = 1 << fftOrder;   // LISTEN pass measures at
    static constexpr int hopSize  = fftSize / 2;

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    /** Where the loop starts. `amount` is the EQ trim knob. */
    void setBase (const std::array<float, numToneBands>& baseDb, float amount);
    void setNoiseFloorDb (float db) noexcept { noiseFloorDb = db; }

    /** Filters only. */
    void process (juce::AudioBuffer<float>&);
    /** Feeds the loop what it should be matching. Split from process() so the
        measurement can be taken at the END of the chain rather than at this
        stage's own output -- otherwise anything after it is invisible to the
        loop and the finished signal misses the target by whatever that stage
        did. */
    void observe (const juce::AudioBuffer<float>&);

    /** Largest departure the loop has made from the static curve, in dB. */
    float getMaxTrimDb() const noexcept { return maxTrimDb; }
    /** Live filter gains, for diagnostics. */
    const std::array<float, numToneBands>& getGainsDb() const noexcept { return gainDb; }
    int   getUpdateCount() const noexcept { return updates; }

private:
    void pushAnalysis (const float* x, int n);
    void analyseFrame();
    void integrate();
    void applyGains();

    double sr = 44100.0;
    int    channels = 1;

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> win { (size_t) fftSize,
                                              juce::dsp::WindowingFunction<float>::hann };

    std::vector<float> ring, scratch, powerAccum;
    int   ringPos = 0, sinceHop = 0, framesSeen = 0, framesSinceUpdate = 0;
    float progPeak = 0.0f, peakDecay = 0.0f, accumLeak = 0.0f;
    int   framesPerUpdate = 12;

    std::array<float, numToneBands> base {}, gainDb {}, measuredDb {};
    std::array<juce::dsp::IIR::Filter<float>, numToneBands> filters[2];
    std::array<bool, numToneBands> active {};

    float noiseFloorDb = -90.0f;
    float maxTrimDb = 0.0f;
    int   updates = 0;
    bool  seeded = false;
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
    int   getLatencySamples() const noexcept { return lookahead; }

private:
    double sr = 44100.0;
    juce::dsp::LinkwitzRileyFilter<float> lowBand, highBand;
    juce::AudioBuffer<float> sibBuffer, restBuffer;

    float thresholdDb = -28.0f, maxReductionDb = -8.0f, ratio = 4.0f;
    float attackCoef = 0.0f, releaseCoef = 0.0f, env = 0.0f;
    float lastReductionDb = 0.0f;

    // Lookahead on the sibilant band only. A de-esser without it always lets
    // the leading edge of an ess through at full level -- the detector cannot
    // know about a transient until it has arrived -- and that leading edge is
    // exactly the part that reads as a spitty consonant.
    juce::AudioBuffer<float> sibDelay;
    int lookahead = 0, delayWrite = 0;
};

//==============================================================================
/** The corrective half.

        de-clip -> plosive guard -> HPF
             -> [de-noise + de-verb + resonance, one STFT] -> gate
             -> upward expander -> surgical EQ -> compression
             -> de-ess -> tone match -> limiter

    The upward expander sits between the gate and the compressor on purpose:
    after the gate, so the room tone it just pulled down is not lifted straight
    back; before the compressor, so the compressor arrives at a signal that
    already needs less gain reduction.

    The corrective half does no pitch work at all: correcting intonation is an
    effect, and it belongs on the other side of the bypass.

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
    float getTransientReductionDb() const noexcept { return softener.getReductionDb(); }
    float getGateGain() const noexcept         { return gateGain; }
    float getPlosiveReductionDb() const noexcept { return plosive.getReductionDb(); }
    float getPlosivePeakBoost()  const noexcept  { return plosive.getPeakBoost(); }
    float getResonanceReductionDb() const noexcept { return spectral[0].getResonanceReductionDb(); }
    int   getResonanceBinCount()    const noexcept { return spectral[0].getResonanceBinCount(); }
    int   getDeclippedCount() const noexcept     { return deClip.getRepairedCount(); }
    float getUpwardBoostDb() const noexcept      { return upward.getMaxBoostDb(); }
    float getToneTrimDb() const noexcept         { return tone.getMaxTrimDb(); }
    const std::array<float, numToneBands>& getToneGainsDb() const noexcept { return tone.getGainsDb(); }
    int   getToneUpdateCount() const noexcept    { return tone.getUpdateCount(); }

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

    // Three cascaded Butterworth sections: 36 dB/oct.
    //
    // The steepness is free. A sixth-order Butterworth is 0.02 dB down half an
    // octave above its corner where a fourth-order one is 0.4 dB down, but 4.4
    // dB further down an octave below it. The tone bank cannot make up that
    // difference: the target curve falls 6 dB per third-octave under 100 Hz,
    // which is a steeper slope than a bank of third-octave peaking filters can
    // synthesise once it is smoothed enough not to comb.
    juce::dsp::IIR::Filter<float> hpf[2], hpf2[2], hpf3[2];
    std::array<juce::dsp::IIR::Filter<float>, maxSurgical>    surgical[2];
    int numSurgical = 0;
    AdaptiveToneMatch tone;

    DeClipper      deClip;
    PlosiveGuard   plosive;
    SpectralEngine spectral[2];
    UpwardExpander upward;
    DualCompressor comp;
    DeEsser        deEss;
    TransientSoftener softener;

    // Gate / expander.
    //
    // Two thresholds, and the operating one is whichever sits higher:
    //   absolute -- from the measured noise floor, kills hiss in true silence
    //   relative -- tracks the running programme level, pulls down the room
    //               tail between words
    //
    // The relative one is what a fixed gate cannot do on this material. With
    // punch-ins recorded 15 dB apart, the reverb tail of a loud take sits at
    // the same absolute level as a quiet take's direct sound, so no fixed
    // threshold can separate them. One that follows the programme can.
    float gateOpenLin = 0.0f, gateCloseLin = 0.0f, gateGain = 1.0f;
    float gateAttackCoef = 0.0f, gateReleaseCoef = 0.0f, gateRangeLin = 0.1f;
    float gateEnv = 1.0f;
    bool  gateOpen = false;

    // The detector is an ENVELOPE, not the sample value. Comparing |x| against
    // a threshold means the comparison flips twice per cycle of the
    // fundamental, so near the threshold the gate opens and shuts at audio
    // rate -- which is what made this stage the largest single source of
    // frame-to-frame spectral instability in the whole chain.
    float gateDetEnv = 0.0f, gateDetAtkCoef = 0.0f, gateDetRelCoef = 0.0f;
    // ...and a hold, so a stop consonant inside a word cannot re-trigger it.
    int   gateHoldSamples = 0, gateHoldLeft = 0;
    std::vector<float> gateTrace;   // per-sample gate gain, handed to the lift

    float progEnv = 0.0f, progReleaseCoef = 0.0f;
    float relativeOffsetLin = 0.0f;   // threshold as a fraction of programme
    float expanderRatio = 2.0f;

    BrickwallLimiter limiter;
};

} // namespace listenator
