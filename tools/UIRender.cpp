// Headless UI renderer.
//
// Instantiates the real ListenatorEditor and writes PNGs of it, so the UI can
// be reviewed without loading the plugin into a DAW. Built only when
// -DLISTENATOR_BUILD_UIRENDER=ON.
//
// Usage:  UIRender <output-dir> [--analysed]

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

class UIRenderApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "LISTENATOR-UIRender"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }

    void initialise (const juce::String& commandLine) override
    {
        auto args = juce::StringArray::fromTokens (commandLine, true);
        args.removeEmptyStrings();

        juce::File outDir (args.isEmpty()
            ? juce::File::getCurrentWorkingDirectory().getChildFile ("ui-shots")
            : juce::File (args[0].unquoted()));
        outDir.createDirectory();

        processor = std::make_unique<listenator::ListenatorProcessor>();
        processor->prepareToPlay (48000.0, 512);

        // Feed a synthetic vocal so the analysis-populated UI can be captured
        // too, not just the cold-start state.
        if (args.contains ("--analysed"))
            runSyntheticAnalysis();

        editor.reset (dynamic_cast<listenator::ListenatorEditor*> (processor->createEditor()));
        jassert (editor != nullptr);
        editor->setBounds (0, 0, 1100, 780);

        // let the editor's timer tick a few times so meters/lamps settle
        for (int i = 0; i < 6; ++i)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
            editor->repaint();
        }

        capture (outDir.getChildFile (args.contains ("--analysed")
                                        ? "listenator-analysed.png"
                                        : "listenator-idle.png"));

        quit();
    }

    void shutdown() override { editor.reset(); processor.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    /** Synthesise a deliberately flawed vocal-like signal and run the LISTEN
        pass on it, so the captured UI shows real derived values. */
    void runSyntheticAnalysis()
    {
        const double sr = 48000.0;
        const int    blockSize = 512;
        const int    totalBlocks = (int) (sr * 16.0 / blockSize);

        processor->triggerListen();

        juce::AudioBuffer<float> buf (2, blockSize);
        juce::MidiBuffer midi;
        juce::Random rng (7);

        double phase = 0.0;
        int    sampleCounter = 0;

        for (int b = 0; b < totalBlocks; ++b)
        {
            buf.clear();
            auto* d = buf.getWritePointer (0);

            for (int i = 0; i < blockSize; ++i, ++sampleCounter)
            {
                const double t = (double) sampleCounter / sr;

                // wandering fundamental around 196 Hz (G3) with vibrato + drift
                const double f0 = 196.0 * (1.0 + 0.012 * std::sin (t * 5.5))
                                        * (1.0 + 0.02 * std::sin (t * 0.4));
                phase += 2.0 * juce::MathConstants<double>::pi * f0 / sr;

                // glottal-ish harmonic stack with a -12 dB/oct rolloff
                float s = 0.0f;
                for (int h = 1; h <= 24; ++h)
                    s += (float) (std::sin (phase * h) / (h * h));

                // phrase envelope so there are gaps for the noise floor
                const float gate = (std::fmod (t, 2.2) < 1.5) ? 1.0f : 0.02f;

                // a resonant peak, a sibilant burst, and a noise floor
                const float sib = (std::fmod (t, 2.2) > 1.35 && std::fmod (t, 2.2) < 1.5)
                                ? rng.nextFloat() * 0.30f - 0.15f : 0.0f;
                const float noise = (rng.nextFloat() - 0.5f) * 0.0016f;

                d[i] = s * 0.34f * gate + sib + noise;
            }

            buf.copyFrom (1, 0, buf, 0, 0, blockSize);
            processor->processBlock (buf, midi);
        }

        // Poll until the background analysis has published and processBlock has
        // picked it up. A fixed sleep silently falls back to the idle UI when
        // the analysis takes longer than expected.
        for (int tries = 0; tries < 200 && ! processor->hasAnalysis(); ++tries)
        {
            juce::Thread::sleep (25);
            buf.clear();
            processor->processBlock (buf, midi);
        }

        // then run real audio so the meters and gauges have live values
        for (int b = 0; b < 120; ++b)
        {
            buf.clear();
            auto* d = buf.getWritePointer (0);
            for (int i = 0; i < blockSize; ++i, ++sampleCounter)
            {
                const double t = (double) sampleCounter / sr;
                phase += 2.0 * juce::MathConstants<double>::pi * 196.0 / sr;
                float sv = 0.0f;
                for (int h = 1; h <= 20; ++h) sv += (float) (std::sin (phase * h) / (h * h));
                d[i] = sv * 0.4f * (float) (0.7 + 0.3 * std::sin (t * 3.0));
            }
            buf.copyFrom (1, 0, buf, 0, 0, blockSize);
            processor->processBlock (buf, midi);
        }

        if (! processor->hasAnalysis())
            std::printf ("WARNING: analysis did not complete; UI will show idle state\n");
    }

    void capture (const juce::File& out)
    {
        juce::Image img (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
        {
            juce::Graphics g (img);
            editor->paintEntireComponent (g, true);
        }

        out.deleteFile();
        if (auto stream = out.createOutputStream())
        {
            juce::PNGImageFormat png;
            png.writeImageToStream (img, *stream);
        }
        std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
    }

    std::unique_ptr<listenator::ListenatorProcessor> processor;
    std::unique_ptr<listenator::ListenatorEditor>    editor;
};

START_JUCE_APPLICATION (UIRenderApp)
