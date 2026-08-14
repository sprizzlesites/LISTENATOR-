#pragma once
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>
#include "AnalysisResult.h"
#include "PitchTracker.h"

namespace listenator
{

/** The LISTEN pass.

    The audio thread pushes mono samples into a capture buffer; once enough
    material has been seen the analysis runs on a background thread and
    publishes an AnalysisResult. Nothing here allocates or locks on the audio
    thread.

    Everything the plugin decides statically comes out of this class. The
    dynamic stages (resonance suppressor, de-esser, dynamic EQ, de-verb)
    consume the result as a starting point but keep adapting per block.
*/
class VocalAnalyzer : private juce::Thread
{
public:
    VocalAnalyzer();
    ~VocalAnalyzer() override;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** Arm the capture. Audio-thread safe. */
    void startListening (double tempoBpm, bool tempoValid);

    /** Audio thread: feed the dry mono signal. */
    void pushSamples (const float* mono, int numSamples) noexcept;

    bool  isListening()  const noexcept { return listening.load (std::memory_order_acquire); }
    bool  isAnalysing()  const noexcept { return analysing.load (std::memory_order_acquire); }
    float getProgress()  const noexcept;

    /** Monotonically increments each time a new result is published. */
    int  getGeneration() const noexcept { return generation.load (std::memory_order_acquire); }

    /** Safe to call from the audio thread once generation has changed. */
    const AnalysisResult& getResult() const noexcept { return published; }

    static constexpr double captureSeconds = 15.0;

private:
    void run() override;
    void analyse();

    // --- analysis stages ---------------------------------------------------
    void computeLtasAndNoise (AnalysisResult&);
    void computePitchStats   (AnalysisResult&);
    void computeFormants     (AnalysisResult&);
    void computeSibilance    (AnalysisResult&);
    void measureSibilantLevel (AnalysisResult&);
    void computeResonances   (AnalysisResult&);
    void computeReverb       (AnalysisResult&);
    void computeLoudness     (AnalysisResult&);
    void deriveSettings      (AnalysisResult&);
    void solveToneFilterGains (AnalysisResult&) const;
    void solveToneGainsInto (const AnalysisResult&, bool withNotches,
                             std::array<float, numToneBands>&) const;
    /** Response of the stages that reshape the spectrum downstream of where the
        tone target is computed, so the solve can cancel them. */
    void accumulateChainResponse (const AnalysisResult&,
                                  std::array<float, numToneBands>&) const;

    double sr = 44100.0;

    // capture
    std::vector<float> capture;
    std::atomic<int>   writePos { 0 };
    int                captureLen = 0;
    std::atomic<bool>  listening { false };
    std::atomic<bool>  analysing { false };
    std::atomic<int>   generation { 0 };

    double pendingTempo = 120.0;
    bool   pendingTempoValid = false;

    AnalysisResult published;

    // scratch (background thread only)
    static constexpr int fftOrder = 12;            // 4096
    static constexpr int fftSize  = 1 << fftOrder;
    static constexpr int hopSize  = fftSize / 2;

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize,
                                                 juce::dsp::WindowingFunction<float>::hann };

    std::vector<float> fftScratch;                 // 2 * fftSize
    std::vector<float> avgPower;                   // fftSize/2 magnitudes
    std::vector<float> sibilantAvgPower;
    std::vector<float> noisePower;
    PitchTracker pitchTracker;
    std::vector<float> f0Track;

    // helpers
    void   powerToBands (const std::vector<float>& power,
                         std::array<float, numToneBands>& outDb) const;
    float  binToHz (int bin) const noexcept { return (float) (bin * sr / fftSize); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalAnalyzer)
};

} // namespace listenator
