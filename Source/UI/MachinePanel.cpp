#include "MachinePanel.h"
#include "DoofLookAndFeel.h"
#include "PluginProcessor.h"

namespace listenator
{

using namespace doof;

//==============================================================================
CrtDisplay::CrtDisplay (ListenatorProcessor& p) : proc (p)
{
    startTimerHz (30);
}

void CrtDisplay::timerCallback()
{
    sweep += 0.012f;
    if (sweep > 1.0f) sweep -= 1.0f;
    repaint();
}

void CrtDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawCrtScreen (g, r);

    auto plot = r.reduced (10.0f);

    // ---- graticule --------------------------------------------------------
    g.setColour (crtGreen.withAlpha (0.13f));
    for (int i = 1; i < 8; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / 8.0f;
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
    }
    for (int i = 1; i < 5; ++i)
    {
        const float y = plot.getY() + plot.getHeight() * (float) i / 5.0f;
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
    }

    const auto& a = proc.getAnalysisResult();

    if (! proc.hasAnalysis())
    {
        // idle: a slow sweeping trace so the tube looks alive
        juce::Path idle;
        for (int i = 0; i <= 120; ++i)
        {
            const float t = (float) i / 120.0f;
            const float x = plot.getX() + t * plot.getWidth();
            const float env = std::exp (-30.0f * (t - sweep) * (t - sweep));
            const float y = plot.getCentreY()
                          - env * plot.getHeight() * 0.30f
                            * std::sin (t * 42.0f + sweep * 22.0f);
            if (i == 0) idle.startNewSubPath (x, y); else idle.lineTo (x, y);
        }
        g.setColour (crtGreen.withAlpha (0.75f));
        g.strokePath (idle, juce::PathStrokeType (1.6f));

        g.setColour (crtGreen.withAlpha (0.55f));
        g.setFont (DoofLookAndFeel::machineFont (13.0f, true));

        const juce::String msg = proc.getAnalyzer().isListening()
            ? "LISTENING... " + juce::String ((int) (proc.getListenProgress() * 100.0f)) + "%"
            : "AWAITING SPECIMEN - PRESS LISTEN";

        g.drawText (msg, plot, juce::Justification::centred, false);
        return;
    }

    // ---- measured LTAS vs the universal target ----------------------------
    auto bandX = [&] (int b)
    {
        const float f = toneBandHz[(size_t) b];
        const float t = std::log10 (f / 20.0f) / std::log10 (20000.0f / 20.0f);
        return plot.getX() + t * plot.getWidth();
    };

    // normalise the measured curve for display
    float maxDb = -200.0f;
    for (int b = 0; b < numToneBands; ++b)
        maxDb = std::max (maxDb, a.measuredLtasDb[(size_t) b]);

    juce::Path meas;
    for (int b = 0; b < numToneBands; ++b)
    {
        const float rel = juce::jlimit (-60.0f, 0.0f, a.measuredLtasDb[(size_t) b] - maxDb);
        const float y = plot.getBottom() + rel / 60.0f * plot.getHeight();
        if (b == 0) meas.startNewSubPath (bandX (b), y); else meas.lineTo (bandX (b), y);
    }

    // filled spectrum
    auto filled = meas;
    filled.lineTo (plot.getRight(), plot.getBottom());
    filled.lineTo (plot.getX(), plot.getBottom());
    filled.closeSubPath();
    g.setColour (crtGreen.withAlpha (0.16f));
    g.fillPath (filled);

    g.setColour (crtGreen);
    g.strokePath (meas, juce::PathStrokeType (1.9f));

    // the correction curve the plugin decided on, drawn around the mid line
    juce::Path corr;
    for (int b = 0; b < numToneBands; ++b)
    {
        const float y = plot.getCentreY()
                      - a.toneMatchDb[(size_t) b] / 12.0f * plot.getHeight() * 0.5f;
        if (b == 0) corr.startNewSubPath (bandX (b), y); else corr.lineTo (bandX (b), y);
    }
    g.setColour (warningYellow.withAlpha (0.85f));
    g.strokePath (corr, juce::PathStrokeType (1.5f));

    // resonance notches as downward ticks
    g.setColour (dangerRed.withAlpha (0.8f));
    for (const auto& res : a.resonances)
    {
        const float t = std::log10 (juce::jmax (res.frequencyHz, 20.0f) / 20.0f)
                      / std::log10 (1000.0f);
        const float x = plot.getX() + t * plot.getWidth();
        g.drawLine (x, plot.getY(), x, plot.getY() + plot.getHeight() * 0.14f, 1.6f);
    }

    // de-ess band marker
    {
        const float t = std::log10 (juce::jmax (a.deEssCentreHz, 20.0f) / 20.0f)
                      / std::log10 (1000.0f);
        const float x = plot.getX() + t * plot.getWidth();
        g.setColour (purpleLight.withAlpha (0.7f));
        g.drawLine (x, plot.getY(), x, plot.getBottom(), 1.2f);
    }

