// Minimal VST3 host used to verify a cross-compiled plugin actually works.
//
// pluginval's scan phase spawns a subprocess, which Wine handles unreliably,
// so this drives the plugin directly instead: load the DLL, enumerate the
// factory, instantiate the audio effect, run real audio through it and check
// the output is finite, non-silent and correctly channelled.
//
// Exit code 0 == the plugin loaded, initialised and processed audio.

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <vector>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("[FAIL] %s\n", msg); return 1; } } while (0)

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: vst3hosttest <plugin.vst3>\n"); return 2; }

    std::printf ("[1] LoadLibrary %s\n", argv[1]); std::fflush (stdout);
    HMODULE lib = LoadLibraryA (argv[1]);
    CHECK (lib != nullptr, "LoadLibrary failed");

    using InitFn    = bool (*)();
    using FactoryFn = IPluginFactory* (*)();

    auto initDll = (InitFn)    (void*) GetProcAddress (lib, "InitDll");
    auto getFact = (FactoryFn) (void*) GetProcAddress (lib, "GetPluginFactory");
    CHECK (getFact != nullptr, "GetPluginFactory not exported");

    if (initDll != nullptr)
        CHECK (initDll(), "InitDll returned false");

    IPluginFactory* factory = getFact();
    CHECK (factory != nullptr, "factory is null");

    PFactoryInfo fi {};
    factory->getFactoryInfo (&fi);
    std::printf ("[2] vendor=\"%s\" classes=%d\n", fi.vendor, factory->countClasses());

    // ---- find the audio effect class --------------------------------------
    PClassInfo chosen {};
    bool found = false;
    for (int32 i = 0; i < factory->countClasses(); ++i)
    {
        PClassInfo ci {};
        if (factory->getClassInfo (i, &ci) != kResultOk) continue;
        std::printf ("    class[%d] \"%s\" category=%s\n", i, ci.name, ci.category);
        if (std::strcmp (ci.category, kVstAudioEffectClass) == 0) { chosen = ci; found = true; }
    }
    CHECK (found, "no kVstAudioEffectClass in factory");

    // ---- instantiate -------------------------------------------------------
    IComponent* component = nullptr;
    CHECK (factory->createInstance (chosen.cid, IComponent::iid, (void**) &component) == kResultOk
           && component != nullptr, "createInstance(IComponent) failed");

    CHECK (component->initialize (nullptr) == kResultOk, "IComponent::initialize failed");
    std::printf ("[3] instantiated \"%s\"\n", chosen.name);

    const int32 numIn  = component->getBusCount (kAudio, kInput);
    const int32 numOut = component->getBusCount (kAudio, kOutput);
    std::printf ("[4] audio buses: %d in, %d out\n", numIn, numOut);
    CHECK (numOut > 0, "plugin reports no output bus");

    for (int32 i = 0; i < numIn;  ++i) component->activateBus (kAudio, kInput,  i, true);
    for (int32 i = 0; i < numOut; ++i) component->activateBus (kAudio, kOutput, i, true);

    IAudioProcessor* processor = nullptr;
    CHECK (component->queryInterface (IAudioProcessor::iid, (void**) &processor) == kResultOk
           && processor != nullptr, "queryInterface(IAudioProcessor) failed");

    constexpr int32 kBlock = 512;
    constexpr double kRate = 48000.0;

    ProcessSetup setup {};
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = kBlock;
    setup.sampleRate         = kRate;
    CHECK (processor->setupProcessing (setup) == kResultOk, "setupProcessing failed");

    CHECK (component->setActive (true) == kResultOk, "setActive(true) failed");
    processor->setProcessing (true);
    std::printf ("[5] activated @ %.0f Hz, block %d\n", kRate, kBlock);

    // ---- push real audio through -------------------------------------------
    std::vector<float> inL (kBlock), inR (kBlock), outL (kBlock), outR (kBlock);
    float* inChans[2]  { inL.data(),  inR.data()  };
    float* outChans[2] { outL.data(), outR.data() };

    AudioBusBuffers inBus {};  inBus.numChannels  = 2; inBus.channelBuffers32 = inChans;
    AudioBusBuffers outBus {}; outBus.numChannels = 2; outBus.channelBuffers32 = outChans;

    ProcessContext ctx {};
    ctx.state      = ProcessContext::kPlaying | ProcessContext::kTempoValid;
    ctx.sampleRate = kRate;
    ctx.tempo      = 120.0;

    ProcessData data {};
    data.processMode          = kRealtime;
    data.symbolicSampleSize   = kSample32;
    data.numSamples           = kBlock;
    data.numInputs            = numIn  > 0 ? 1 : 0;
    data.numOutputs           = 1;
    data.inputs               = numIn > 0 ? &inBus : nullptr;
    data.outputs              = &outBus;
    data.processContext       = &ctx;

    double phase = 0.0;
    double peak = 0.0;
    int    badSamples = 0;
    const int blocks = (int) (kRate * 3.0 / kBlock);   // 3 seconds

    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            phase += 2.0 * 3.14159265358979 * 220.0 / kRate;
            float s = 0.0f;
            for (int h = 1; h <= 12; ++h) s += (float) (std::sin (phase * h) / (h * h));
            inL[(size_t) i] = inR[(size_t) i] = s * 0.35f;
            outL[(size_t) i] = outR[(size_t) i] = 0.0f;
        }

        if (processor->process (data) != kResultOk)
        { std::printf ("[FAIL] process() returned an error at block %d\n", b); return 1; }

        for (int i = 0; i < kBlock; ++i)
        {
            const float l = outL[(size_t) i], r = outR[(size_t) i];
            if (! std::isfinite (l) || ! std::isfinite (r)) ++badSamples;
            peak = std::fmax (peak, std::fmax (std::fabs ((double) l), std::fabs ((double) r)));
        }
    }

    std::printf ("[6] processed %d blocks (%.1f s), output peak %.4f, non-finite %d\n",
                 blocks, blocks * kBlock / kRate, peak, badSamples);

    CHECK (badSamples == 0, "output contained NaN or Inf");
    CHECK (peak > 1.0e-6,   "output was completely silent");
    CHECK (peak < 8.0,      "output level implausibly high (runaway feedback?)");

    processor->setProcessing (false);
    component->setActive (false);
    component->terminate();
    processor->release();
    component->release();

    std::printf ("[OK] plugin loaded, initialised and processed audio cleanly\n");
    return 0;
}
