#include "DoofArt.h"
#include "DoofLookAndFeel.h"

namespace listenator::doofart
{

using namespace listenator::doof;

namespace
{
    const juce::Colour coatWhite  { 0xffeceaef };
    const juce::Colour coatShade  { 0xffc4c0cb };
    const juce::Colour coatLine   { 0xff8e8a97 };
    const juce::Colour skinTone   { 0xffe6bb92 };
    const juce::Colour skinShade  { 0xffcb9c73 };
    const juce::Colour hairBrown  { 0xff7a5334 };
    const juce::Colour trouserTan { 0xff9a8f79 };
    const juce::Colour turtleneck { 0xff2f6f62 };

    const juce::Colour perryTeal  { 0xff54b8c4 };
    const juce::Colour perryDark  { 0xff3f97a5 };
    const juce::Colour perryBill  { 0xffe89b3f };
    const juce::Colour perryHat   { 0xff6b4a2f };
    const juce::Colour perryBand  { 0xff4a3220 };

    /** Soft drop shadow behind a filled path. */
    void shadowed (juce::Graphics& g, const juce::Path& p, juce::Colour fill,
                   float offset = 2.0f, float alpha = 0.28f)
    {
        auto shadow = p;
        shadow.applyTransform (juce::AffineTransform::translation (offset, offset));
        g.setColour (juce::Colours::black.withAlpha (alpha));
        g.fillPath (shadow);
        g.setColour (fill);
        g.fillPath (p);
    }
}

//==============================================================================
/** Dr. Doofenshmirtz.

    The silhouette is the recognisable part: extremely tall and lanky, a long
    pointed nose that reads as a triangle from the side, a narrow tapering head
    with a receding hairline, and a lab coat that flares below the waist over
    very thin legs. Proportions are roughly eight heads tall with the head
    occupying the top eighth.
*/
void drawDoctor (juce::Graphics& g, juce::Rectangle<float> r, float mouthOpen01)
{
    const float w = r.getWidth(), h = r.getHeight();
    auto px = [&] (float f) { return r.getX() + f * w; };
    auto py = [&] (float f) { return r.getY() + f * h; };

    const float mo = juce::jlimit (0.0f, 1.0f, mouthOpen01);

    // ---- legs: long, thin, slightly bowed --------------------------------
    {
        juce::Path legs;
        legs.startNewSubPath (px (0.435f), py (0.66f));
        legs.lineTo (px (0.415f), py (0.93f));
        legs.lineTo (px (0.470f), py (0.93f));
        legs.lineTo (px (0.478f), py (0.66f));
        legs.closeSubPath();
        legs.startNewSubPath (px (0.522f), py (0.66f));
        legs.lineTo (px (0.530f), py (0.93f));
        legs.lineTo (px (0.585f), py (0.93f));
        legs.lineTo (px (0.565f), py (0.66f));
        legs.closeSubPath();
        shadowed (g, legs, trouserTan);
    }

    // shoes: flat, wide, dark
    g.setColour (juce::Colour (0xff33302c));
    g.fillRoundedRectangle (px (0.385f), py (0.925f), w * 0.11f, h * 0.035f, w * 0.012f);
    g.fillRoundedRectangle (px (0.520f), py (0.925f), w * 0.11f, h * 0.035f, w * 0.012f);

    // ---- lab coat: narrow shoulders, flaring hem -------------------------
    {
        juce::Path coat;
        coat.startNewSubPath (px (0.445f), py (0.225f));   // left shoulder
        coat.lineTo (px (0.360f), py (0.285f));
        coat.lineTo (px (0.332f), py (0.480f));            // taper in at waist
        coat.lineTo (px (0.318f), py (0.700f));            // flare out at hem
        coat.lineTo (px (0.430f), py (0.716f));
        coat.lineTo (px (0.500f), py (0.700f));
        coat.lineTo (px (0.570f), py (0.716f));
        coat.lineTo (px (0.682f), py (0.700f));
        coat.lineTo (px (0.668f), py (0.480f));
        coat.lineTo (px (0.640f), py (0.285f));
        coat.lineTo (px (0.555f), py (0.225f));            // right shoulder
        coat.closeSubPath();
        shadowed (g, coat, coatWhite, 2.5f, 0.30f);

        g.setColour (coatShade);
        g.strokePath (coat, juce::PathStrokeType (1.3f));
    }

    // turtleneck showing at the collar
    {
        juce::Path neck;
        neck.startNewSubPath (px (0.462f), py (0.196f));
        neck.lineTo (px (0.462f), py (0.240f));
        neck.lineTo (px (0.538f), py (0.240f));
        neck.lineTo (px (0.538f), py (0.196f));
        neck.closeSubPath();
        g.setColour (turtleneck);
        g.fillPath (neck);
        g.setColour (turtleneck.darker (0.3f));
        g.strokePath (neck, juce::PathStrokeType (1.0f));
    }

    // coat opening: a deep V down the front
    {
        juce::Path v;
        v.startNewSubPath (px (0.445f), py (0.232f));
        v.lineTo (px (0.500f), py (0.470f));
        v.lineTo (px (0.555f), py (0.232f));
        g.setColour (coatLine);
        g.strokePath (v, juce::PathStrokeType (1.6f));

        // the turtleneck visible through the V
        juce::Path inner;
        inner.startNewSubPath (px (0.452f), py (0.236f));
        inner.lineTo (px (0.500f), py (0.462f));
        inner.lineTo (px (0.548f), py (0.236f));
        inner.closeSubPath();
        g.setColour (turtleneck.withAlpha (0.85f));
        g.fillPath (inner);
    }

    // coat pocket + hem line
    g.setColour (coatLine.withAlpha (0.7f));
    g.drawLine (px (0.360f), py (0.560f), px (0.415f), py (0.560f), 1.2f);
    g.drawLine (px (0.585f), py (0.560f), px (0.640f), py (0.560f), 1.2f);

    // ---- arms: very thin, hanging long -----------------------------------
    {
        juce::Path arms;
        arms.startNewSubPath (px (0.362f), py (0.290f));
        arms.lineTo (px (0.322f), py (0.470f));
        arms.lineTo (px (0.300f), py (0.610f));
        g.setColour (coatWhite);
        g.strokePath (arms, juce::PathStrokeType (w * 0.052f,
                            juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (coatShade);
        g.strokePath (arms, juce::PathStrokeType (1.0f));

        juce::Path arms2;
        arms2.startNewSubPath (px (0.638f), py (0.290f));
        arms2.lineTo (px (0.678f), py (0.470f));
        arms2.lineTo (px (0.700f), py (0.610f));
        g.setColour (coatWhite);
        g.strokePath (arms2, juce::PathStrokeType (w * 0.052f,
                             juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (coatShade);
        g.strokePath (arms2, juce::PathStrokeType (1.0f));
    }

    // long thin hands
    g.setColour (skinTone);
    g.fillEllipse (px (0.278f), py (0.605f), w * 0.046f, h * 0.048f);
    g.fillEllipse (px (0.676f), py (0.605f), w * 0.046f, h * 0.048f);
    g.setColour (skinShade);
    g.drawEllipse (px (0.278f), py (0.605f), w * 0.046f, h * 0.048f, 0.8f);
    g.drawEllipse (px (0.676f), py (0.605f), w * 0.046f, h * 0.048f, 0.8f);

    // ---- neck --------------------------------------------------------------
    g.setColour (skinTone);
    g.fillRect (px (0.478f), py (0.170f), w * 0.044f, h * 0.035f);

    // ---- head: narrow, tapering to a pointed chin -------------------------
    {
        juce::Path head;
        head.startNewSubPath (px (0.432f), py (0.052f));   // top left
        head.quadraticTo (px (0.500f), py (0.022f), px (0.568f), py (0.052f));
        head.lineTo (px (0.576f), py (0.108f));            // cheekbone
        head.lineTo (px (0.548f), py (0.170f));            // taper to jaw
        head.lineTo (px (0.500f), py (0.186f));            // pointed chin
        head.lineTo (px (0.452f), py (0.170f));
        head.lineTo (px (0.424f), py (0.108f));
        head.closeSubPath();
        shadowed (g, head, skinTone, 2.0f, 0.25f);
        g.setColour (skinShade);
        g.strokePath (head, juce::PathStrokeType (1.1f));
    }

    // ---- THE nose: long, sharp, angling down and to the right -------------
    {
        juce::Path nose;
        nose.startNewSubPath (px (0.548f), py (0.094f));
        nose.lineTo (px (0.686f), py (0.126f));            // the point
        nose.lineTo (px (0.546f), py (0.136f));
        nose.closeSubPath();
        g.setColour (skinTone);
        g.fillPath (nose);
        g.setColour (skinShade);
        g.strokePath (nose, juce::PathStrokeType (1.1f));

        // underside shading gives it volume
        juce::Path underside;
        underside.startNewSubPath (px (0.686f), py (0.126f));
        underside.lineTo (px (0.546f), py (0.136f));
        underside.lineTo (px (0.590f), py (0.130f));
        underside.closeSubPath();
        g.setColour (skinShade.withAlpha (0.5f));
        g.fillPath (underside);
    }

    // ---- hair: receding, swept back, thin at the temples ------------------
    {
        juce::Path hair;
        hair.startNewSubPath (px (0.424f), py (0.100f));
        hair.lineTo (px (0.428f), py (0.056f));
        hair.quadraticTo (px (0.500f), py (0.020f), px (0.572f), py (0.054f));
        hair.lineTo (px (0.576f), py (0.076f));
        // the receding hairline dips down in the middle
        hair.quadraticTo (px (0.532f), py (0.050f), px (0.500f), py (0.062f));
        hair.quadraticTo (px (0.468f), py (0.050f), px (0.432f), py (0.078f));
        hair.closeSubPath();
        g.setColour (hairBrown);
        g.fillPath (hair);
        g.setColour (hairBrown.darker (0.35f));
        g.strokePath (hair, juce::PathStrokeType (0.9f));

        // sideburns
        g.setColour (hairBrown);
        g.fillRect (px (0.424f), py (0.084f), w * 0.014f, h * 0.030f);
        g.fillRect (px (0.562f), py (0.084f), w * 0.014f, h * 0.030f);
    }

    // ---- eyes: small, close-set, permanently harried ----------------------
    g.setColour (juce::Colours::white);
    g.fillEllipse (px (0.466f), py (0.086f), w * 0.036f, h * 0.030f);
    g.fillEllipse (px (0.514f), py (0.086f), w * 0.036f, h * 0.030f);
    g.setColour (skinShade.withAlpha (0.6f));
    g.drawEllipse (px (0.466f), py (0.086f), w * 0.036f, h * 0.030f, 0.7f);
    g.drawEllipse (px (0.514f), py (0.086f), w * 0.036f, h * 0.030f, 0.7f);

    g.setColour (juce::Colour (0xff1e1c22));
    g.fillEllipse (px (0.480f), py (0.093f), w * 0.014f, h * 0.015f);
    g.fillEllipse (px (0.528f), py (0.093f), w * 0.014f, h * 0.015f);

    // eyebrows angled inward: the permanent scheme
    g.setColour (hairBrown.darker (0.2f));
    g.drawLine (px (0.458f), py (0.080f), px (0.504f), py (0.072f), 2.2f);
    g.drawLine (px (0.512f), py (0.072f), px (0.558f), py (0.080f), 2.2f);

    // ---- mouth: wide, opens while the machine talks -----------------------
    {
        const float mw = w * (0.052f + 0.028f * mo);
        const float mh = h * (0.008f + 0.030f * mo);
        auto mouth = juce::Rectangle<float> (px (0.500f) - mw * 0.5f,
                                             py (0.146f), mw, mh);
        g.setColour (juce::Colour (0xff5c2b2b));
        g.fillRoundedRectangle (mouth, mh * 0.4f);

        if (mo > 0.35f)   // tongue only when properly open
        {
            g.setColour (juce::Colour (0xffb3595e));
            g.fillEllipse (mouth.reduced (mw * 0.22f, mh * 0.42f)
                                .translated (0.0f, mh * 0.22f));
        }
    }
}

//==============================================================================
/** The DEI building: tall, narrow, top-heavy, with the sign band across it. */
void drawDeiLogo (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float w = r.getWidth(), h = r.getHeight();
    auto px = [&] (float f) { return r.getX() + f * w; };
    auto py = [&] (float f) { return r.getY() + f * h; };

    // the distinctive stepped silhouette
    juce::Path tower;
    tower.startNewSubPath (px (0.24f), py (1.00f));
    tower.lineTo (px (0.24f), py (0.34f));
    tower.lineTo (px (0.32f), py (0.34f));
    tower.lineTo (px (0.32f), py (0.16f));
    tower.lineTo (px (0.40f), py (0.16f));
    tower.lineTo (px (0.40f), py (0.06f));
    tower.lineTo (px (0.72f), py (0.06f));
    tower.lineTo (px (0.72f), py (0.20f));
    tower.lineTo (px (0.80f), py (0.20f));
    tower.lineTo (px (0.80f), py (1.00f));
    tower.closeSubPath();

    g.setGradientFill (juce::ColourGradient (purpleLight, r.getX(), r.getY(),
                                             purpleMid.darker (0.4f), r.getX(), r.getBottom(),
                                             false));
    g.fillPath (tower);
    g.setColour (purpleLight.brighter (0.3f));
    g.strokePath (tower, juce::PathStrokeType (1.2f));

    // window grid
    g.setColour (warningYellow.withAlpha (0.5f));
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 4; ++col)
        {
            const float wx = px (0.30f + (float) col * 0.12f);
            const float wy = py (0.30f + (float) row * 0.095f);
            if (wy < py (0.34f) && wx < px (0.32f)) continue;
            g.fillRect (wx, wy, w * 0.06f, h * 0.045f);
        }

    // the little spire
    g.setColour (metalLight);
    g.drawLine (px (0.56f), py (0.06f), px (0.56f), py (-0.04f), 1.6f);
    g.setColour (dangerRed);
    g.fillEllipse (px (0.545f), py (-0.06f), w * 0.03f, w * 0.03f);
}

//==============================================================================
void drawSkyline (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    juce::Random rng (1902);   // fixed seed: the skyline must not flicker
    float x = r.getX();

    while (x < r.getRight())
    {
        const float bw = 12.0f + rng.nextFloat() * 26.0f;
        const float bh = r.getHeight() * (0.25f + rng.nextFloat() * 0.7f);

        g.setColour (c);
        g.fillRect (x, r.getBottom() - bh, bw, bh);

        // a few lit windows
        g.setColour (warningYellow.withAlpha (0.22f));
        for (float wy = r.getBottom() - bh + 5.0f; wy < r.getBottom() - 6.0f; wy += 9.0f)
            for (float wx = x + 3.0f; wx < x + bw - 4.0f; wx += 7.0f)
                if (rng.nextFloat() > 0.55f)
                    g.fillRect (wx, wy, 2.5f, 3.5f);

        x += bw + 3.0f + rng.nextFloat() * 6.0f;
    }
}

//==============================================================================
/** Perry the Platypus in agent posture: upright, teal, orange bill and tail,
    brown fedora with a darker band, white eye patches. */
void drawPlatypus (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float w = r.getWidth(), h = r.getHeight();
    auto px = [&] (float f) { return r.getX() + f * w; };
    auto py = [&] (float f) { return r.getY() + f * h; };

    // ---- flat paddle tail, cross-hatched ----------------------------------
    {
        juce::Path tail;
        tail.startNewSubPath (px (0.66f), py (0.62f));
        tail.lineTo (px (0.99f), py (0.50f));
        tail.lineTo (px (0.99f), py (0.82f));
        tail.lineTo (px (0.66f), py (0.76f));
        tail.closeSubPath();
        g.setColour (perryBill.darker (0.25f));
        g.fillPath (tail);
        g.setColour (perryBill.darker (0.5f));
        g.strokePath (tail, juce::PathStrokeType (1.0f));
        for (int i = 1; i < 3; ++i)
            g.drawLine (px (0.66f + (float) i * 0.11f), py (0.56f),
                        px (0.66f + (float) i * 0.11f), py (0.79f), 0.8f);
    }

    // ---- body: rounded, wider at the bottom -------------------------------
    {
        juce::Path body;
        body.startNewSubPath (px (0.30f), py (0.40f));
        body.quadraticTo (px (0.22f), py (0.70f), px (0.36f), py (0.86f));
        body.quadraticTo (px (0.54f), py (0.96f), px (0.68f), py (0.80f));
        body.quadraticTo (px (0.76f), py (0.58f), px (0.64f), py (0.42f));
        body.closeSubPath();
        shadowed (g, body, perryTeal, 1.8f, 0.25f);
        g.setColour (perryDark);
        g.strokePath (body, juce::PathStrokeType (1.1f));
    }

    // lighter belly
    g.setColour (perryTeal.brighter (0.28f));
    g.fillEllipse (px (0.36f), py (0.60f), w * 0.28f, h * 0.28f);

    // ---- webbed feet -------------------------------------------------------
    g.setColour (perryBill);
    for (float fx : { 0.34f, 0.54f })
    {
        juce::Path foot;
        foot.startNewSubPath (px (fx), py (0.86f));
        foot.lineTo (px (fx - 0.06f), py (0.97f));
        foot.lineTo (px (fx + 0.14f), py (0.97f));
        foot.closeSubPath();
        g.fillPath (foot);
    }

    // ---- head: wide and flat-topped ---------------------------------------
    {
        juce::Path head;
        head.startNewSubPath (px (0.16f), py (0.36f));
        head.quadraticTo (px (0.16f), py (0.16f), px (0.42f), py (0.16f));
        head.quadraticTo (px (0.68f), py (0.16f), px (0.68f), py (0.38f));
        head.quadraticTo (px (0.60f), py (0.50f), px (0.34f), py (0.50f));
        head.closeSubPath();
        shadowed (g, head, perryTeal, 1.8f, 0.25f);
        g.setColour (perryDark);
        g.strokePath (head, juce::PathStrokeType (1.1f));
    }

    // ---- the bill: broad, flat, orange ------------------------------------
    {
        juce::Path bill;
        bill.startNewSubPath (px (0.20f), py (0.34f));
        bill.quadraticTo (px (-0.06f), py (0.38f), px (0.02f), py (0.48f));
        bill.quadraticTo (px (0.12f), py (0.54f), px (0.30f), py (0.48f));
        bill.closeSubPath();
        g.setColour (perryBill);
        g.fillPath (bill);
        g.setColour (perryBill.darker (0.4f));
        g.strokePath (bill, juce::PathStrokeType (1.0f));
        g.drawLine (px (0.06f), py (0.42f), px (0.26f), py (0.41f), 0.8f);
    }

    // ---- eyes: two white ovals sitting high, close together ---------------
    g.setColour (juce::Colours::white);
    g.fillEllipse (px (0.26f), py (0.20f), w * 0.17f, h * 0.17f);
    g.fillEllipse (px (0.43f), py (0.20f), w * 0.17f, h * 0.17f);
    g.setColour (perryDark.withAlpha (0.5f));
    g.drawEllipse (px (0.26f), py (0.20f), w * 0.17f, h * 0.17f, 0.8f);
    g.drawEllipse (px (0.43f), py (0.20f), w * 0.17f, h * 0.17f, 0.8f);

    g.setColour (juce::Colours::black);
    g.fillEllipse (px (0.335f), py (0.255f), w * 0.055f, h * 0.075f);
    g.fillEllipse (px (0.475f), py (0.255f), w * 0.055f, h * 0.075f);

    // ---- the fedora --------------------------------------------------------
    {
        // brim
        juce::Path brim;
        brim.addEllipse (px (0.04f), py (0.11f), w * 0.76f, h * 0.11f);
        shadowed (g, brim, perryHat, 1.5f, 0.3f);

        // crown with a pinched top
        juce::Path crown;
        crown.startNewSubPath (px (0.20f), py (0.14f));
        crown.lineTo (px (0.235f), py (-0.05f));
        crown.quadraticTo (px (0.42f), py (-0.10f), px (0.605f), py (-0.05f));
        crown.lineTo (px (0.64f), py (0.14f));
        crown.closeSubPath();
        g.setColour (perryHat);
        g.fillPath (crown);
        g.setColour (perryHat.darker (0.4f));
        g.strokePath (crown, juce::PathStrokeType (1.0f));

        // hat band
        g.setColour (perryBand);
        g.fillRect (px (0.205f), py (0.055f), w * 0.43f, h * 0.055f);
    }
}

//==============================================================================
juce::String quoteForIdle()
{
    static const char* lines[] = {
        "BEHOLD! The LISTEN-INATOR!",
        "Ah, Perry the Platypus. Unexpected... but not unexpected.",
        "It does nothing yet. That is the tragedy of my life.",
        "Curse you, unprocessed vocal!"
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
        "Behold, a professional vocal! I'm as surprised as you are.",
        "Now the whole Tri-State Area will hear this correctly!"
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
