#include "DoofLookAndFeel.h"

namespace listenator
{

namespace doof
{

void drawRivet (juce::Graphics& g, juce::Point<float> c, float r)
{
    juce::ColourGradient grad (metalLight, c.x - r * 0.4f, c.y - r * 0.4f,
                               metalDark,  c.x + r * 0.5f, c.y + r * 0.5f, true);
    g.setGradientFill (grad);
    g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.drawEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f, 1.0f);

    g.setColour (juce::Colours::white.withAlpha (0.30f));
    g.fillEllipse (c.x - r * 0.45f, c.y - r * 0.55f, r * 0.5f, r * 0.4f);
}

void drawMetalPanel (juce::Graphics& g, juce::Rectangle<float> r,
                     float cornerSize, bool recessed)
{
    const auto top = recessed ? metalDark : metalMid;
    const auto bot = recessed ? metalMid.darker (0.3f) : metalDark;

    g.setGradientFill (juce::ColourGradient (top, r.getX(), r.getY(),
                                             bot, r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, cornerSize);

    // brushed texture: faint horizontal striations
    g.saveState();
    g.reduceClipRegion (r.toNearestInt());
    g.setColour (juce::Colours::white.withAlpha (0.022f));
    for (float y = r.getY(); y < r.getBottom(); y += 3.0f)
        g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
    g.restoreState();

    // bevel
    g.setColour ((recessed ? juce::Colours::black : juce::Colours::white).withAlpha (0.28f));
    g.drawRoundedRectangle (r.reduced (0.5f), cornerSize, 1.4f);
    g.setColour ((recessed ? juce::Colours::white : juce::Colours::black).withAlpha (0.35f));
    g.drawRoundedRectangle (r.reduced (1.8f), cornerSize, 1.0f);

    // corner bolts
    if (r.getWidth() > 44.0f && r.getHeight() > 34.0f)
    {
        const float inset = 9.0f, rad = 3.2f;
        drawRivet (g, { r.getX() + inset,     r.getY() + inset },        rad);
        drawRivet (g, { r.getRight() - inset, r.getY() + inset },        rad);
        drawRivet (g, { r.getX() + inset,     r.getBottom() - inset },   rad);
        drawRivet (g, { r.getRight() - inset, r.getBottom() - inset },   rad);
    }
}

void drawHazardStripes (juce::Graphics& g, juce::Rectangle<float> r,
                        juce::Colour a, juce::Colour b)
{
    g.saveState();
    g.reduceClipRegion (r.toNearestInt());
    g.fillAll (b);
    g.setColour (a);

    const float w = 14.0f;
    for (float x = r.getX() - r.getHeight(); x < r.getRight() + r.getHeight(); x += w * 2.0f)
    {
        juce::Path p;
        p.startNewSubPath (x, r.getBottom());
        p.lineTo (x + r.getHeight(), r.getY());
        p.lineTo (x + r.getHeight() + w, r.getY());
        p.lineTo (x + w, r.getBottom());
        p.closeSubPath();
        g.fillPath (p);
    }
    g.restoreState();
}

void drawCrtScreen (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (crtDim);
    g.fillRoundedRectangle (r, 4.0f);

    // phosphor glow toward the centre
    juce::ColourGradient glow (crtGreen.withAlpha (0.10f), r.getCentreX(), r.getCentreY(),
                               juce::Colours::transparentBlack, r.getX(), r.getY(), true);
    g.setGradientFill (glow);
    g.fillRoundedRectangle (r, 4.0f);

    // scanlines
    g.setColour (juce::Colours::black.withAlpha (0.22f));
    for (float y = r.getY(); y < r.getBottom(); y += 3.0f)
        g.drawHorizontalLine ((int) y, r.getX(), r.getRight());

    // bezel
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawRoundedRectangle (r, 4.0f, 2.0f);
    g.setColour (mintGreen.withAlpha (0.25f));
    g.drawRoundedRectangle (r.reduced (2.0f), 3.0f, 1.0f);
}

void drawCable (juce::Graphics& g, juce::Point<float> from, juce::Point<float> to,
                juce::Colour c, float thickness, float sag)
{
    juce::Path p;
    p.startNewSubPath (from);
    const auto mid = (from + to) * 0.5f;
    p.quadraticTo (mid.x, mid.y + sag, to.x, to.y);

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.strokePath (p, juce::PathStrokeType (thickness + 1.6f,
                                           juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (thickness,
                                           juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));
}

void drawVacuumTube (juce::Graphics& g, juce::Rectangle<float> r, float glow)
{
    glow = juce::jlimit (0.0f, 1.0f, glow);

    // glass envelope
    g.setColour (juce::Colour (0xff1c2430).withAlpha (0.85f));
    g.fillRoundedRectangle (r, r.getWidth() * 0.45f);

    // filament glow
    auto inner = r.reduced (r.getWidth() * 0.28f);
    juce::ColourGradient gr (warningYellow.withAlpha (0.15f + 0.75f * glow),
                             inner.getCentreX(), inner.getCentreY(),
                             juce::Colours::transparentBlack,
                             inner.getX(), inner.getY(), true);
    g.setGradientFill (gr);
    g.fillEllipse (inner.expanded (r.getWidth() * 0.35f));

    g.setColour (dangerRed.withAlpha (0.35f + 0.6f * glow));
    g.fillRoundedRectangle (inner.reduced (1.0f), inner.getWidth() * 0.4f);

    // glass highlight + base
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.fillRoundedRectangle (r.getX() + r.getWidth() * 0.18f, r.getY() + r.getHeight() * 0.12f,
                            r.getWidth() * 0.16f, r.getHeight() * 0.5f, 2.0f);

    g.setColour (metalDark);
    g.fillRoundedRectangle (r.getX(), r.getBottom() - r.getHeight() * 0.18f,
                            r.getWidth(), r.getHeight() * 0.18f, 2.0f);
}

} // namespace doof

//==============================================================================
juce::Font DoofLookAndFeel::machineFont (float height, bool bold)
{
    juce::Font f (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(),
                                     height,
                                     bold ? juce::Font::bold : juce::Font::plain));
    f.setHorizontalScale (0.88f);   // condensed, stencil-ish
    return f;
}

DoofLookAndFeel::DoofLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, doof::purpleDark);
    setColour (juce::Label::textColourId,                 doof::mintBright);
    setColour (juce::Slider::textBoxTextColourId,         doof::crtGreen);
    setColour (juce::Slider::textBoxBackgroundColourId,   doof::crtDim);
    setColour (juce::Slider::textBoxOutlineColourId,      doof::metalDark);
    setColour (juce::ComboBox::backgroundColourId,        doof::crtDim);
    setColour (juce::ComboBox::textColourId,              doof::crtGreen);
    setColour (juce::ComboBox::outlineColourId,           doof::metalLight);
    setColour (juce::ComboBox::arrowColourId,             doof::mintGreen);
    setColour (juce::PopupMenu::backgroundColourId,       doof::purpleDark);
    setColour (juce::PopupMenu::textColourId,             doof::mintBright);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, doof::purpleMid);
    setColour (juce::TooltipWindow::backgroundColourId,   doof::purpleDark);
    setColour (juce::TooltipWindow::textColourId,         doof::mintBright);
}

