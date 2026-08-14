#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Analysis/AnalysisResult.h"

namespace listenator
{

class ListenatorProcessor;

/** The CRT porthole: spectrum, target curve, and the machine's readouts. */
class CrtDisplay : public juce::Component,
                   private juce::Timer
{
public:
    explicit CrtDisplay (ListenatorProcessor&);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    ListenatorProcessor& proc;
    float sweep = 0.0f;
    int   lastGeneration = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrtDisplay)
};

//==============================================================================
/** A labelled analogue gauge with a swinging needle. */
class Gauge : public juce::Component
{
public:
    Gauge (juce::String label, juce::String units);

    void paint (juce::Graphics&) override;
    void setValue (float normalised, juce::String readout);

private:
    juce::String name, unit, text;
    float value = 0.0f, smoothed = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Gauge)
};

//==============================================================================
/** Vertical VU strip with a peak-hold tick. */
class LevelStrip : public juce::Component
{
public:
    explicit LevelStrip (juce::String label);

    void paint (juce::Graphics&) override;
    void setLevelDb (float db);

private:
    juce::String name;
    float levelDb = -100.0f, peakDb = -100.0f;
    int   peakHoldCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelStrip)
};

//==============================================================================
/** A module row: name plate, activity lamp, and its bypass switch. */
class ModuleStrip : public juce::Component
{
public:
    ModuleStrip (juce::String name, juce::Colour accent);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::ToggleButton bypassButton;
    void setActivity (float a) { activity = a; }

private:
    juce::String moduleName;
    juce::Colour accentColour;
    float activity = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleStrip)
};

} // namespace listenator