    // ---- noise floor, so you can see what the gate and de-noise face -------
    {
        juce::Path nf;
        bool started = false;
        for (int b = 0; b < numToneBands; ++b)
        {
            const float v = a.noiseLtasDb[(size_t) b];
            if (v <= -119.0f) continue;
            const float rel = juce::jlimit (-60.0f, 0.0f, v - maxDb);
            const float y = plot.getBottom() + rel / 60.0f * plot.getHeight();
            if (! started) { nf.startNewSubPath (bandX (b), y); started = true; }
            else nf.lineTo (bandX (b), y);
        }
        g.setColour (purpleLight.withAlpha (0.55f));
        g.strokePath (nf, juce::PathStrokeType (1.0f));
    }

    // ---- frequency ruler ---------------------------------------------------
    g.setFont (DoofLookAndFeel::machineFont (8.0f, false));
    g.setColour (crtGreen.withAlpha (0.4f));
    for (float f : { 100.0f, 1000.0f, 10000.0f })
    {
        const float t = std::log10 (f / 20.0f) / std::log10 (1000.0f);
        const float x = plot.getX() + t * plot.getWidth();
        g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k"
                                 : juce::String ((int) f),
                    juce::Rectangle<float> (x - 14.0f, plot.getBottom() - 11.0f, 28.0f, 10.0f),
                    juce::Justification::centred, false);
    }

    // ---- legend ------------------------------------------------------------
    {
        auto key = plot.removeFromTop (13.0f).removeFromRight (250.0f);
        struct E { juce::Colour c; const char* label; };
        const E entries[] = { { crtGreen, "SPECTRUM" }, { warningYellow, "CORRECTION" },
                              { purpleLight, "NOISE" }, { dangerRed, "RESONANCE" } };
        g.setFont (DoofLookAndFeel::machineFont (8.0f, true));
        for (const auto& e : entries)
        {
            auto cell = key.removeFromLeft (62.0f);
            g.setColour (e.c);
            g.fillRect (cell.removeFromLeft (10.0f).withSizeKeepingCentre (9.0f, 2.0f));
            g.drawText (e.label, cell, juce::Justification::centredLeft, false);
        }
    }

    // ---- readout corner ---------------------------------------------------
    g.setColour (crtGreen.withAlpha (0.85f));
    g.setFont (DoofLookAndFeel::machineFont (11.0f, false));

    juce::StringArray lines;
    lines.add ("F0 "     + juce::String (a.medianF0Hz, 1) + " Hz");
    lines.add ("LUFS "   + juce::String (a.integratedLufs, 1));
    lines.add ("CREST "  + juce::String (a.crestFactorDb, 1) + " dB");
    lines.add ("NOISE "  + juce::String (a.noiseFloorDb, 1) + " dB");
    lines.add ("RT60 "   + juce::String (a.rt60Seconds, 2) + " s");
    lines.add ("SIB "    + juce::String ((int) a.deEssCentreHz) + " Hz");
    lines.add ("HPF "    + juce::String ((int) a.highPassHz) + " Hz");
    lines.add ("GATE "   + juce::String ((int) a.gateThresholdDb) + " dB");
    lines.add ("TILT "   + juce::String (a.spectralTiltDbPerOct, 1) + " dB/oct");

    auto textArea = plot.removeFromLeft (108.0f).reduced (2.0f)
                        .withHeight (13.0f * (float) lines.size() + 8.0f);

    // opaque backing so the trace doesn't run through the numbers
    g.setColour (crtDim.withAlpha (0.92f));
    g.fillRoundedRectangle (textArea, 3.0f);
    g.setColour (crtGreen.withAlpha (0.30f));
    g.drawRoundedRectangle (textArea, 3.0f, 1.0f);

    g.setColour (crtGreen.withAlpha (0.9f));
    auto textInner = textArea.reduced (5.0f, 4.0f);
    for (int i = 0; i < lines.size(); ++i)
        g.drawText (lines[i],
                    textInner.withHeight (13.0f).translated (0.0f, (float) i * 13.0f),
                    juce::Justification::centredLeft, false);
}

//==============================================================================
Gauge::Gauge (juce::String label, juce::String units)
    : name (std::move (label)), unit (std::move (units)) {}

void Gauge::setValue (float normalised, juce::String readout)
{
    value = juce::jlimit (0.0f, 1.0f, normalised);
    text = std::move (readout);
    smoothed = smoothed * 0.7f + value * 0.3f;
    repaint();
}

