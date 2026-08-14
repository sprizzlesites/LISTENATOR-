// Offline renderer: runs a real audio file through the plugin and writes the
// result, printing everything the analysis derived along the way.
//
// This is the tuning loop. Synthetic signals prove each stage does what it
// claims; only real material shows whether the derived VALUES are right.
//
// Usage:
//   Render <in.wav> <outdir> [--listen-at S] [--listen-len S] [--tempo BPM]
//          [--key N] [--scale N] [--tag NAME] [--trim-start S]

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace listenator;

namespace
{
constexpr int kBlock = 512;

float dbOf (float lin) { return lin > 1.0e-9f ? 20.0f * std::log10 (lin) : -120.0f; }

struct Stats
{
    float peakDb = -120.0f, rmsDb = -120.0f, crestDb = 0.0f;
    int   clipped = 0;
    float lufs = -120.0f;
};

Stats measure (const std::vector<float>& x, double sr)
{
    Stats s;
    double sumSq = 0.0;
    float peak = 0.0f;
    for (float v : x)
    {
        peak = std::max (peak, std::abs (v));
        sumSq += (double) v * v;
        if (std::abs (v) >= 0.999f) ++s.clipped;
    }
    s.peakDb = dbOf (peak);
    s.rmsDb  = 10.0f * (float) std::log10 (std::max (sumSq / std::max<size_t> (1, x.size()), 1e-20));
    s.crestDb = s.peakDb - s.rmsDb;

    // rough gated loudness: mean of 400 ms block powers above -70
    const int bl = (int) (0.4 * sr);
    std::vector<float> blocks;
    for (size_t i = 0; i + (size_t) bl <= x.size(); i += (size_t) bl / 4)
    {
        double a = 0.0;
        for (int k = 0; k < bl; ++k) a += (double) x[i + (size_t) k] * x[i + (size_t) k];
        const float d = 10.0f * (float) std::log10 (std::max (a / bl, 1e-20));
        if (d > -70.0f) blocks.push_back (d);
    }
    if (! blocks.empty())
    {
        double a = 0.0; for (float b : blocks) a += b;
        s.lufs = (float) (a / blocks.size()) - 0.691f;
    }
    return s;
}

/** Median level in the 80-250 ms window after a speech offset, relative to the
    level just before it -- the same quantity the analyser uses to decide how
    live the room is. Running it on the OUTPUT says whether de-verb worked. */
float measureTailDb (const std::vector<float>& x, double sr, float noiseFloorDb)
{
    const int frameLen = std::max (16, (int) (0.005 * sr));
    std::vector<float> envDb;
    for (size_t i = 0; i + (size_t) frameLen <= x.size(); i += (size_t) frameLen)
    {
        double a = 0.0;
        for (int k = 0; k < frameLen; ++k) a += (double) x[i + (size_t) k] * x[i + (size_t) k];
        envDb.push_back (10.0f * (float) std::log10 (std::max (a / frameLen, 1e-20)));
    }
    if (envDb.size() < 40) return -99.0f;

    const float frameSec = (float) frameLen / (float) sr;
    const int pre   = std::max (1, (int) (0.100 / frameSec));
    const int gapLo = std::max (1, (int) (0.080 / frameSec));
    const int gapHi = std::max (2, (int) (0.250 / frameSec));

    std::vector<float> ratios;
    for (size_t i = (size_t) pre; i + (size_t) gapHi < envDb.size(); ++i)
    {
        const float before = envDb[i];
        if (before < noiseFloorDb + 18.0f) continue;
        if (envDb[i + (size_t) gapLo] > before - 6.0f) continue;

        float preAcc = 0.0f;
        for (int k = 0; k < pre; ++k) preAcc += envDb[i - (size_t) k];
        preAcc /= (float) pre;

        float tailAcc = 0.0f, tailMax = -200.0f; int n = 0;
        for (int k = gapLo; k <= gapHi; ++k)
        { const float v = envDb[i + (size_t) k]; tailAcc += v; tailMax = std::max (tailMax, v); ++n; }
        tailAcc /= (float) std::max (1, n);

        if (tailMax > before - 6.0f) continue;
        if (tailAcc < noiseFloorDb + 3.0f) continue;
        ratios.push_back (tailAcc - preAcc);
    }
    if (ratios.empty()) return -99.0f;
    std::sort (ratios.begin(), ratios.end());
    return ratios[ratios.size() / 2];
}

/** Envelope modulation depth at syllable rate (2-8 Hz).

    This is the measure that matters for "boxy". Reverberation fills the valleys
    between syllables, so a washy recording has a shallower envelope than a dry
    one carrying the same words. Unlike the gap-tail figure it stays meaningful
    during continuous delivery, which is most of a rap vocal.

    Higher is drier. */
float measureModulationDepth (const std::vector<float>& x, double sr)
{
    // 20 ms envelope, which resolves syllables without tracking the waveform
    const int frameLen = std::max (16, (int) (0.020 * sr));
    std::vector<float> env;
    for (size_t i = 0; i + (size_t) frameLen <= x.size(); i += (size_t) frameLen)
    {
        double a = 0.0;
        for (int k = 0; k < frameLen; ++k) a += (double) x[i + (size_t) k] * x[i + (size_t) k];
        env.push_back ((float) std::sqrt (a / frameLen));
    }
    if (env.size() < 64) return 0.0f;

    // consider only the parts where there is actually a performance
    std::vector<float> active;
    float peak = 0.0f;
    for (float v : env) peak = std::max (peak, v);
    for (float v : env) if (v > peak * 0.02f) active.push_back (v);
    if (active.size() < 64) return 0.0f;

    double mean = 0.0;
    for (float v : active) mean += v;
    mean /= (double) active.size();
    if (mean < 1e-9) return 0.0f;

    // Bandpass the ENVELOPE to syllable rate before measuring its depth.
    //
    // A plain coefficient of variation counts take-to-take level jumps as
    // modulation, so correctly levelling punch-ins recorded 15 dB apart scores
    // as a loss -- which would push the tuning in exactly the wrong direction.
    // Only 2-8 Hz is speech rhythm; slower is gain staging, faster is noise.
    const double envRate = sr / (double) frameLen;      // ~50 Hz
    const double hpCoef = std::exp (-2.0 * juce::MathConstants<double>::pi * 2.0 / envRate);
    const double lpCoef = std::exp (-2.0 * juce::MathConstants<double>::pi * 8.0 / envRate);

    double lowState = active[0], bandState = 0.0, acc = 0.0;
    int n = 0;

    for (float v : active)
    {
        lowState = hpCoef * lowState + (1.0 - hpCoef) * v;   // slow trend
        const double highPassed = v - lowState;              // remove it
        bandState = lpCoef * bandState + (1.0 - lpCoef) * highPassed;
        acc += bandState * bandState;
        ++n;
    }

    if (n == 0) return 0.0f;
    return (float) (std::sqrt (acc / n) / mean);
}

void printLevelMap (const std::vector<float>& x, double sr, const char* label)
{
    std::printf ("  %s level map (2 s cells, dBFS RMS):\n    ", label);
    const int cell = (int) (2.0 * sr);
    int n = 0;
    for (size_t i = 0; i + (size_t) cell <= x.size(); i += (size_t) cell)
    {
        double a = 0.0;
        for (int k = 0; k < cell; ++k) a += (double) x[i + (size_t) k] * x[i + (size_t) k];
        const float d = 10.0f * (float) std::log10 (std::max (a / cell, 1e-20));
        std::printf ("%5.0f", d < -90.0f ? -99.0f : d);
        if (++n % 16 == 0) std::printf ("\n    ");
    }
    std::printf ("\n");
}

std::vector<float> loadWav (const juce::File& f, double& sr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    if (rd == nullptr) { std::printf ("cannot read %s\n", f.getFullPathName().toRawUTF8()); return {}; }

    sr = rd->sampleRate;
    juce::AudioBuffer<float> buf ((int) rd->numChannels, (int) rd->lengthInSamples);
    rd->read (&buf, 0, (int) rd->lengthInSamples, 0, true, true);

    std::vector<float> mono ((size_t) buf.getNumSamples());
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        float acc = 0.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c) acc += buf.getSample (c, i);
        mono[(size_t) i] = acc / (float) buf.getNumChannels();
    }
    return mono;
}

