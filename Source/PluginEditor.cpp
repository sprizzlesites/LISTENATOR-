#include "PluginEditor.h"
#include "UI/DoofArt.h"

namespace listenator
{

using namespace doof;

//==============================================================================
ListenatorEditor::ListenatorEditor (ListenatorProcessor& p)
    : AudioProcessorEditor (&p), proc (p), crt (p)
{
    setLookAndFeel (&lnf);

    // ---- headline controls -------------------------------------------------
    listenButton.setColour (juce::TextButton::buttonColourId, dangerRed);
    listenButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    listenButton.onClick = [this]
    {
        proc.triggerListen();
        currentQuote = doofart::quoteForListening();
        quoteCountdown = 150;
    };
    addAndMakeVisible (listenButton);

    cleanupBypass.setButtonText ("CLEAN-INATOR");
    effectsBypass.setButtonText ("FX-INATOR");
    addAndMakeVisible (cleanupBypass);
    addAndMakeVisible (effectsBypass);

    auto& state = proc.getState();
    cleanupAttach = std::make_unique<ButtonAttach> (state, pid::cleanupBypass, cleanupBypass);
    effectsAttach = std::make_unique<ButtonAttach> (state, pid::effectsBypass, effectsBypass);

    // key + scale (manual, as chosen)
    for (auto* n : { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" })
        keyBox.addItem (n, keyBox.getNumItems() + 1);
    for (auto* s : { "Chromatic","Major","Minor","Harm Minor","Pent Major","Pent Minor" })
        scaleBox.addItem (s, scaleBox.getNumItems() + 1);

    addAndMakeVisible (keyBox);
    addAndMakeVisible (scaleBox);
    keyAttach   = std::make_unique<ComboAttach> (state, pid::keyRoot, keyBox);
    scaleAttach = std::make_unique<ComboAttach> (state, pid::keyScale, scaleBox);

    keyLabel.setText ("TARGET KEY", juce::dontSendNotification);
    keyLabel.setJustificationType (juce::Justification::centred);
    keyLabel.setFont (DoofLookAndFeel::machineFont (10.0f, true));
    addAndMakeVisible (keyLabel);

    addAndMakeVisible (crt);
    addAndMakeVisible (gainReductionGauge);
    addAndMakeVisible (deEssGauge);
    addAndMakeVisible (pitchGauge);
    addAndMakeVisible (inputStrip);
    addAndMakeVisible (outputStrip);

    buildRack();

    // ---- trim knobs --------------------------------------------------------
    addTrim (eqTrim,      pid::eqAmount,      "EQ");
    addTrim (compTrim,    pid::compAmount,    "COMP");
    addTrim (deEssTrim,   pid::deEssAmount,   "DE-ESS");
    addTrim (cleanTrim,   pid::cleanupAmount, "CLEAN");
    addTrim (tuneTrim,    pid::tuneAmount,    "TUNE");
    addTrim (doublerTrim, pid::doublerMix,    "DOUBLE");
    addTrim (driveTrim,   pid::driveAmount,   "DRIVE");
    addTrim (charTrim,    pid::characterMix,  "CHAR");
    addTrim (widthTrim,   pid::widthAmount,   "WIDTH");
    addTrim (delayTrim,   pid::delayMix,      "DELAY");
    addTrim (reverbTrim,  pid::reverbMix,     "VERB");
    addTrim (duckTrim,    pid::duckAmount,    "DUCK");
    addTrim (outputTrim,  pid::outputGain,    "OUTPUT");

    currentQuote = doofart::quoteForIdle();

    setSize (1100, 780);
    startTimerHz (30);
}

ListenatorEditor::~ListenatorEditor()
{
    setLookAndFeel (nullptr);
}

void ListenatorEditor::addTrim (TrimKnob& k, const char* paramId, const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible (k.slider);

    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (DoofLookAndFeel::machineFont (9.0f, true));
    k.label.setColour (juce::Label::textColourId, mintBright);
    addAndMakeVisible (k.label);

    k.attach = std::make_unique<SliderAttach> (proc.getState(), paramId, k.slider);
}

void ListenatorEditor::addModule (juce::OwnedArray<ModuleStrip>& rack, const char* paramId,
                                  const juce::String& name, juce::Colour accent)
{
    auto* strip = new ModuleStrip (name, accent);
    rack.add (strip);
    addAndMakeVisible (strip);

    moduleAttachments.add (new std::unique_ptr<ButtonAttach> (
        std::make_unique<ButtonAttach> (proc.getState(), paramId, strip->bypassButton)));
}

void ListenatorEditor::buildRack()
{
    addModule (cleanupModules, pid::bpDeClip,    "DE-CLIP",      mintGreen);
    addModule (cleanupModules, pid::bpPlosive,   "PLOSIVE GUARD",mintGreen);
    addModule (cleanupModules, pid::bpHighPass,  "HIGH-PASS",    mintGreen);
    addModule (cleanupModules, pid::bpDeNoise,   "DE-NOISE",     mintGreen);
    addModule (cleanupModules, pid::bpDeVerb,    "DE-VERB",      mintGreen);
    addModule (cleanupModules, pid::bpGate,      "GATE",         mintGreen);
    addModule (cleanupModules, pid::bpSurgical,  "SURGICAL EQ",  mintGreen);
    addModule (cleanupModules, pid::bpResonance, "RESONANCE",    mintGreen);
    addModule (cleanupModules, pid::bpComp,      "COMPRESSOR",   mintGreen);
    addModule (cleanupModules, pid::bpDeEss,     "DE-ESSER",     mintGreen);
    addModule (cleanupModules, pid::bpTone,      "TONE MATCH",   mintGreen);
    addModule (cleanupModules, pid::bpLimiter,   "LIMITER",      mintGreen);

    addModule (effectsModules, pid::bpAutoTune,   "AUTO-TUNE",   purpleLight);
    addModule (effectsModules, pid::bpDoubler,    "DOUBLER",     purpleLight);
    addModule (effectsModules, pid::bpSaturation, "SATURATION",  purpleLight);
    addModule (effectsModules, pid::bpExciter,    "AIR EXCITER", purpleLight);
    addModule (effectsModules, pid::bpCharacter,  "CHARACTER",   purpleLight);
    addModule (effectsModules, pid::bpWidth,      "WIDTH",       purpleLight);
    addModule (effectsModules, pid::bpDelay,      "DELAY",       purpleLight);
    addModule (effectsModules, pid::bpReverb,     "REVERB",      purpleLight);
}

//==============================================================================
void ListenatorEditor::timerCallback()
{
    inputStrip.setLevelDb (proc.getInputLevelDb());
    outputStrip.setLevelDb (proc.getOutputLevelDb());

    const float gr = proc.getGainReductionDb();
    gainReductionGauge.setValue (juce::jlimit (0.0f, 1.0f, -gr / 18.0f),
                                 juce::String (gr, 1) + " dB");

    const float de = proc.getDeEssReductionDb();
    deEssGauge.setValue (juce::jlimit (0.0f, 1.0f, -de / 12.0f),
                         juce::String (de, 1) + " dB");

    const float hz = proc.getDetectedHz();
    pitchGauge.setValue (juce::jlimit (0.0f, 1.0f, hz / 800.0f),
                         hz > 1.0f ? juce::String ((int) hz) + " Hz" : "--");

    // module activity lamps
    if (! cleanupModules.isEmpty())
    {
        cleanupModules[6]->setActivity (juce::jlimit (0.0f, 1.0f, -gr / 12.0f));
        cleanupModules[7]->setActivity (juce::jlimit (0.0f, 1.0f, -de / 8.0f));
    }

    const float lvl = juce::jlimit (0.0f, 1.0f, (proc.getOutputLevelDb() + 60.0f) / 60.0f);
    tubeGlow = tubeGlow * 0.8f + lvl * 0.2f;
    mouthOpen = mouthOpen * 0.75f + lvl * 0.25f;

    // the doctor comments on state changes
    const bool hasAnalysis = proc.hasAnalysis();
    if (hasAnalysis && ! lastHadAnalysis)
    {
        currentQuote = doofart::quoteForAnalysed();
        quoteCountdown = 180;
    }
    lastHadAnalysis = hasAnalysis;

    if (quoteCountdown > 0 && --quoteCountdown == 0)
        currentQuote = hasAnalysis ? doofart::quoteForAnalysed() : doofart::quoteForIdle();

    repaint();
}

//==============================================================================
void ListenatorEditor::resized()
{
    auto r = getLocalBounds();

    logoArea    = r.removeFromTop (74);
    skylineArea = r.removeFromBottom (56);

    // bottom racks
    auto racks = r.removeFromBottom (272);
    cleanupRackArea = racks.removeFromLeft (racks.getWidth() / 2).reduced (8, 6);
    effectsRackArea = racks.reduced (8, 6);

    // upper section: doctor | crt+listen | gauges
    doctorArea = r.removeFromLeft (200);
    gaugeArea  = r.removeFromRight (215);

    auto centre = r.reduced (8, 6);
    listenArea = centre.removeFromBottom (86);
    crtArea    = centre;

    crt.setBounds (crtArea.reduced (4));

    // ---- listen row --------------------------------------------------------
    auto lr = listenArea.reduced (6, 8);
    listenButton.setBounds (lr.removeFromLeft (128).reduced (2, 4));
    lr.removeFromLeft (10);

    auto keyCol = lr.removeFromLeft (150);
    keyLabel.setBounds (keyCol.removeFromTop (14));
    keyBox.setBounds   (keyCol.removeFromLeft (58).reduced (2));
    scaleBox.setBounds (keyCol.reduced (2));

    lr.removeFromLeft (8);
    cleanupBypass.setBounds (lr.removeFromTop (lr.getHeight() / 2).reduced (2));
    effectsBypass.setBounds (lr.reduced (2));

    // ---- gauges ------------------------------------------------------------
    auto ga = gaugeArea.reduced (6);
    auto meters = ga.removeFromRight (62);
    inputStrip.setBounds  (meters.removeFromLeft (30).reduced (2));
    outputStrip.setBounds (meters.reduced (2));

    const int gh = ga.getHeight() / 3;
    gainReductionGauge.setBounds (ga.removeFromTop (gh).reduced (3));
    deEssGauge.setBounds         (ga.removeFromTop (gh).reduced (3));
    pitchGauge.setBounds         (ga.reduced (3));

    // ---- racks -------------------------------------------------------------
    auto layoutRack = [] (juce::OwnedArray<ModuleStrip>& rack, juce::Rectangle<int> area,
                          std::vector<TrimKnob*> trims)
    {
        auto strips = area.removeFromTop (area.getHeight() - 66);
        strips.removeFromTop (26);                    // header plate
        const int n = juce::jmax (1, rack.size());
        const int h = juce::jmin (24, strips.getHeight() / n);

        for (auto* s : rack)
            s->setBounds (strips.removeFromTop (h).reduced (3, 1));

        // trim knob row underneath
        if (! trims.empty())
        {
            const int w = area.getWidth() / (int) trims.size();
            for (auto* t : trims)
            {
                auto cell = area.removeFromLeft (w);
                t->label.setBounds (cell.removeFromBottom (12));
                t->slider.setBounds (cell.reduced (3));
            }
        }
    };

    layoutRack (cleanupModules, cleanupRackArea,
                { &eqTrim, &compTrim, &deEssTrim, &cleanTrim });
    layoutRack (effectsModules, effectsRackArea,
                { &tuneTrim, &doublerTrim, &driveTrim, &charTrim,
                  &widthTrim, &delayTrim, &reverbTrim, &duckTrim });

    outputTrim.slider.setBounds (logoArea.removeFromRight (58).reduced (6, 4));
    outputTrim.label.setBounds (outputTrim.slider.getBounds()
                                    .withY (outputTrim.slider.getBottom() - 2)
                                    .withHeight (11));
}

//==============================================================================
void ListenatorEditor::paint (juce::Graphics& g)
{
    auto full = getLocalBounds().toFloat();

    // ---- backdrop: purple lab wall ----------------------------------------
    g.setGradientFill (juce::ColourGradient (purpleDark.brighter (0.16f), 0.0f, 0.0f,
                                             purpleDark.darker (0.45f), 0.0f, full.getBottom(),
                                             false));
    g.fillAll();

    // faint blueprint grid
    g.setColour (purpleLight.withAlpha (0.05f));
    for (int x = 0; x < getWidth(); x += 24) g.drawVerticalLine (x, 0.0f, full.getBottom());
    for (int y = 0; y < getHeight(); y += 24) g.drawHorizontalLine (y, 0.0f, full.getRight());

    // ---- title plate -------------------------------------------------------
    auto logo = logoArea.toFloat();
    drawHazardStripes (g, logo.removeFromTop (7.0f));
    drawMetalPanel (g, logo.reduced (3.0f), 5.0f);

    auto inner = logo.reduced (12.0f);
    doofart::drawDeiLogo (g, inner.removeFromLeft (60.0f).reduced (0.0f, 2.0f));
    inner.removeFromLeft (14.0f);

    auto titleArea = inner.removeFromLeft (400.0f);
    auto nameRow = titleArea.removeFromTop (titleArea.getHeight() * 0.66f);

    // engraved lettering: dark offset under a bright face
    g.setFont (DoofLookAndFeel::machineFont (38.0f, true));
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.drawText ("LISTENATOR", nameRow.translated (1.5f, 1.5f),
                juce::Justification::centredLeft, false);
    g.setColour (mintBright);
    g.drawText ("LISTENATOR", nameRow, juce::Justification::centredLeft, false);

    g.setColour (purpleLight.withAlpha (0.9f));
    g.setFont (DoofLookAndFeel::machineFont (10.5f, false));
    g.drawText ("SPRIZZLE   //   DOOFENSHMIRTZ EVIL INCORPORATED   //   MODEL 1.0",
                titleArea, juce::Justification::centredLeft, false);

    // status LED cluster
    {
        auto leds = inner.removeFromLeft (150.0f).withSizeKeepingCentre (150.0f, 26.0f);
        struct L { const char* name; bool on; juce::Colour c; };
        const L lamps[] = {
            { "PWR",   true,                                       mintGreen },
            { "SIG",   proc.getInputLevelDb() > -60.0f,            warningYellow },
            { "ANLZ",  proc.hasAnalysis(),                         crtGreen },
            { "EVIL",  true,                                       dangerRed }
        };
        for (int i = 0; i < 4; ++i)
        {
            auto cell = leds.removeFromLeft (37.0f);
            auto dot = cell.removeFromTop (11.0f).withSizeKeepingCentre (8.0f, 8.0f);
            const auto c = lamps[i].on ? lamps[i].c : metalMid;
            if (lamps[i].on)
            { g.setColour (c.withAlpha (0.35f)); g.fillEllipse (dot.expanded (3.0f)); }
            g.setColour (c);
            g.fillEllipse (dot);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawEllipse (dot, 0.8f);
            g.setColour (purpleLight.withAlpha (0.8f));
            g.setFont (DoofLookAndFeel::machineFont (8.0f, true));
            g.drawText (lamps[i].name, cell, juce::Justification::centredTop, false);
        }
    }

    // ---- the doctor --------------------------------------------------------
    auto doc = doctorArea.toFloat().reduced (6.0f);
    drawMetalPanel (g, doc, 6.0f, true);

    auto bubble = doc.removeFromTop (74.0f).reduced (5.0f);
    g.setColour (juce::Colour (0xfff2eee6));
    g.fillRoundedRectangle (bubble, 7.0f);
    g.setColour (metalDark);
    g.drawRoundedRectangle (bubble, 7.0f, 1.4f);

    juce::Path tailPath;
    tailPath.addTriangle (bubble.getCentreX() - 8.0f, bubble.getBottom() - 1.0f,
                          bubble.getCentreX() + 8.0f, bubble.getBottom() - 1.0f,
                          bubble.getCentreX() - 2.0f, bubble.getBottom() + 11.0f);
    g.setColour (juce::Colour (0xfff2eee6));
    g.fillPath (tailPath);

    g.setColour (juce::Colour (0xff2a2630));
    g.setFont (DoofLookAndFeel::machineFont (11.0f, false));
    g.drawFittedText (currentQuote, bubble.reduced (6.0f).toNearestInt(),
                      juce::Justification::centred, 4);

    // leave a strip at the foot of the panel for the tubes
    {
        auto figure = doc.reduced (2.0f).withTrimmedTop (4.0f).withTrimmedBottom (44.0f);
        // a soft pool of light behind him lifts the figure off the panel
        g.setGradientFill (juce::ColourGradient (
            purpleLight.withAlpha (0.14f), figure.getCentreX(), figure.getCentreY(),
            juce::Colours::transparentBlack, figure.getX() - 10.0f, figure.getY(), true));
        g.fillEllipse (figure.expanded (8.0f, 0.0f));

        doofart::drawDoctor (g, figure, mouthOpen);

        // floor shadow
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillEllipse (figure.getCentreX() - figure.getWidth() * 0.26f,
                       figure.getBottom() - 6.0f,
                       figure.getWidth() * 0.52f, 8.0f);
    }

    // ---- decorative machinery around the CRT -------------------------------
    auto crtFrame = crtArea.toFloat();
    drawMetalPanel (g, crtFrame, 7.0f);

    // vacuum tubes in the doctor's panel, glowing with output level
    // (they can't go on the CRT frame -- the CrtDisplay child paints over it)
    {
        const float tubeW = 14.0f, tubeH = 32.0f;
        const float ty = (float) doctorArea.getBottom() - tubeH - 10.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float x = (float) doctorArea.getX() + 20.0f + (float) i * 32.0f;
            drawVacuumTube (g, { x, ty, tubeW, tubeH },
                            tubeGlow * (0.55f + 0.45f * std::sin ((float) i * 1.7f
                                        + (float) juce::Time::getMillisecondCounter() * 0.004f)));
        }
    }

    // exposed cabling from the machine down to the racks
    drawCable (g, { (float) crtArea.getX() + 20.0f, (float) crtArea.getBottom() },
                  { (float) cleanupRackArea.getCentreX(), (float) cleanupRackArea.getY() },
                  dangerRed.withAlpha (0.8f), 3.0f, 26.0f);
    drawCable (g, { (float) crtArea.getRight() - 20.0f, (float) crtArea.getBottom() },
                  { (float) effectsRackArea.getCentreX(), (float) effectsRackArea.getY() },
                  mintGreen.withAlpha (0.8f), 3.0f, 26.0f);

    // ---- listen button surround -------------------------------------------
    auto la = listenArea.toFloat().reduced (2.0f);
    drawMetalPanel (g, la, 5.0f);
    drawHazardStripes (g, la.removeFromLeft (140.0f).reduced (3.0f)
                             .withHeight (5.0f).translated (0.0f, 1.0f));

    // ---- rack panels -------------------------------------------------------
    auto drawRack = [&] (juce::Rectangle<int> area, const juce::String& title,
                         juce::Colour accent, bool bypassed)
    {
        auto ar = area.toFloat();
        drawMetalPanel (g, ar, 6.0f);

        auto header = ar.reduced (6.0f).removeFromTop (20.0f);
        g.setColour (accent.withAlpha (bypassed ? 0.20f : 0.34f));
        g.fillRoundedRectangle (header, 3.0f);

        g.setColour (bypassed ? metalLight : mintBright);
        g.setFont (DoofLookAndFeel::machineFont (13.0f, true));
        g.drawText (title + (bypassed ? "   [ OFFLINE ]" : ""), header,
                    juce::Justification::centred, false);
    };

    drawRack (cleanupRackArea, "CLEAN-INATOR", mintGreen,
              cleanupBypass.getToggleState());
    drawRack (effectsRackArea, "FX-INATOR", purpleLight,
              effectsBypass.getToggleState());

    // ---- Danville skyline at the foot --------------------------------------
    auto sky = skylineArea.toFloat();
    g.setColour (purpleDark.darker (0.6f));
    g.fillRect (sky);
    doofart::drawSkyline (g, sky, purpleDark.darker (0.85f));
    // Perry, peering over the parapet. His fedora sits above the band, so the
    // draw rect has to extend upward past it or the hat gets clipped.
    {
        auto perry = sky.removeFromRight (74.0f).translated (-8.0f, 0.0f);
        doofart::drawPlatypus (g, juce::Rectangle<float> (
            perry.getX(), perry.getY() - 6.0f, perry.getWidth(), perry.getHeight() + 10.0f));
    }

    g.setColour (purpleLight.withAlpha (0.45f));
    g.setFont (DoofLookAndFeel::machineFont (9.0f, false));
    g.drawText (proc.hasAnalysis() ? "SPECIMEN ANALYSED"
                                   : "NO SPECIMEN - CHAIN INACTIVE",
                sky.reduced (8.0f), juce::Justification::centredLeft, false);
}

} // namespace listenator