void Gauge::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawMetalPanel (g, r, 5.0f, true);

    auto face = r.reduced (7.0f);
    face.removeFromBottom (13.0f);

    // cream dial face
    g.setColour (juce::Colour (0xffe9e2cd));
    g.fillEllipse (face);
    g.setColour (metalDark);
    g.drawEllipse (face, 1.5f);

    const auto centre = face.getCentre().withY (face.getBottom() - face.getHeight() * 0.18f);
    const float radius = face.getWidth() * 0.42f;
    const float a0 = juce::MathConstants<float>::pi * 1.20f;
    const float a1 = juce::MathConstants<float>::pi * 1.80f;

    // scale ticks, red at the top end
    for (int i = 0; i <= 10; ++i)
    {
        const float t = (float) i / 10.0f;
        const float ang = a0 + t * (a1 - a0);
        g.setColour (t > 0.8f ? dangerRed : juce::Colour (0xff33312c));
        const auto p1 = centre.getPointOnCircumference (radius, ang);
        const auto p2 = centre.getPointOnCircumference (radius * (i % 5 == 0 ? 0.80f : 0.88f), ang);
        g.drawLine ({ p1, p2 }, i % 5 == 0 ? 1.8f : 0.9f);
    }

    // needle
    const float ang = a0 + smoothed * (a1 - a0);
    const auto tip = centre.getPointOnCircumference (radius * 0.86f, ang);
    g.setColour (dangerRed);
    g.drawLine ({ centre, tip }, 2.0f);
    g.setColour (metalDark);
    g.fillEllipse (centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f);

    // labels
    g.setColour (juce::Colour (0xff33312c));
    g.setFont (DoofLookAndFeel::machineFont (9.0f, true));
    g.drawText (name, face.removeFromTop (12.0f), juce::Justification::centred, false);

    g.setColour (mintBright);
    g.setFont (DoofLookAndFeel::machineFont (11.0f, true));
    g.drawText (text.isEmpty() ? unit : text,
                r.removeFromBottom (14.0f), juce::Justification::centred, false);
}

//==============================================================================
LevelStrip::LevelStrip (juce::String label) : name (std::move (label)) {}

void LevelStrip::setLevelDb (float db)
{
    levelDb = db;
    if (db > peakDb) { peakDb = db; peakHoldCount = 45; }
    else if (--peakHoldCount <= 0) peakDb = juce::jmax (-100.0f, peakDb - 0.7f);
    repaint();
}

void LevelStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    g.setColour (mintBright);
    g.setFont (DoofLookAndFeel::machineFont (9.0f, true));
    g.drawText (name, r.removeFromBottom (12.0f), juce::Justification::centred, false);

    drawMetalPanel (g, r, 3.0f, true);
    auto well = r.reduced (4.0f);

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (well, 2.0f);

    auto norm = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };

    // segmented ladder, green -> yellow -> red
    const int segments = 24;
    const float segH = well.getHeight() / (float) segments;
    const float lit = norm (levelDb) * (float) segments;

    for (int i = 0; i < segments; ++i)
    {
        const float t = (float) i / (float) (segments - 1);
        auto c = t > 0.90f ? dangerRed : (t > 0.72f ? warningYellow : mintGreen);
        const bool on = (float) i < lit;

        g.setColour (on ? c : c.withAlpha (0.10f));
        g.fillRect (well.getX() + 1.0f,
                    well.getBottom() - (float) (i + 1) * segH + 1.0f,
                    well.getWidth() - 2.0f, segH - 1.5f);
    }

    // peak hold tick
    const float py = well.getBottom() - norm (peakDb) * well.getHeight();
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.drawHorizontalLine ((int) py, well.getX(), well.getRight());
}

//==============================================================================
ModuleStrip::ModuleStrip (juce::String name, juce::Colour accent)
    : moduleName (std::move (name)), accentColour (accent)
{
    bypassButton.setButtonText ("");
    addAndMakeVisible (bypassButton);
}

void ModuleStrip::resized()
{
    bypassButton.setBounds (getLocalBounds().removeFromRight (30).reduced (2));
}

void ModuleStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawMetalPanel (g, r, 3.0f, false);

    const bool off = bypassButton.getToggleState();

    // activity lamp
    auto lamp = r.removeFromLeft (20.0f).withSizeKeepingCentre (9.0f, 9.0f);
    const auto c = off ? metalMid : accentColour;
    g.setColour (c.withAlpha (off ? 0.35f : 0.45f + 0.55f * activity));
    g.fillEllipse (lamp.expanded (2.5f));
    g.setColour (c.withAlpha (off ? 0.5f : 1.0f));
    g.fillEllipse (lamp);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawEllipse (lamp, 0.8f);

    // stencilled name
    g.setColour (off ? metalLight.withAlpha (0.45f) : mintBright);
    g.setFont (DoofLookAndFeel::machineFont (11.0f, true));
    g.drawText (moduleName, r.withTrimmedRight (32.0f).withTrimmedLeft (2.0f),
                juce::Justification::centredLeft, true);
}

} // namespace listenator
