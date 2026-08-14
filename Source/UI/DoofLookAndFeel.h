#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace listenator
{

/** Doofenshmirtz Evil Incorporated house style.

    Purple/green 70s evil-architecture palette, riveted metal panels, warning
    stripes and CRT green. Everything is drawn in code -- no image assets.
*/
namespace doof
{
    // Core palette, from the DEI building's purple/green scheme
    const juce::Colour purpleDark   { 0xff2a1f3d };
    const juce::Colour purpleMid    { 0xff6f65a0 };
    const juce::Colour purpleLight  { 0xff9282ba };
    const juce::Colour mintGreen    { 0xff5dc89b };
    const juce::Colour mintBright   { 0xff73f3bd };
    const juce::Colour panelGrey    { 0xff656a62 };
    const juce::Colour metalDark    { 0xff3a3a42 };
    const juce::Colour metalMid     { 0xff5a5a66 };
    const juce::Colour metalLight   { 0xff8a8a99 };
    const juce::Colour warningYellow{ 0xffe8c547 };
    const juce::Colour dangerRed    { 0xffd0392b };
    const juce::Colour crtGreen     { 0xff41ff8a };
    const juce::Colour crtDim       { 0xff0d2418 };
    const juce::Colour boltGrey     { 0xff9a9aa8 };

    /** Riveted brushed-metal panel with bevel and corner bolts. */
    void drawMetalPanel (juce::Graphics&, juce::Rectangle<float>,
                         float cornerSize = 6.0f, bool recessed = false);

    /** Diagonal hazard stripes, used to frame the dangerous controls. */
    void drawHazardStripes (juce::Graphics&, juce::Rectangle<float>,
                            juce::Colour a = warningYellow,
                            juce::Colour b = juce::Colour (0xff1a1a1a));

    /** Glowing CRT-style screen with scanlines and a vignette. */
    void drawCrtScreen (juce::Graphics&, juce::Rectangle<float>);

    /** A single bolt/rivet head. */
    void drawRivet (juce::Graphics&, juce::Point<float> centre, float radius);

    /** Exposed wiring: a hanging catenary cable between two points. */
    void drawCable (juce::Graphics&, juce::Point<float> from, juce::Point<float> to,
                    juce::Colour, float thickness = 3.0f, float sag = 18.0f);

    /** A vacuum tube that glows according to level (0..1). */
    void drawVacuumTube (juce::Graphics&, juce::Rectangle<float>, float glow);
}

//==============================================================================
class DoofLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DoofLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;

    /** Chunky condensed lettering for the machine's stencilled labels. */
    static juce::Font machineFont (float height, bool bold = true);
};

} // namespace listenator
