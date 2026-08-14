#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Analysis/VocalAnalyzer.h"
#include "DSP/CleanupChain.h"
#include "DSP/EffectsChain.h"

namespace listenator
{

/** Parameter IDs. Kept in one place so the editor and processor can't drift. */
namespace pid
{
    inline constexpr const char* listen        = "listen";
    inline constexpr const char* cleanupBypass = "cleanupBypass";
    inline constexpr const char* effectsBypass = "effectsBypass";

    inline constexpr const char* keyRoot  = "keyRoot";
    inline constexpr const char* keyScale = "keyScale";

    inline constexpr const char* eqAmount      = "eqAmount";
    inline constexpr const char* compAmount    = "compAmount";
    inline constexpr const char* deEssAmount   = "deEssAmount";
    inline constexpr const char* cleanupAmount = "cleanupAmount";

    inline constexpr const char* tuneAmount   = "tuneAmount";
    inline constexpr const char* doublerMix   = "doublerMix";
    inline constexpr const char* driveAmount  = "driveAmount";
    inline constexpr const char* characterMix = "characterMix";
    inline constexpr const char* widthAmount  = "widthAmount";
    inline constexpr const char* delayMix     = "delayMix";
    inline constexpr const char* reverbMix    = "reverbMix";
    inline constexpr const char* duckAmount   = "duckAmount";

    inline constexpr const char* outputGain = "outputGain";

    // per-module bypasses
    inline constexpr const char* bpHighPass  = "bpHighPass";
    inline constexpr const char* bpDeNoise   = "bpDeNoise";
    inline constexpr const char* bpDeVerb    = "bpDeVerb";
    inline constexpr const char* bpGate      = "bpGate";
    inline constexpr const char* bpSurgical  = "bpSurgical";
    inline constexpr const char* bpResonance = "bpResonance";
    inline constexpr const char* bpDeEss     = "bpDeEss";
    inline constexpr const char* bpComp      = "bpComp";
    inline constexpr const char* bpTone      = "bpTone";
    inline constexpr const char* bpLimiter   = "bpLimiter";

    inline constexpr const char* bpAutoTune   = "bpAutoTune";
    inline constexpr const char* bpDoubler    = "bpDoubler";
    inline constexpr const char* bpSaturation = "bpSaturation";
    inline constexpr const char* bpExciter    = "bpExciter";
    inline constexpr const char* bpCharacter  = "bpCharacter";
    inline constexpr const char* bpWidth      = "bpWidth";
    inline constexpr const char* bpDelay      = "bpDelay";
    inline constexpr const char* bpReverb     = "bpReverb";
}

//==============================================================================
class ListenatorProcessor : public juce::AudioProcessor
{
public:
    ListenatorProcessor();
    ~ListenatorProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "LISTENATOR"; }
    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- editor-facing ------------------------------------------------------
    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }
    VocalAnalyzer& getAnalyzer() noexcept { return analyzer; }

    /** Fired from the UI's big red LISTEN button. */
    void triggerListen();

    bool  hasAnalysis()   const noexcept { return analysisApplied; }
    float getListenProgress() const noexcept { return analyzer.getProgress(); }

    float getInputLevelDb()   const noexcept { return inputLevelDb.load(); }
    float getOutputLevelDb()  const noexcept { return outputLevelDb.load(); }
    float getGainReductionDb() const noexcept { return cleanup.getGainReductionDb(); }
    float getDeEssReductionDb() const noexcept { return cleanup.getDeEssReductionDb(); }
    float getDetectedHz() const noexcept { return effects.getDetectedHz(); }

    /** Copy of the last published analysis, for the UI readouts. */
    const AnalysisResult& getAnalysisResult() const noexcept { return lastResult; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void pullParameters();

    juce::AudioProcessorValueTreeState apvts;

    VocalAnalyzer analyzer;
    CleanupChain  cleanup;
    EffectsChain  effects;

    AnalysisResult lastResult;
    int  seenGeneration = 0;
    bool analysisApplied = false;

    juce::AudioBuffer<float> monoScratch;

    std::atomic<float> inputLevelDb  { -100.0f };
    std::atomic<float> outputLevelDb { -100.0f };

    double currentSr = 44100.0;
    int    currentBlock = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ListenatorProcessor)
};

} // namespace listenator
