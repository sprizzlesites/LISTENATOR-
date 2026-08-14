#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace listenator::doofart
{

/** ---------------------------------------------------------------------------
    ISOLATED THEME ART.

    Everything in this file is the Phineas and Ferb / Doofenshmirtz Evil
    Incorporated homage: the character caricature, the DEI logo and the quote
    bank. Those elements are Disney-owned intellectual property.

    This is deliberately the only file that touches them. Replacing this one
    translation unit with original artwork makes the plugin distributable
    without changing a single line anywhere else in the codebase.
    ------------------------------------------------------------------------ */

/** Vector caricature of the doctor: lab coat, pointed nose, hunched posture. */
void drawDoctor (juce::Graphics&, juce::Rectangle<float>, float mouthOpen01);

/** The Doofenshmirtz Evil Incorporated block logo. */
void drawDeiLogo (juce::Graphics&, juce::Rectangle<float>);

/** Danville skyline silhouette, used as a backdrop band. */
void drawSkyline (juce::Graphics&, juce::Rectangle<float>, juce::Colour);

/** A semi-aquatic egg-laying mammal of action, for the corner. */
void drawPlatypus (juce::Graphics&, juce::Rectangle<float>);

/** Quote bank, keyed to plugin state. */
juce::String quoteForIdle();
juce::String quoteForListening();
juce::String quoteForAnalysed();
juce::String quoteForBypassed();

} // namespace listenator::doofart
