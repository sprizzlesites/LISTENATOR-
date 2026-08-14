#pragma once
#include <array>
#include <vector>

namespace listenator
{

/** Number of log-spaced bands the tonal analysis is reduced to (1/3 octave,
    20 Hz .. 20 kHz). Everything tone-related is expressed on this grid so the
    measured spectrum and the target curve are directly subtractable. */
static constexpr int numToneBands = 31;

/** Centre frequencies of the 1/3-octave grid (ISO 266 preferred numbers). */
static constexpr std::array<float, numToneBands> toneBandHz {
    20.f,   25.f,   31.5f,  40.f,   50.f,   63.f,   80.f,   100.f,
    125.f,  160.f,  200.f,  250.f,  315.f,  400.f,  500.f,  630.f,
    800.f,  1000.f, 1250.f, 1600.f, 2000.f, 2500.f, 3150.f, 4000.f,
    5000.f, 6300.f, 8000.f, 10000.f,12500.f,16000.f,20000.f
};

/** Target long-term average spectrum for a professionally mixed lead vocal, in
    dB relative to the 200 Hz-2 kHz average. Lives here rather than inside the
    analyser so the verification harness can check the OUTPUT against the same
    curve the correction was derived from. */
inline constexpr std::array<float, numToneBands> kTargetLtasDb {
    -42.0f, -38.0f, -34.0f, -29.0f, -24.0f, -18.0f, -12.0f,  -7.0f,  // 20..100
     -3.5f,  -1.5f,  -0.5f,   0.0f,   0.3f,   0.3f,   0.0f,  -0.8f,  // 125..630
     -1.8f,  -2.8f,  -3.8f,  -4.8f,  -5.4f,  -5.8f,  -6.0f,  -6.6f,  // 800..4k
     -8.5f, -10.5f, -13.0f, -16.0f, -19.5f, -24.0f, -32.0f           // 5k..20k
};

/** A static notch the surgical EQ should apply, derived from resonance detection. */
struct Resonance
{
    float frequencyHz = 0.0f;
    float gainDb      = 0.0f;   // negative
    float q           = 0.0f;
};

/** Everything the LISTEN pass derives. Written once by the analysis thread,
    then read by the audio thread under an atomic generation counter. */
struct AnalysisResult
{
    bool  valid = false;

    // ---- level / dynamics -------------------------------------------------
    float peakDb          = 0.0f;
    float rmsDb           = 0.0f;
    float integratedLufs  = 0.0f;
    float crestFactorDb   = 0.0f;   // peak - rms, drives attack/release
    float loudnessRangeDb = 0.0f;   // drives leveller ratio

    // ---- noise / room -----------------------------------------------------
    float noiseFloorDb    = -90.0f; // minimum-statistics estimate
    float snrDb           = 90.0f;
    float speechFloorDb   = -40.0f;   // level of the quietest real delivery
    float rt60Seconds     = 0.0f;   // 0 == anechoic
    float reverbRatio     = 0.0f;   // late energy / direct energy, 0..1
    float tailDb          = -40.0f; // tail level ~165 ms after an offset
    float directToReverbDb = -12.0f; // reverb level DURING speech, back-extrapolated
    int   tailSamples     = 0;      // usable gaps the measurement found

    // ---- pitch ------------------------------------------------------------
    float medianF0Hz      = 0.0f;
    float f0P05Hz         = 0.0f;   // 5th percentile, sets the HPF
    float f0P95Hz         = 0.0f;
    float pitchStabilityCents = 0.0f; // median |deviation| from nearest semitone
    float voicedFraction  = 0.0f;

    // ---- timbre -----------------------------------------------------------
    float f1Hz = 0.0f, f2Hz = 0.0f, f3Hz = 0.0f;  // LPC formants
    float spectralCentroidHz = 0.0f;
    float spectralTiltDbPerOct = 0.0f;

    // ---- derived processor settings --------------------------------------
    float highPassHz      = 80.0f;
    float gateThresholdDb = -60.0f;
    float gateRangeDb     = -20.0f;
    float gateAttackMs    = 1.0f;
    float gateReleaseMs   = 120.0f;

    float denoiseAmount   = 0.0f;   // 0..1, scaled by measured SNR
    float deverbAmount    = 0.0f;   // 0..1, scaled by measured RT60

    float deEssCentreHz   = 6500.0f;
    float deEssBandwidthOct = 1.2f;
    float deEssThresholdDb = -28.0f;
    float deEssMaxReductionDb = -8.0f;
    float sibilantPeakDb  = -20.0f;   // dBFS, band-filtered envelope
    float sibilantSpreadDb = 12.0f;   // how far esses stick out of the body

    // two compressor stages: slow leveller then fast peak control
    float compLevelThreshDb = -24.0f, compLevelRatio = 2.0f;
    float compLevelAttackMs = 30.0f, compLevelReleaseMs = 300.0f;
    float compPeakThreshDb  = -12.0f, compPeakRatio  = 4.0f;
    float compPeakAttackMs  = 5.0f,  compPeakReleaseMs  = 80.0f;
    float makeupGainDb      = 0.0f;

    float saturationDrive = 0.0f;   // 0..1 from measured dryness/dynamics

    std::vector<Resonance> resonances;              // surgical notches
    /** Diagnostic: the strongest stationary peaks considered, whether or not
        they survived de-duplication. Lets an empty result be explained. */
    std::vector<Resonance> resonanceCandidates;
    std::array<float, numToneBands> measuredLtasDb {};  // what came in
    std::array<float, numToneBands> noiseLtasDb    {};  // spectrum of the quiet frames
    std::array<float, numToneBands> toneMatchDb    {};  // correction we want
    /** Per-filter gains that actually PRODUCE toneMatchDb once the overlap
        between neighbouring 1/3-octave peaking filters is accounted for.
        Solved on the analysis thread; the audio thread just uses them. */
    std::array<float, numToneBands> toneFilterGainDb {};
    /** The same solve done on the assumption the surgical notches are bypassed.
        Their skirts reach into neighbouring bands, so a set of gains solved
        around them is wrong the moment they are switched off. */
    std::array<float, numToneBands> toneFilterGainDbNoNotch {};

    // ---- host-derived -----------------------------------------------------
    double tempoBpm = 120.0;
    bool   tempoValid = false;
};

} // namespace listenator
