#include "DoofArt.h"
#include "DoofLookAndFeel.h"

namespace listenator::doofart
{

using namespace listenator::doof;

namespace
{
    const juce::Colour coatWhite  { 0xffe8e6ea };
    const juce::Colour coatShade  { 0xffc2bfc8 };
    const juce::Colour skinTone   { 0xffe0b48c };
    const juce::Colour skinShade  { 0xffc59a72 };
    const juce::Colour hairBrown  { 0xff6b4a33 };
    const juce::Colour trouserTan { 0xff8a7f6b };
    const juce::Colour teal       { 0xff3aa88f };
}

//==============================================================================
void drawDoctor (juce::Graphics& g, juce::Rectangle<float> r, float mouthOpen01)
{
    const float w = r.getWidth(), h = r.getHeight();
    const float cx = r.getCentreX();

    auto px = [&] (float fx) { return r.getX() + fx * w; };
    auto py = [&] (float fy) { return r.getY() + fy * h; };

    // ---- legs / trousers ------------------------------------------------
    g.setColour (trouserTan);
    g.fillRoundedRectangle (px (0.36f), py (0.72f), w * 0.10f, h * 0.26f, w * 0.03f);
    g.fillRoundedRectangle (px (0.54f), py (0.72f), w * 0.10f, h * 0.26f, w * 0.03f);

    g.setColour (juce::Colour (0xff2e2a26));
    g.fillRoundedRectangle (px (0.33f), py (0.955f), w * 0.16f, h * 0.045f, w * 0.02f);
    g.fillRoundedRectangle (px (0.51f), py (0.955f), w * 0.16f, h * 0.045f, w * 0.02f);

    // ---- lab coat: narrow shoulders, flared hem -------------------------
    juce::Path coat;
    coat.startNewSubPath (px (0.42f), py (0.30f));
    coat.lineTo (px (0.26f), py (0.42f));
    coat.lineTo (px (0.21f), py (0.78f));
    coat.lineTo (px (0.34f), py (0.80f));
    coat.lineTo (px (0.36f), py (0.55f));
    coat.lineTo (px (0.64f), py (0.55f));
    coat.lineTo (px (0.66f), py (0.80f));
    coat.lineTo (px (0.79f), py (0.78f));
    coat.lineTo (px (0.74f), py (0.42f));
    coat.lineTo (px (0.58f), py (0.30f));
    coat.closeSubPath();

    g.setColour (coatWhite);
    g.fillPath (coat);
    g.setColour (coatShade);
    g.strokePath (coat, juce::PathStrokeType (1.4f));

    // coat lapels
    juce::Path lapel;
    lapel.startNewSubPath (px (0.42f), py (0.30f));
    lapel.lineTo (px (0.50f), py (0.52f));
    lapel.lineTo (px (0.58f), py (0.30f));
    g.setColour (coatShade);
    g.strokePath (lapel, juce::PathStrokeType (1.8f));

    // teal turtleneck under the coat
    g.setColour (teal);
    g.fillRoundedRectangle (px (0.44f), py (0.255f), w * 0.12f, h * 0.07f, w * 0.02f);

    // ---- arms -----------------------------------------------------------
    g.setColour (coatWhite);
    g.fillRoundedRectangle (px (0.17f), py (0.40f), w * 0.10f, h * 0.30f, w * 0.04f);
    g.fillRoundedRectangle (px (0.73f), py (0.40f), w * 0.10f, h * 0.30f, w * 0.04f);
    g.setColour (skinTone);
    g.fillEllipse (px (0.17f), py (0.68f), w * 0.10f, h * 0.06f);
    g.fillEllipse (px (0.73f), py (0.68f), w * 0.10f, h * 0.06f);

    // ---- head: tall, angular ---------------------------------------------
    juce::Path head;
    head.startNewSubPath (px (0.40f), py (0.26f));
    head.lineTo (px (0.385f), py (0.11f));
    head.quadraticTo (px (0.50f), py (0.03f), px (0.615f), py (0.11f));
    head.lineTo (px (0.60f), py (0.26f));
    head.closeSubPath();

    g.setColour (skinTone);
    g.fillPath (head);
    g.setColour (skinShade);
    g.strokePath (head, juce::PathStrokeType (1.2f));

    // the nose: long, pointed, unmistakable
    juce::Path nose;
    nose.startNewSubPath (px (0.60f), py (0.155f));
    nose.lineTo (px (0.735f), py (0.185f));
    nose.lineTo (px (0.60f), py (0.215f));
    nose.closeSubPath();
    g.setColour (skinTone);
    g.fillPath (nose);
    g.setColour (skinShade);
    g.strokePath (nose, juce::PathStrokeType (1.0f));

    // hair: thin side tufts, bald crown
    g.setColour (hairBrown);
    juce::Path hair;
    hair.startNewSubPath (px (0.385f), py (0.115f));
    hair.quadraticTo (px (0.36f), py (0.075f), px (0.40f), py (0.065f));
    hair.quadraticTo (px (0.44f), py (0.055f), px (0.47f), py (0.068f));
    g.strokePath (hair, juce::PathStrokeType (2.2f));

    juce::Path hair2;
    hair2.startNewSubPath (px (0.615f), py (0.115f));
    hair2.quadraticTo (px (0.635f), py (0.075f), px (0.60f), py (0.062f));
    g.strokePath (hair2, juce::PathStrokeType (2.2f));

    // eyes
    g.setColour (juce::Colours::white);
    g.fillEllipse (px (0.445f), py (0.135f), w * 0.048f, h * 0.038f);
    g.fillEllipse (px (0.525f), py (0.135f), w * 0.048f, h * 0.038f);
    g.setColour (juce::Colour (0xff222028));
    g.fillEllipse (px (0.462f), py (0.144f), w * 0.020f, h * 0.020f);
    g.fillEllipse (px (0.542f), py (0.144f), w * 0.020f, h * 0.020f);

    // eyebrows, angled for permanent scheming
    g.setColour (hairBrown);
    g.drawLine (px (0.435f), py (0.128f), px (0.495f), py (0.118f), 2.0f);
    g.drawLine (px (0.515f), py (0.118f), px (0.575f), py (0.128f), 2.0f);

    // mouth: opens while the machine is talking
    const float mo = juce::jlimit (0.0f, 1.0f, mouthOpen01);
    g.setColour (juce::Colour (0xff5a2a2a));
    g.fillEllipse (px (0.465f), py (0.205f), w * 0.07f, h * (0.012f + 0.035f * mo));
}

//==============================================================================
void drawDeiLogo (juce::Graphics& g, juce::Rectangle<float> r)
{
    // The building: tall, narrow, oddly-shaped, with the sign across it
    const float w = r.getWidth(), h = r.getHeight();
    auto px = [&] (float f) { return r.getX() + f * w; };
    auto py = [&] (float f) { return r.getY() + f * h; };

    juce::Path tower;
    tower.startNewSubPath (px (0.32f), py (1.0f));
    tower.lineTo (px (0.32f), py (0.26f));
    tower.lineTo (px (0.40f), py (0.26f));
    tower.lineTo (px (0.40f), py (0.10f));
    tower.lineTo (px (0.62f), py (0.10f));
    tower.lineTo (px (0.62f), py (0.26f));
    tower.lineTo (px (0.70f), py (0.26f));
    tower.lineTo (px (0.70f), py (1.0f));
    tower.closeSubPath();

    g.setColour (purpleMid);
    g.fillPath (tower);
    g.setColour (purpleLight);
    g.strokePath (tower, juce::PathStrokeType (1.4f));

    // windows
    g.setColour (warningYellow.withAlpha (0.55f));
    for (int row = 0; row < 6; ++row)
        for (int col = 0; col < 3; ++col)
            g.fillRect (px (0.37f + (float) col * 0.11f),
                        py (0.34f + (float) row * 0.10f),
                        w * 0.06f, h * 0.05f);

    // the sign band
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRect (px (0.16f), py (0.44f), w * 0.70f, h * 0.13f);
    g.setColour (mintBright);
    g.setFont (DoofLookAndFeel::machineFont (h * 0.085f, true));
    g.drawText ("DOOFENSHMIRTZ", juce::Rectangle<float> (px (0.16f), py (0.445f),
                                                         w * 0.70f, h * 0.06f),
                juce::Justification::centred, false);
    g.setFont (DoofLookAndFeel::machineFont (h * 0.06f, true));
    g.drawText ("EVIL INCORPORATED",
                juce::Rectangle<float> (px (0.16f), py (0.505f), w * 0.70f, h * 0.055f),
                juce::Justification::centred, false);
}

//==============================================================================
void drawSkyline (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    g.setColour (c);
    juce::Random rng (1902);   // fixed seed: the skyline shouldn't flicker

    float x = r.getX();
    while (x < r.getRight())
    {
        const float bw = 12.0f + rng.nextFloat() * 26.0f;
        const float bh = r.getHeight() * (0.25f + rng.nextFloat() * 0.7f);
        g.fillRect (x, r.getBottom() - bh, bw, bh);

        // a few lit windows
        g.setColour (c.brighter (0.5f).withAlpha (0.35f));
        for (float wy = r.getBottom() - bh + 5.0f; wy < r.getBottom() - 6.0f; wy += 9.0f)
            for (float wx = x + 3.0f; wx < x + bw - 4.0f; wx += 7.0f)
                if (rng.nextFloat() > 0.55f)
                    g.fillRect (wx, wy, 2.5f, 3.5f);
        g.setColour (c);

        x += bw + 3.0f + rng.nextFloat() * 6.0f;
    }
}

//==============================================================================
void drawPlatypus (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float w = r.getWidth(), h = r.getHeight();
    auto px = [&] (float f) { return r.getX() + f * w; };
    auto py = [&] (float f) { return r.getY() + f * h; };

    const juce::Colour body { 0xff4aa3c7 };

    // body
    g.setColour (body);
    g.fillEllipse (px (0.20f), py (0.35f), w * 0.60f, h * 0.45f);

    // tail
    juce::Path tail;
    tail.startNewSubPath (px (0.74f), py (0.58f));
    tail.lineTo (px (0.98f), py (0.48f));
    tail.lineTo (px (0.98f), py (0.72f));
    tail.closeSubPath();
    g.setColour (juce::Colour (0xff8a6a3a));
    g.fillPath (tail);

    // head + bill
    g.setColour (body);
    g.fillEllipse (px (0.06f), py (0.26f), w * 0.34f, h * 0.32f);
    g.setColour (juce::Colour (0xffe08a3c));
    g.fillEllipse (px (-0.04f), py (0.36f), w * 0.24f, h * 0.14f);

    // eyes
    g.setColour (juce::Colours::white);
    g.fillEllipse (px (0.14f), py (0.29f), w * 0.09f, h * 0.10f);
    g.fillEllipse (px (0.24f), py (0.29f), w * 0.09f, h * 0.10f);
    g.setColour (juce::Colours::black);
    g.fillEllipse (px (0.17f), py (0.32f), w * 0.035f, h * 0.045f);
    g.fillEllipse (px (0.27f), py (0.32f), w * 0.035f, h * 0.045f);

    // the fedora
    g.setColour (juce::Colour (0xff6b4a33));
    g.fillRect (px (0.02f), py (0.24f), w * 0.42f, h * 0.045f);
    g.fillRoundedRectangle (px (0.12f), py (0.11f), w * 0.22f, h * 0.14f, w * 0.03f);
}

//==============================================================================
juce::String quoteForIdle()
{
    static const char* lines[] = {
        "BEHOLD! The LISTEN-INATOR!",
        "Ah, Perry the Platypus. Unexpected... but not unexpected.",
        "Press the button. PRESS IT.",
        "It does nothing yet. That is the tragedy of my life."
    };
    return lines[juce::Random::getSystemRandom().nextInt (4)];
}

juce::String quoteForListening()
{
    static const char* lines[] = {
        "Listening... LISTENING! Mwa-ha-ha!",
        "Back in Gimmelshtump, we had no spectrum analysers...",
        "Analysing your vocal. Do NOT touch anything.",
        "This is the part where I explain my whole scheme."
    };
    return lines[juce::Random::getSystemRandom().nextInt (4)];
}

juce::String quoteForAnalysed()
{
    static const char* lines[] = {
        "SUCCESS! Curse you, imperfect gain staging!",
        "The vocal is MINE. And also fixed. Mostly fixed.",
        "Behold, a professionally mixed vocal! I'm as surprised as you are.",
        "Now the entire Tri-State Area will hear this vocal correctly!"
    };
    return lines[juce::Random::getSystemRandom().nextInt (4)];
}

juce::String quoteForBypassed()
{
    static const char* lines[] = {
        "You've deactivated half my -inator. Rude.",
        "Fine. FINE! Do it your way.",
        "Curse you, bypass switch!"
    };
    return lines[juce::Random::getSystemRandom().nextInt (3)];
}

} // namespace listenator::doofart
