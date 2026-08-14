#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "UI/DoofLookAndFeel.h"
#include "UI/MachinePanel.h"

namespace listenator
{

class ListenatorEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit ListenatorEditor (ListenatorProcessor&);
    ~ListenatorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void buildRack();

    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttach  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    struct TrimKnob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttach> attach;
    };

    void addTrim (TrimKnob&, const char* paramId, const juce::String& text);
    void addModule (juce::OwnedArray<ModuleStrip>&, const char* paramId,
                    const juce::String& name, juce::Colour accent);

    ListenatorProcessor& proc;
    DoofLookAndFeel lnf;

    // headline controls
    juce::TextButton listenButton { "LISTEN" };
    juce::ToggleButton cleanupBypass, effectsBypass;
    std::unique_ptr<ButtonAttach> cleanupAttach, effectsAttach;

    juce::ComboBox keyBox, scaleBox;
    juce::Label    keyLabel;
    std::unique_ptr<ComboAttach> keyAttach, scaleAttach;

    CrtDisplay crt;

    Gauge gainReductionGauge { "COMPRESS", "dB" };
    Gauge deEssGauge         { "SIBILANCE", "dB" };
    Gauge pitchGauge         { "PITCH", "Hz" };

    LevelStrip inputStrip  { "IN" };
    LevelStrip outputStrip { "OUT" };

    juce::OwnedArray<ModuleStrip> cleanupModules, effectsModules;
    juce::OwnedArray<std::unique_ptr<ButtonAttach>> moduleAttachments;

    TrimKnob eqTrim, compTrim, deEssTrim, cleanTrim;
    TrimKnob tuneTrim, doublerTrim, driveTrim, charTrim,
             widthTrim, delayTrim, reverbTrim, duckTrim;
    TrimKnob outputTrim;

    juce::String currentQuote;
    float mouthOpen = 0.0f;
    float tubeGlow  = 0.0f;
    int   quoteCountdown = 0;
    bool  lastHadAnalysis = false;

    juce::Rectangle<int> doctorArea, crtArea, cleanupRackArea, effectsRackArea,
                         gaugeArea, listenArea, skylineArea, logoArea, trimArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ListenatorEditor)
};

} // namespace listenator