void writeWav (const juce::File& f, const std::vector<float>& l,
               const std::vector<float>& r, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    if (os == nullptr) return;

    std::unique_ptr<juce::AudioFormatWriter> w (
        fmt.createWriterFor (os.release(), sr, 2, 24, {}, 0));
    if (w == nullptr) return;

    const int n = (int) std::min (l.size(), r.size());
    juce::AudioBuffer<float> buf (2, n);
    for (int i = 0; i < n; ++i)
    { buf.setSample (0, i, l[(size_t) i]); buf.setSample (1, i, r[(size_t) i]); }
    w->writeFromAudioSampleBuffer (buf, 0, n);
}

void dumpAnalysis (const AnalysisResult& a)
{
    std::printf ("\n--- what the plugin decided -------------------------------\n");
    std::printf ("  level     peak %.1f dB   rms %.1f dB   LUFS %.1f   crest %.1f dB   LRA %.1f\n",
                 a.peakDb, a.rmsDb, a.integratedLufs, a.crestFactorDb, a.loudnessRangeDb);
    std::printf ("  noise     floor %.1f dB   SNR %.1f dB\n", a.noiseFloorDb, a.snrDb);
    std::printf ("  room      RT60 %.2f s   tail %.1f dB from %d gaps   ratio %.2f   -> deverb %.2f\n",
                 a.rt60Seconds, a.tailDb, a.tailSamples, a.reverbRatio, a.deverbAmount);
    std::printf ("            reverb during speech %.1f dB (gamma %.2f)\n",
                 a.directToReverbDb, std::pow (10.0f, a.directToReverbDb / 20.0f));
    std::printf ("  denoise   amount %.2f\n", a.denoiseAmount);
    std::printf ("  pitch     medianF0 %.1f Hz   p05 %.1f   p95 %.1f   voiced %.0f%%   drift %.1f cents\n",
                 a.medianF0Hz, a.f0P05Hz, a.f0P95Hz, a.voicedFraction * 100.0f,
                 a.pitchStabilityCents);
    std::printf ("  timbre    F1 %.0f  F2 %.0f  F3 %.0f   centroid %.0f Hz   tilt %.2f dB/oct\n",
                 a.f1Hz, a.f2Hz, a.f3Hz, a.spectralCentroidHz, a.spectralTiltDbPerOct);
    std::printf ("  hpf       %.0f Hz\n", a.highPassHz);
    std::printf ("  speech    quietest real delivery %.1f dB\n", a.speechFloorDb);
    std::printf ("  gate      thresh %.1f dB  range %.1f dB  rel %.0f ms\n",
                 a.gateThresholdDb, a.gateRangeDb, a.gateReleaseMs);
    std::printf ("  de-ess    centre %.0f Hz  bw %.2f oct  thresh %.1f dBFS  peak %.1f  spread %.1f  max %.1f dB\n",
                 a.deEssCentreHz, a.deEssBandwidthOct, a.deEssThresholdDb,
                 a.sibilantPeakDb, a.sibilantSpreadDb, a.deEssMaxReductionDb);
    std::printf ("  comp L    thresh %.1f  ratio %.2f  att %.0f ms  rel %.0f ms\n",
                 a.compLevelThreshDb, a.compLevelRatio, a.compLevelAttackMs, a.compLevelReleaseMs);
    std::printf ("  comp P    thresh %.1f  ratio %.2f  att %.1f ms  rel %.0f ms\n",
                 a.compPeakThreshDb, a.compPeakRatio, a.compPeakAttackMs, a.compPeakReleaseMs);
    std::printf ("  makeup    %.1f dB     saturation drive %.2f\n", a.makeupGainDb, a.saturationDrive);

    std::printf ("  res cands:");
    for (const auto& c : a.resonanceCandidates)
        std::printf ("  %.0fHz/%.1fdB/agree%.0f", c.frequencyHz, -c.gainDb, c.q);
    std::printf ("\n  resonances (%d):", (int) a.resonances.size());
    for (const auto& r : a.resonances)
        std::printf ("  %.0fHz/%.1fdB/Q%.1f", r.frequencyHz, r.gainDb, r.q);
    std::printf ("\n  tone match (dB per 1/3-oct band, 20 Hz..20 kHz):\n    ");
    for (int b = 0; b < numToneBands; ++b)
    {
        std::printf ("%5.1f", a.toneMatchDb[(size_t) b]);
        if ((b + 1) % 16 == 0) std::printf ("\n    ");
    }
    std::printf ("\n-----------------------------------------------------------\n");
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 3) { std::printf ("usage: Render <in.wav> <outdir> [opts]\n"); return 2; }

    juce::StringArray args;
    for (int i = 0; i < argc; ++i) args.add (argv[i]);

    auto opt = [&] (const char* name, double def)
    {
        const int i = args.indexOf (name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1].getDoubleValue() : def;
    };

    const juce::File inFile { juce::String (argv[1]) };
    const juce::File outDir { juce::String (argv[2]) };
    outDir.createDirectory();

    const double listenAt  = opt ("--listen-at", 17.0);
    const double tempo     = opt ("--tempo", 150.0);
    const int    key       = (int) opt ("--key", 0);
    const int    scale     = (int) opt ("--scale", 0);
    const double trimStart = opt ("--trim-start", 0.0);
    const int tagIdx = args.indexOf ("--tag");
    const juce::String tag = tagIdx >= 0 && tagIdx + 1 < args.size() ? args[tagIdx + 1] : "v1";

    double sr = 48000.0;
    auto dry = loadWav (inFile, sr);
    if (dry.empty()) return 1;

    std::printf ("=== SOURCE: %s ===\n", inFile.getFileName().toRawUTF8());
    std::printf ("  %.1f s @ %.0f Hz\n", dry.size() / sr, sr);
    {
        auto s = measure (dry, sr);
        std::printf ("  peak %.1f dBFS   rms %.1f dB   crest %.1f dB   LUFS %.1f   clipped samples %d\n",
                     s.peakDb, s.rmsDb, s.crestDb, s.lufs, s.clipped);
        printLevelMap (dry, sr, "source");
    }

    const float srcModulation = measureModulationDepth (dry, sr);
    std::printf ("  modulation depth %.3f  (higher = drier)\n", srcModulation);

    // ---- run the two renders ----------------------------------------------
    struct Variant { const char* name; bool cleanup; bool effects; bool deverbOnly; };
    std::vector<Variant> variants { { "cleanup", true, false, false },
                                    { "full",    true, true,  false } };
    // isolate the de-verb so its contribution is measurable on its own
    if (args.contains ("--diag"))
        variants.push_back ({ "deverbonly", true, false, true });
    if (args.contains ("--no-deverb"))
        variants[0].name = "cleanup-nodeverb";

    for (const auto& variant : variants)
    {
        ListenatorProcessor p;
        p.prepareToPlay (sr, kBlock);

        auto& st = p.getState();
        auto setP = [&] (const char* id, float v)
        { if (auto* par = st.getParameter (id)) par->setValueNotifyingHost (v); };

        setP (pid::cleanupBypass, variant.cleanup ? 0.0f : 1.0f);
        setP (pid::effectsBypass, variant.effects ? 0.0f : 1.0f);

        if (args.contains ("--no-deverb"))
            setP (pid::bpDeVerb, 1.0f);

        if (variant.deverbOnly)
            for (auto* id : { pid::bpDeClip, pid::bpPlosive, pid::bpHighPass,
                              pid::bpDeNoise, pid::bpGate, pid::bpSurgical,
                              pid::bpResonance, pid::bpComp, pid::bpDeEss,
                              pid::bpTone, pid::bpLimiter })
                setP (id, 1.0f);
        if (auto* kp = st.getParameter (pid::keyRoot))
            kp->setValueNotifyingHost (kp->convertTo0to1 ((float) key));
        if (auto* sp = st.getParameter (pid::keyScale))
            sp->setValueNotifyingHost (sp->convertTo0to1 ((float) scale));

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;

        // Worst-case meter values across the WHOLE render. Sampling the
        // processor after the last block reports whatever the tail of the file
        // happened to leave behind, which for a track ending in silence is
        // always "nothing happened".
        float worstPlosive = 0.0f, worstGr = 0.0f, worstDeEss = 0.0f;

        auto pump = [&] (size_t from, size_t to, std::vector<float>* outL,
                         std::vector<float>* outR)
        {
            for (size_t i = from; i + kBlock <= to && i + kBlock <= dry.size(); i += kBlock)
            {
                buf.clear();
                for (int k = 0; k < kBlock; ++k)
                {
                    buf.setSample (0, k, dry[i + (size_t) k]);
                    buf.setSample (1, k, dry[i + (size_t) k]);
                }
                p.processBlock (buf, midi);
                worstPlosive = std::min (worstPlosive, p.getPlosiveReductionDb());
                worstGr      = std::min (worstGr,      p.getGainReductionDb());
                worstDeEss   = std::min (worstDeEss,   p.getDeEssReductionDb());
                if (outL != nullptr)
                    for (int k = 0; k < kBlock; ++k)
                    {
                        outL->push_back (buf.getSample (0, k));
                        outR->push_back (buf.getSample (1, k));
                    }
            }
        };

        // LISTEN over a representative stretch of actual singing
        p.triggerListen();
        const size_t listenFrom = (size_t) (listenAt * sr);
        pump (listenFrom, listenFrom + (size_t) (VocalAnalyzer::captureSeconds * sr + kBlock),
              nullptr, nullptr);

        for (int t = 0; t < 200 && ! p.hasAnalysis(); ++t)
        {
            juce::Thread::sleep (25);
            buf.clear();
            p.processBlock (buf, midi);
        }

        if (! p.hasAnalysis()) { std::printf ("ANALYSIS FAILED\n"); return 1; }

        if (variant.cleanup && ! variant.effects)
            dumpAnalysis (p.getAnalysisResult());

        // full render from the top, with the chain already configured
        const int latency = p.getLatencySamples();
        std::vector<float> outL, outR;
        outL.reserve (dry.size()); outR.reserve (dry.size());
        pump (0, dry.size(), &outL, &outR);

        // drop the reported latency so the render time-aligns with the source
        if (latency > 0 && (int) outL.size() > latency)
        {
            outL.erase (outL.begin(), outL.begin() + latency);
            outR.erase (outR.begin(), outR.begin() + latency);
        }

        if (trimStart > 0.0)
        {
            const size_t cut = std::min ((size_t) (trimStart * sr), outL.size());
            outL.erase (outL.begin(), outL.begin() + (long) cut);
            outR.erase (outR.begin(), outR.begin() + (long) cut);
        }

        const float outTail = measureTailDb (outL, sr, p.getAnalysisResult().noiseFloorDb);
        const float outMod  = measureModulationDepth (outL, sr);
        std::printf ("  tail %.1f dB (src %.1f)   modulation depth %.3f (src %.3f)\n",
                     outTail, p.getAnalysisResult().tailDb, outMod, srcModulation);
        auto s = measure (outL, sr);
        std::printf ("  declipped %d samples   plosive guard max %.1f dB (LF transient ratio %.1f)\n",
                     p.getDeclippedCount(), worstPlosive, p.getPlosivePeakBoost());
        std::printf ("  compressor max %.1f dB   de-esser max %.1f dB\n", worstGr, worstDeEss);
        std::printf ("[%s] latency %d  ->  peak %.1f dBFS  rms %.1f  crest %.1f  LUFS %.1f  clipped %d\n",
                     variant.name, latency, s.peakDb, s.rmsDb, s.crestDb, s.lufs, s.clipped);
        printLevelMap (outL, sr, variant.name);

        writeWav (outDir.getChildFile (juce::String ("listenator-") + tag + "-"
                                       + variant.name + ".wav"),
                  outL, outR, sr);
        std::printf ("  wrote listenator-%s-%s.wav\n", tag.toRawUTF8(), variant.name);
    }

    return 0;
}