juce::Font DoofLookAndFeel::getLabelFont (juce::Label& l)
{
    return machineFont (l.getFont().getHeight(), false);
}

juce::Font DoofLookAndFeel::getComboBoxFont (juce::ComboBox& b)
{
    return machineFont (juce::jmin (15.0f, (float) b.getHeight() * 0.6f), true);
}

void DoofLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width,
                                        int height, float sliderPos,
                                        float rotaryStartAngle, float rotaryEndAngle,
                                        juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle  = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // recessed well
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

    // ticks around the dial
    g.setColour (doof::metalLight.withAlpha (0.5f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = rotaryStartAngle + (float) i / 10.0f * (rotaryEndAngle - rotaryStartAngle);
        const auto p1 = centre.getPointOnCircumference (radius * 0.98f, a);
        const auto p2 = centre.getPointOnCircumference (radius * 0.86f, a);
        g.drawLine ({ p1, p2 }, i % 5 == 0 ? 1.8f : 0.9f);
    }

    // knob body
    const float knobR = radius * 0.76f;
    juce::ColourGradient body (doof::metalLight, centre.x - knobR * 0.5f, centre.y - knobR * 0.7f,
                               doof::metalDark,  centre.x + knobR * 0.5f, centre.y + knobR * 0.8f, false);
    g.setGradientFill (body);
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.5f);

    // value arc in phosphor green
    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius * 0.92f, radius * 0.92f, 0.0f,
                       rotaryStartAngle, angle, true);
    g.setColour (slider.isEnabled() ? doof::crtGreen : doof::metalMid);
    g.strokePath (arc, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    // pointer
    const auto tip  = centre.getPointOnCircumference (knobR * 0.82f, angle);
    const auto base = centre.getPointOnCircumference (knobR * 0.22f, angle);
    g.setColour (juce::Colours::black.withAlpha (0.75f));
    g.drawLine ({ base, tip }, 4.2f);
    g.setColour (doof::warningYellow);
    g.drawLine ({ base, tip }, 2.4f);

    // centre cap
    g.setColour (doof::metalDark);
    g.fillEllipse (centre.x - knobR * 0.2f, centre.y - knobR * 0.2f,
                   knobR * 0.4f, knobR * 0.4f);
}

void DoofLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                                        bool highlighted, bool /*down*/)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();

    // a chunky industrial rocker switch
    const auto body = r.withWidth (juce::jmin (r.getWidth(), 34.0f));
    doof::drawMetalPanel (g, body, 3.0f, true);

    auto lamp = body.reduced (5.0f).withHeight (body.getHeight() * 0.42f);
    if (on) lamp.translate (0.0f, body.getHeight() * 0.46f);

    g.setColour (on ? doof::dangerRed : doof::metalMid);
    g.fillRoundedRectangle (lamp, 2.0f);
    if (on)
    {
        g.setColour (doof::dangerRed.withAlpha (0.45f));
        g.fillRoundedRectangle (lamp.expanded (2.5f), 3.0f);
    }

    g.setColour (highlighted ? doof::mintBright : doof::purpleLight);
    g.setFont (machineFont (12.0f, true));
    g.drawText (b.getButtonText(),
                r.withTrimmedLeft (body.getWidth() + 6.0f),
                juce::Justification::centredLeft, true);
}

void DoofLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                            const juce::Colour& bg,
                                            bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (r.translated (0.0f, down ? 0.0f : 2.5f), 5.0f);

    auto face = down ? r.translated (0.0f, 1.5f) : r;
    auto c = bg;
    if (highlighted) c = c.brighter (0.15f);

    g.setGradientFill (juce::ColourGradient (c.brighter (0.25f), face.getX(), face.getY(),
                                             c.darker (0.35f),  face.getX(), face.getBottom(),
                                             false));
    g.fillRoundedRectangle (face, 5.0f);

    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.drawRoundedRectangle (face, 5.0f, 1.4f);
    g.setColour (juce::Colours::white.withAlpha (0.20f));
    g.drawRoundedRectangle (face.reduced (1.6f), 4.0f, 1.0f);
}

void DoofLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
                                    bool /*isButtonDown*/, int, int, int, int,
                                    juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    doof::drawCrtScreen (g, r.reduced (1.0f));

    g.setColour (box.findColour (juce::ComboBox::outlineColourId).withAlpha (0.6f));
    g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 1.2f);

    juce::Path arrow;
    const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
    arrow.addTriangle (cx - 5.0f, cy - 2.5f, cx + 5.0f, cy - 2.5f, cx, cy + 3.5f);
    g.setColour (doof::mintGreen);
    g.fillPath (arrow);
}

} // namespace listenator
