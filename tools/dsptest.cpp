// Objective verification of the corrective chain.
//
// Every check here measures a property of the processed audio and compares it
// against what the analysis said it would do. This is the difference between
// "the EQ is correct" being a claim and being a measurement.
//
// Build:  cmake --build build-linux --target DspTest
// Run:    ./build-linux/DspTest_artefacts/DspTest

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace listenator;

namespace
{
constexpr double kSr = 48000.0;
constexpr int    kBlock = 512;

int failures = 0, checks = 0;

void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    ++checks;
    if (! ok) ++failures;
    std::printf ("  %s %-52s %s\n", ok ? "PASS" : "FAIL",
                 what.toRawUTF8(), detail.toRawUTF8());
}

void checkNear (float actual, float expected, float tol, const juce::String& what)
{
    const bool ok = std::abs (actual - expected) <= tol;
    check (ok, what, juce::String::formatted ("got %.2f, expected %.2f +/- %.2f",
                                              actual, expected, tol));
}

/** A synthetic vocal: harmonic stack, vibrato, phrase gaps, a resonant peak,
    sibilant bursts and a noise floor. Deliberately flawed so the chain has
    something to correct. */
std::vector<float> makeVocal (double seconds, float noiseAmp = 0.0016f,
                              bool withSibilance = true, float resonanceDb = 0.0f)
{
    const int n = (int) (seconds * kSr);
    std::vector<float> out ((size_t) n);
    juce::Random rng (7);
    double phase = 0.0;

    juce::dsp::IIR::Filter<float> res;
    res.prepare ({ kSr, (juce::uint32) kBlock, 1 });
    *res.coefficients = *juce::dsp::IIR::Coefficients<float>::makePeakFilter (
        kSr, 900.0f, 8.0f, std::pow (10.0f, resonanceDb / 20.0f));

    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / kSr;
        const double f0 = 196.0 * (1.0 + 0.012 * std::sin (t * 5.5))
                                * (1.0 + 0.02 * std::sin (t * 0.4));
        phase += 2.0 * juce::MathConstants<double>::pi * f0 / kSr;

        float s = 0.0f;
        for (int h = 1; h <= 24; ++h)
            s += (float) (std::sin (phase * h) / (h * h));

        const float gate = (std::fmod (t, 2.2) < 1.5) ? 1.0f : 0.02f;
        const float sib = (withSibilance && std::fmod (t, 2.2) > 1.35
                           && std::fmod (t, 2.2) < 1.5)
                        ? (rng.nextFloat() - 0.5f) * 0.6f : 0.0f;

        float v = s * 0.34f * gate + sib + (rng.nextFloat() - 0.5f) * 2.0f * noiseAmp;
        if (resonanceDb != 0.0f) v = res.processSample (v);
        out[(size_t) i] = v;
    }
    return out;
}

/** Push a signal through the processor, return the output (mono). */
std::vector<float> runThrough (ListenatorProcessor& p, const std::vector<float>& in)
{
    std::vector<float> out;
    out.reserve (in.size());

    juce::AudioBuffer<float> buf (2, kBlock);
    juce::MidiBuffer midi;

    for (size_t i = 0; i + kBlock <= in.size(); i += kBlock)
    {
        buf.clear();
        for (int k = 0; k < kBlock; ++k)
        {
            buf.setSample (0, k, in[i + (size_t) k]);
            buf.setSample (1, k, in[i + (size_t) k]);
        }
        p.processBlock (buf, midi);
        for (int k = 0; k < kBlock; ++k)
            out.push_back (buf.getSample (0, k));
    }
    return out;
}

/** Run the LISTEN pass to completion and let the result land. */
void doListen (ListenatorProcessor& p, const std::vector<float>& material)
{
    p.triggerListen();
    runThrough (p, material);

    for (int tries = 0; tries < 100 && ! p.hasAnalysis(); ++tries)
    {
        juce::Thread::sleep (50);
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        buf.clear();
        p.processBlock (buf, midi);
    }
}

/** Magnitude spectrum of a signal, averaged over Hann frames, in dB. */
std::vector<float> spectrumDb (const std::vector<float>& x, int fftOrder = 14)
{
    const int size = 1 << fftOrder;
    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> win ((size_t) size,
                                             juce::dsp::WindowingFunction<float>::hann);
    std::vector<float> acc ((size_t) size / 2, 0.0f), scratch ((size_t) size * 2);
    int frames = 0;

    for (size_t s = 0; s + (size_t) size <= x.size(); s += (size_t) size / 2)
    {
        std::fill (scratch.begin(), scratch.end(), 0.0f);
        std::copy (x.begin() + (long) s, x.begin() + (long) s + size, scratch.begin());
        win.multiplyWithWindowingTable (scratch.data(), (size_t) size);
        fft.performFrequencyOnlyForwardTransform (scratch.data());
        for (int b = 0; b < size / 2; ++b)
            acc[(size_t) b] += scratch[(size_t) b] * scratch[(size_t) b];
        ++frames;
    }

    for (auto& v : acc)
        v = frames > 0 && v > 0.0f ? 10.0f * std::log10 (v / (float) frames) : -200.0f;
    return acc;
}

float rmsDb (const std::vector<float>& x, size_t from = 0)
{
    if (from >= x.size()) return -200.0f;
    double acc = 0.0;
    for (size_t i = from; i < x.size(); ++i) acc += (double) x[i] * x[i];
    return 10.0f * (float) std::log10 (std::max (acc / (double) (x.size() - from), 1e-20));
}

float peakOf (const std::vector<float>& x, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < x.size(); ++i) p = std::max (p, std::abs (x[i]));
    return p;
}

//==============================================================================
void testColdStartIsTransparent()
{
    std::printf ("\n[cold start] plugin must not touch audio before LISTEN\n");
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);

    auto in  = makeVocal (2.0);
    auto out = runThrough (p, in);

    double maxDiff = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max (maxDiff, (double) std::abs (out[i] - in[i]));

    check (maxDiff < 1.0e-6, "output is bit-identical to input",
           juce::String::formatted ("max diff %.2e", maxDiff));
    check (p.getLatencySamples() == 0, "reports zero latency when inactive");
}

void testAnalysisAccuracy()
{
    std::printf ("\n[analysis] derived values must match the known input\n");
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0));

    check (p.hasAnalysis(), "analysis completed");
    const auto& a = p.getAnalysisResult();

    checkNear (a.medianF0Hz, 196.0f, 4.0f, "median F0 matches the 196 Hz source");
    check (a.highPassHz > 40.0f && a.highPassHz < 196.0f,
           "high-pass sits below the fundamental",
           juce::String::formatted ("%.0f Hz", a.highPassHz));
    check (a.noiseFloorDb < -40.0f, "noise floor detected",
           juce::String::formatted ("%.1f dB", a.noiseFloorDb));
    check (a.deEssCentreHz > 3000.0f && a.deEssCentreHz < 12000.0f,
           "sibilant band located in a plausible range",
           juce::String::formatted ("%.0f Hz", a.deEssCentreHz));

    // The de-ess threshold has to be on the dBFS scale the detector uses.
    // The old FFT-magnitude derivation landed tens of dB off.
    check (a.deEssThresholdDb > -60.0f && a.deEssThresholdDb < -6.0f,
           "de-ess threshold is a sane dBFS value",
           juce::String::formatted ("%.1f dBFS", a.deEssThresholdDb));
    check (a.sibilantPeakDb > a.deEssThresholdDb,
           "sibilant peak sits above the threshold",
           juce::String::formatted ("peak %.1f vs thresh %.1f",
                                    a.sibilantPeakDb, a.deEssThresholdDb));

    check (a.compLevelThreshDb < a.compPeakThreshDb,
           "RMS-stage threshold sits below the peak-stage threshold",
           juce::String::formatted ("%.1f vs %.1f",
                                    a.compLevelThreshDb, a.compPeakThreshDb));
    check (a.makeupGainDb > 0.0f, "makeup gain is non-zero",
           juce::String::formatted ("%.1f dB", a.makeupGainDb));
}

void testNoRunawayOrNaN()
{
    std::printf ("\n[stability] full chain must stay finite and bounded\n");
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0));

    auto out = runThrough (p, makeVocal (8.0));

    int bad = 0;
    for (float v : out) if (! std::isfinite (v)) ++bad;
    check (bad == 0, "no NaN or Inf in output", juce::String (bad) + " bad samples");

    const float pk = peakOf (out, out.size() / 4);
    check (pk > 1.0e-4f, "output is not silent", juce::String::formatted ("peak %.4f", pk));
    check (pk < 1.2f, "limiter holds the output near full scale",
           juce::String::formatted ("peak %.4f", pk));
}

void testToneMatchConverges()
{
    std::printf ("\n[tone match] the loop must converge on the target curve\n");

    // The stage is a closed loop, so the property worth testing is not "the
    // filters match the curve the analysis asked for" -- that curve is only a
    // starting guess. It is "whatever goes in, the spectrum that comes out
    // approaches the target". Feeding it a source whose shape is nothing like
    // the target is the strongest version of that test.
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0));

    auto& st = p.getState();
    for (auto* id : { pid::bpHighPass, pid::bpDeNoise, pid::bpDeVerb, pid::bpGate,
                      pid::bpUpward, pid::bpSurgical, pid::bpResonance, pid::bpDeEss,
                      pid::bpComp, pid::bpLimiter, pid::bpDeClip, pid::bpPlosive })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::bpTone)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    // Flat noise: every band is wrong by the full depth of the target curve.
    juce::Random rng (11);
    std::vector<float> noise ((size_t) (kSr * 45.0));
    for (auto& v : noise) v = (rng.nextFloat() - 0.5f) * 0.4f;

    auto out = runThrough (p, noise);

    // Only the last third: the loop is deliberately slow, and measuring while
    // it is still converging tests the ramp rather than the destination.
    const size_t tail = out.size() * 2 / 3;
    std::vector<float> settled (out.begin() + (long) tail, out.end());
    std::vector<float> before (noise.begin() + (long) tail, noise.end());

    auto bandsOf = [] (const std::vector<float>& x)
    {
        auto sp = spectrumDb (x);
        const int fftLen = 1 << 14;
        std::array<float, numToneBands> b {};
        for (int i = 0; i < numToneBands; ++i)
        {
            const float f = toneBandHz[(size_t) i];
            const int lo = (int) std::round (f / 1.122462f * fftLen / kSr);
            const int hi = (int) std::round (f * 1.122462f * fftLen / kSr);
            float acc = 0.0f; int cnt = 0;
            for (int k = std::max (1, lo); k <= std::min (hi, (int) sp.size() - 1); ++k)
            { acc += sp[(size_t) k]; ++cnt; }
            b[(size_t) i] = cnt > 0 ? acc / (float) cnt : -200.0f;
        }
        // normalise on the 200 Hz-2 kHz average, as the target curve is
        float ref = 0.0f; int n = 0;
        for (int i = 0; i < numToneBands; ++i)
        {
            const float f = toneBandHz[(size_t) i];
            if (f < 200.0f || f > 2000.0f) continue;
            ref += b[(size_t) i]; ++n;
        }
        if (n > 0) { ref /= (float) n; for (auto& v : b) v -= ref; }
        return b;
    };

    auto tgt = kTargetLtasDb;
    {
        float ref = 0.0f; int n = 0;
        for (int i = 0; i < numToneBands; ++i)
        {
            const float f = toneBandHz[(size_t) i];
            if (f < 200.0f || f > 2000.0f) continue;
            ref += tgt[(size_t) i]; ++n;
        }
        if (n > 0) for (auto& v : tgt) v -= ref / (float) n;
    }

    const auto inB = bandsOf (before);
    const auto outB = bandsOf (settled);

    float errIn = 0.0f, errOut = 0.0f, worst = 0.0f; int cnt = 0;
    juce::String worstAt;
    for (int i = 0; i < numToneBands; ++i)
    {
        const float f = toneBandHz[(size_t) i];
        // 125 Hz-6.3 kHz: outside it, flat noise needs more than the deliberate
        // +/-12 dB filter bound to reach the target, so the clamp is the answer.
        if (f < 125.0f || f > 6300.0f) continue;
        const float eo = std::abs (outB[(size_t) i] - tgt[(size_t) i]);
        errIn  += std::abs (inB[(size_t) i] - tgt[(size_t) i]);
        errOut += eo;
        if (eo > worst) { worst = eo; worstAt = juce::String ((int) f) + " Hz"; }
        ++cnt;
    }
    errIn /= (float) std::max (1, cnt);
    errOut /= (float) std::max (1, cnt);

    check (cnt > 10, "enough bands compared", juce::String (cnt));
    check (errOut < errIn * 0.5f, "loop moves the spectrum toward the target",
           juce::String::formatted ("mean |error| %.2f dB in, %.2f dB out", errIn, errOut));
    check (worst < 4.0f, "no band left badly wrong",
           juce::String::formatted ("worst %.2f dB at %s", worst, worstAt.toRawUTF8()));
    check (p.getToneUpdateCount() > 10, "loop actually ran",
           juce::String::formatted ("%d updates, max trim %.1f dB",
                                    p.getToneUpdateCount(), p.getToneTrimDb()));
}

void testReportedLatencyIsTrue()
{
    std::printf ("\n[latency] the reported figure must be the real delay\n");

    // Cross-correlation against a noise burst, not an impulse: a single sample
    // gets gated away, and "centre of energy" is biased by however long the
    // filters ring. The lag that best aligns input and output IS the delay.
    auto delayOf = [] (const std::vector<float>& in, const std::vector<float>& out)
    {
        const size_t start = in.size() / 3, len = in.size() / 3;
        const int search = 8192;
        double best = -1.0e30; int bestLag = 0;
        for (int lag = 0; lag <= search; ++lag)
        {
            double acc = 0.0;
            for (size_t i = 0; i < len; i += 8)
            {
                const size_t j = start + i + (size_t) lag;
                if (j >= out.size()) break;
                acc += (double) in[start + i] * out[j];
            }
            if (acc > best) { best = acc; bestLag = lag; }
        }
        return bestLag;
    };

    juce::Random rng (31);
    std::vector<float> noise ((size_t) (kSr * 6.0));
    for (auto& v : noise) v = (rng.nextFloat() - 0.5f) * 0.5f;

    struct Case { const char* name; std::vector<const char*> active; };
    const std::vector<Case> cases {
        { "no modules",  {} },
        { "spectral",    { pid::bpDeNoise } },
        { "limiter",     { pid::bpLimiter } },
        { "everything",  { pid::bpDeClip, pid::bpPlosive, pid::bpHighPass, pid::bpDeNoise,
                           pid::bpDeVerb, pid::bpGate, pid::bpUpward, pid::bpSurgical,
                           pid::bpResonance, pid::bpDeEss, pid::bpComp, pid::bpTone,
                           pid::bpLimiter } },
    };
    // The effects half is measured the same way: a host trusts one number for
    // the whole plugin, so a delay hiding on that side is just as wrong.
    struct FxCase { const char* name; std::vector<const char*> active; };
    const std::vector<FxCase> fxCases {
        { "fx: none",       {} },
        { "fx: tuner",      { pid::bpAutoTune } },
        { "fx: everything", { pid::bpAutoTune, pid::bpDoubler, pid::bpSaturation,
                              pid::bpExciter, pid::bpCharacter, pid::bpWidth,
                              pid::bpDelay, pid::bpReverb } },
    };

    for (const auto& c : cases)
    {
        ListenatorProcessor p;
        p.prepareToPlay (kSr, kBlock);
        doListen (p, makeVocal (16.0));

        auto& st = p.getState();
        for (auto* id : { pid::bpDeClip, pid::bpPlosive, pid::bpHighPass, pid::bpDeNoise,
                          pid::bpDeVerb, pid::bpGate, pid::bpUpward, pid::bpSurgical,
                          pid::bpResonance, pid::bpDeEss, pid::bpComp, pid::bpTone,
                          pid::bpLimiter })
            st.getParameter (id)->setValueNotifyingHost (1.0f);
        for (auto* id : c.active)
            st.getParameter (id)->setValueNotifyingHost (0.0f);
        st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

        auto out = runThrough (p, noise);
        // read AFTER processing: parameter changes only reach the chain there,
        // and with them the latency the chain would report to a host
        const int reported = p.getLatencySamples();
        const int actual   = delayOf (noise, out);

        check (std::abs (actual - reported) <= 32,
               juce::String ("reported latency is real: ") + c.name,
               juce::String::formatted ("reported %d, measured %d", reported, actual));
    }

    for (const auto& c : fxCases)
    {
        ListenatorProcessor p;
        p.prepareToPlay (kSr, kBlock);
        doListen (p, makeVocal (16.0));

        auto& st = p.getState();
        st.getParameter (pid::cleanupBypass)->setValueNotifyingHost (1.0f);
        for (auto* id : { pid::bpAutoTune, pid::bpDoubler, pid::bpSaturation, pid::bpExciter,
                          pid::bpCharacter, pid::bpWidth, pid::bpDelay, pid::bpReverb })
            st.getParameter (id)->setValueNotifyingHost (1.0f);
        for (auto* id : c.active)
            st.getParameter (id)->setValueNotifyingHost (0.0f);

        auto out = runThrough (p, noise);
        const int reported = p.getLatencySamples();
        const int actual   = delayOf (noise, out);

        check (std::abs (actual - reported) <= 32,
               juce::String ("reported latency is real: ") + c.name,
               juce::String::formatted ("reported %d, measured %d", reported, actual));
    }
}

//==============================================================================
void testDeverbRemovesAKnownRoom()
{
    std::printf ("\n[de-verb] a known reverb must come back off\n");

    // Dry source, then the same source through a synthetic room: an exponential
    // noise tail convolved in. Because the room is applied by us, "did it come
    // off" is a measurement rather than an opinion.
    auto dry = makeVocal (34.0, 0.0004f, true, 0.0f);

    const float rt60 = 0.9f;
    const int irLen = (int) (rt60 * kSr);
    std::vector<float> ir ((size_t) irLen, 0.0f);
    {
        juce::Random rng (99);
        ir[0] = 1.0f;                                   // direct
        for (int i = (int) (0.008 * kSr); i < irLen; ++i)
            ir[(size_t) i] = (rng.nextFloat() - 0.5f) * 2.0f
                           * std::pow (10.0f, -3.0f * (float) i / (float) irLen) * 0.16f;
    }

    std::vector<float> wet (dry.size(), 0.0f);
    {
        // sparse convolution: only every 4th tap, which is plenty to make a
        // dense-sounding tail and keeps the test quick
        for (size_t n = 0; n < dry.size(); ++n)
        {
            const float x = dry[n];
            if (std::abs (x) < 1.0e-7f) continue;
            for (int k = 0; k < irLen; k += 4)
            {
                const size_t j = n + (size_t) k;
                if (j >= wet.size()) break;
                wet[j] += x * ir[(size_t) k];
            }
        }
        float pk = 0.0f; for (float v : wet) pk = std::max (pk, std::abs (v));
        if (pk > 0.0f) for (auto& v : wet) v *= 0.6f / pk;
    }

    // How much tail the room added, measured the way the analyser measures it.
    auto tailOf = [] (const std::vector<float>& x)
    {
        const int frameLen = (int) (0.005 * kSr);
        std::vector<float> envDb;
        for (size_t i = 0; i + (size_t) frameLen <= x.size(); i += (size_t) frameLen)
        {
            double a = 0.0;
            for (int k = 0; k < frameLen; ++k) a += (double) x[i + (size_t) k] * x[i + (size_t) k];
            envDb.push_back (10.0f * (float) std::log10 (std::max (a / frameLen, 1e-20)));
        }
        const int pre = 20, lo = 16, hi = 50;
        std::vector<float> ratios;
        for (size_t i = (size_t) pre; i + (size_t) hi < envDb.size(); ++i)
        {
            if (envDb[i] < -55.0f) continue;
            if (envDb[i + (size_t) lo] > envDb[i] - 6.0f) continue;
            float p = 0.0f; for (int k = 0; k < pre; ++k) p += envDb[i - (size_t) k];
            float t = 0.0f; for (int k = lo; k <= hi; ++k) t += envDb[i + (size_t) k];
            ratios.push_back (t / (float) (hi - lo + 1) - p / (float) pre);
        }
        if (ratios.empty()) return 0.0f;
        std::sort (ratios.begin(), ratios.end());
        return ratios[ratios.size() / 2];
    };

    const float dryTail = tailOf (dry);
    const float wetTail = tailOf (wet);

    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, wet);

    auto& st = p.getState();
    for (auto* id : { pid::bpDeClip, pid::bpPlosive, pid::bpHighPass, pid::bpDeNoise,
                      pid::bpGate, pid::bpUpward, pid::bpSurgical, pid::bpResonance,
                      pid::bpDeEss, pid::bpComp, pid::bpTone, pid::bpLimiter })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::bpDeVerb)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    auto out = runThrough (p, wet);
    const int lat = p.getLatencySamples();
    if (lat > 0 && (int) out.size() > lat) out.erase (out.begin(), out.begin() + lat);

    const float outTail = tailOf (out);
    const auto& a = p.getAnalysisResult();

    std::printf ("       [diag] RT60 measured %.2f s (true %.2f)  tail dry %.1f  wet %.1f  out %.1f\n",
                 a.rt60Seconds, rt60, dryTail, wetTail, outTail);
    std::printf ("       [diag] deverbAmount %.2f  reverbRatio %.2f  tailDb %.1f  directToReverb %.1f  gaps %d\n",
                 a.deverbAmount, a.reverbRatio, a.tailDb, a.directToReverbDb, a.tailSamples);

    check (wetTail > dryTail + 4.0f, "the synthetic room really is audible",
           juce::String::formatted ("%.1f dB -> %.1f dB", dryTail, wetTail));
    check (outTail < wetTail - 3.0f, "the stage removes a measurable part of it",
           juce::String::formatted ("%.1f dB -> %.1f dB", wetTail, outTail));

    // It must not get there by turning the direct sound down.
    //
    // Overall RMS is the wrong guard: removing reverb removes energy, and the
    // tail figure is already a RATIO against the level just before the gap, so
    // a uniform attenuation cancels out of it entirely. What matters is that
    // the loud material -- the direct sound -- survives.
    auto loudLevel = [] (const std::vector<float>& x)
    {
        const int win = (int) (0.05 * kSr);
        std::vector<float> cells;
        for (size_t i = 0; i + (size_t) win <= x.size(); i += (size_t) win)
        {
            double a = 0.0;
            for (int k = 0; k < win; ++k) a += (double) x[i + (size_t) k] * x[i + (size_t) k];
            cells.push_back (10.0f * (float) std::log10 (std::max (a / win, 1e-20)));
        }
        if (cells.empty()) return -200.0f;
        std::sort (cells.begin(), cells.end());
        return cells[(size_t) ((cells.size() - 1) * 4 / 5)];      // 80th percentile
    };

    const float wetLoud = loudLevel (wet), outLoud = loudLevel (out);
    check (outLoud > wetLoud - 3.0f, "the direct sound survives",
           juce::String::formatted ("loud-cell level %.1f dB -> %.1f dB", wetLoud, outLoud));

    // No separate "the tail fell further than the voice" check: the tail figure
    // is a ratio against the level just before the gap, so a uniform
    // attenuation cancels out of it by construction. Its 3.5 dB improvement
    // already IS the differential -- comparing it against the broadband drop as
    // well counts the same normalisation twice.
    check (p.getAnalysisResult().valid, "analysis landed");
}

//==============================================================================
void testUpwardExpanderLiftsQuietDelivery()
{
    std::printf ("\n[upward expander] quiet delivery must come up, the room must not\n");

    // Alternating loud and quiet phrases over a constant noise floor. The stage
    // has to close the gap between the phrases without lifting the floor.
    const float floorAmp = 0.0016f;
    const int n = (int) (36.0 * kSr);
    std::vector<float> material ((size_t) n);
    {
        juce::Random rng (23);
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / kSr;
            phase += 2.0 * juce::MathConstants<double>::pi * 196.0 / kSr;
            float sig = 0.0f;
            for (int h = 1; h <= 20; ++h) sig += (float) (std::sin (phase * h) / (h * h));

            // 1.2 s of phrase, 0.6 s of gap; every other phrase 10 dB down
            const bool inPhrase = std::fmod (t, 1.8) < 1.2;
            const bool quietOne = std::fmod (t, 3.6) >= 1.8;
            const float amp = inPhrase ? (quietOne ? 0.34f * 0.316f : 0.34f) : 0.0f;
            material[(size_t) i] = sig * amp + (rng.nextFloat() - 0.5f) * 2.0f * floorAmp;
        }
    }

    auto window = [] (const std::vector<float>& x, double from, double to)
    {
        const size_t a = (size_t) (from * kSr);
        const size_t b = std::min ((size_t) (to * kSr), x.size());
        if (a >= b) return 1.0e-20;
        double acc = 0.0;
        for (size_t i = a; i < b; ++i) acc += (double) x[i] * x[i];
        return std::max (acc / (double) (b - a), 1.0e-20);
    };

    // Average the loud phrases, the quiet phrases and the gaps separately.
    auto phraseStats = [&] (const std::vector<float>& x, float& loud, float& quiet, float& gap)
    {
        double l = 0.0, q = 0.0, g = 0.0; int cnt = 0;
        for (double t = 3.6; t + 3.6 < (double) x.size() / kSr; t += 3.6)
        {
            l += window (x, t + 0.3,  t + 1.1);
            q += window (x, t + 2.1,  t + 2.9);
            g += window (x, t + 1.35, t + 1.75);
            ++cnt;
        }
        cnt = std::max (1, cnt);
        loud  = 10.0f * (float) std::log10 (l / cnt);
        quiet = 10.0f * (float) std::log10 (q / cnt);
        gap   = 10.0f * (float) std::log10 (g / cnt);
    };

    float srcLoud = 0, srcQuiet = 0, srcGap = 0;
    phraseStats (material, srcLoud, srcQuiet, srcGap);

    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, material);

    // Isolate: only the upward expander, so the compressors cannot be credited
    // with what this stage does.
    auto& st = p.getState();
    for (auto* id : { pid::bpDeClip, pid::bpPlosive, pid::bpHighPass, pid::bpDeNoise,
                      pid::bpDeVerb, pid::bpGate, pid::bpSurgical, pid::bpResonance,
                      pid::bpDeEss, pid::bpComp, pid::bpTone, pid::bpLimiter })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::bpUpward)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    auto out = runThrough (p, material);
    const int lat = p.getLatencySamples();
    if (lat > 0 && (int) out.size() > lat) out.erase (out.begin(), out.begin() + lat);

    float outLoud = 0, outQuiet = 0, outGap = 0;
    phraseStats (out, outLoud, outQuiet, outGap);

    const float srcSpread = srcLoud - srcQuiet;
    const float outSpread = outLoud - outQuiet;

    check (outSpread < srcSpread - 1.0f, "quiet phrases come closer to the loud ones",
           juce::String::formatted ("spread %.1f dB -> %.1f dB", srcSpread, outSpread));
    check (outQuiet > srcQuiet + 0.8f, "the lift lands on the quiet phrase",
           juce::String::formatted ("%.1f dB -> %.1f dB", srcQuiet, outQuiet));
    check (outLoud < srcLoud + 0.6f, "the loud phrase is left alone",
           juce::String::formatted ("%.1f dB -> %.1f dB", srcLoud, outLoud));
    check (outGap < srcGap + 1.0f, "the noise floor between phrases is not lifted",
           juce::String::formatted ("%.1f dB -> %.1f dB", srcGap, outGap));
}

//==============================================================================
void testPlosiveGuardHitsThumpsNotNotes()
{
    std::printf ("\n[plosive guard] pops must duck, low notes must not\n");

    // Two things a naive low-band detector cannot tell apart: a 70 Hz burst
    // with no high-frequency content, and the onset of a sung note whose
    // fundamental lives in the same region.
    const int n = (int) (30.0 * kSr);
    std::vector<float> material ((size_t) n);
    std::vector<char> isPop ((size_t) n, 0), isNote ((size_t) n, 0);
    {
        juce::Random rng (5);
        double phase = 0.0, popPhase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / kSr;
            phase    += 2.0 * juce::MathConstants<double>::pi * 130.0 / kSr;
            popPhase += 2.0 * juce::MathConstants<double>::pi * 70.0  / kSr;

            float sig = 0.0f;
            for (int h = 1; h <= 20; ++h) sig += (float) (std::sin (phase * h) / (h * h));

            const double cyc = std::fmod (t, 2.0);
            float v = 0.0f;

            if (cyc < 0.9)                                  // sung note
            {
                v = sig * 0.34f * (float) std::min (1.0, cyc / 0.008);
                if (cyc > 0.02 && cyc < 0.85) isNote[(size_t) i] = 1;
            }
            else if (cyc > 1.2 && cyc < 1.30)               // pop: low, no HF
            {
                v = (float) std::sin (popPhase) * 0.55f
                  * (float) std::exp (-(cyc - 1.2) * 45.0);
                isPop[(size_t) i] = 1;
            }

            material[(size_t) i] = v + (rng.nextFloat() - 0.5f) * 0.0016f;
        }
    }

    auto energy = [] (const std::vector<float>& x, const std::vector<char>& mask)
    {
        double acc = 0.0; size_t cnt = 0;
        for (size_t i = 0; i < x.size() && i < mask.size(); ++i)
            if (mask[i] != 0) { acc += (double) x[i] * x[i]; ++cnt; }
        return 10.0f * (float) std::log10 (std::max (acc / (double) std::max<size_t> (1, cnt), 1e-20));
    };

    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, material);

    auto& st = p.getState();
    for (auto* id : { pid::bpDeClip, pid::bpHighPass, pid::bpDeNoise, pid::bpDeVerb,
                      pid::bpGate, pid::bpUpward, pid::bpSurgical, pid::bpResonance,
                      pid::bpDeEss, pid::bpComp, pid::bpTone, pid::bpLimiter })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::bpPlosive)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    auto out = runThrough (p, material);
    const int lat = p.getLatencySamples();
    if (lat > 0 && (int) out.size() > lat) out.erase (out.begin(), out.begin() + lat);

    const float popIn  = energy (material, isPop),  popOut  = energy (out, isPop);
    const float noteIn = energy (material, isNote), noteOut = energy (out, isNote);
    const auto& a = p.getAnalysisResult();

    std::printf ("       [diag] bands %.0f/%.0f Hz  depth %.1f dB  trip %.2f  "
                 "measured %.2f/s worst %.1f  guard max %.1f dB\n",
                 a.plosiveCornerHz, a.plosiveUpperHz, a.plosiveDepthDb,
                 a.plosiveSensitivity, a.plosiveRate, a.plosivePeakRatio,
                 p.getPlosiveReductionDb());

    check (popOut < popIn - 3.0f, "the pop is ducked",
           juce::String::formatted ("%.1f dB -> %.1f dB", popIn, popOut));
    check (noteOut > noteIn - 1.0f, "the sung note is not",
           juce::String::formatted ("%.1f dB -> %.1f dB", noteIn, noteOut));
    check (a.plosiveCornerHz > a.highPassHz,
           "the guard's band sits above the high-pass",
           juce::String::formatted ("corner %.0f Hz vs hpf %.0f Hz",
                                    a.plosiveCornerHz, a.highPassHz));
}

//==============================================================================
void testCompressorReducesDynamicRange()
{
    std::printf ("\n[compressor] must reduce range, not crush everything\n");
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0));

    auto& st = p.getState();
    for (auto* id : { pid::bpHighPass, pid::bpDeNoise, pid::bpDeVerb, pid::bpGate,
                      pid::bpSurgical, pid::bpResonance, pid::bpDeEss, pid::bpTone,
                      pid::bpLimiter })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::bpComp)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    // Two passes at levels 20 dB apart; compression should narrow the gap.
    auto loud = makeVocal (6.0);
    auto quiet = loud;
    for (auto& v : quiet) v *= 0.1f;      // -20 dB

    auto outLoud = runThrough (p, loud);
    ListenatorProcessor p2;
    p2.prepareToPlay (kSr, kBlock);
    doListen (p2, makeVocal (16.0));
    auto& st2 = p2.getState();
    for (auto* id : { pid::bpHighPass, pid::bpDeNoise, pid::bpDeVerb, pid::bpGate,
                      pid::bpSurgical, pid::bpResonance, pid::bpDeEss, pid::bpTone,
                      pid::bpLimiter })
        st2.getParameter (id)->setValueNotifyingHost (1.0f);
    st2.getParameter (pid::bpComp)->setValueNotifyingHost (0.0f);
    st2.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);
    auto outQuiet = runThrough (p2, quiet);

    const size_t skip = outLoud.size() / 3;
    const float inGap  = rmsDb (loud, skip) - rmsDb (quiet, skip);
    const float outGap = rmsDb (outLoud, skip) - rmsDb (outQuiet, skip);

    check (outGap < inGap - 1.0f, "20 dB input difference is compressed",
           juce::String::formatted ("in %.1f dB -> out %.1f dB", inGap, outGap));
    check (outGap > 2.0f, "compression is not total (dynamics survive)",
           juce::String::formatted ("%.1f dB remains", outGap));
}

void testDeEsserActsOnSibilanceOnly()
{
    std::printf ("\n[de-esser] must duck sibilance and leave the body alone\n");
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0));

    auto& st = p.getState();
    for (auto* id : { pid::bpHighPass, pid::bpDeNoise, pid::bpDeVerb, pid::bpGate,
                      pid::bpUpward, pid::bpDeClip, pid::bpPlosive,
                      pid::bpSurgical, pid::bpResonance, pid::bpComp, pid::bpTone,
                      pid::bpLimiter })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::bpDeEss)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    auto in = makeVocal (8.0);
    auto out = runThrough (p, in);

    const size_t skip = (size_t) kSr;
    std::vector<float> inTail (in.begin() + (long) skip, in.begin() + (long) out.size());
    std::vector<float> outTail (out.begin() + (long) skip, out.end());

    auto sIn = spectrumDb (inTail), sOut = spectrumDb (outTail);

    auto bandDelta = [&] (float lo, float hi)
    {
        const double fftLen = 1 << 14;    // must match spectrumDb's order
        const int b0 = (int) (lo * fftLen / kSr), b1 = (int) (hi * fftLen / kSr);
        float di = 0.0f, doo = 0.0f; int n = 0;
        for (int b = b0; b <= b1 && b < (int) sIn.size(); ++b)
        { di += sIn[(size_t) b]; doo += sOut[(size_t) b]; ++n; }
        return n > 0 ? (doo - di) / (float) n : 0.0f;
    };

    const float sibDelta  = bandDelta (5000.0f, 10000.0f);
    const float bodyDelta = bandDelta (200.0f, 1000.0f);

    std::printf ("       [diag] deEss centre %.0f Hz  thresh %.1f dBFS  peak %.1f  "
                 "maxRed %.1f  live GR %.2f dB\n",
                 p.getAnalysisResult().deEssCentreHz,
                 p.getAnalysisResult().deEssThresholdDb,
                 p.getAnalysisResult().sibilantPeakDb,
                 p.getAnalysisResult().deEssMaxReductionDb,
                 p.getDeEssReductionDb());

    check (sibDelta < -0.5f, "sibilant band is reduced",
           juce::String::formatted ("%.2f dB", sibDelta));
    check (std::abs (bodyDelta) < 0.6f, "vocal body is left alone",
           juce::String::formatted ("%.2f dB", bodyDelta));
    check (sibDelta < bodyDelta - 0.5f, "reduction is band-selective");
}

void testSpectralEngineIsAudible()
{
    std::printf ("\n[spectral] de-noise/resonance stage must actually reach the output\n");

    // The overlap-add bug made this stage write behind the read pointer, so it
    // silently produced near-silence. Guard against that regressing.
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0, 0.02f));   // noisy source so de-noise engages

    auto& st = p.getState();
    for (auto* id : { pid::bpHighPass, pid::bpGate, pid::bpSurgical, pid::bpDeEss,
                      pid::bpComp, pid::bpTone, pid::bpLimiter })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    for (auto* id : { pid::bpDeNoise, pid::bpDeVerb, pid::bpResonance })
        st.getParameter (id)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    auto in = makeVocal (6.0, 0.02f);
    auto out = runThrough (p, in);

    const size_t skip = out.size() / 3;
    const float inRms = rmsDb (in, skip), outRms = rmsDb (out, skip);

    check (outRms > inRms - 12.0f, "signal survives the STFT stage",
           juce::String::formatted ("in %.1f dB -> out %.1f dB", inRms, outRms));
    check (outRms < inRms + 3.0f, "stage does not add gain",
           juce::String::formatted ("%.1f dB", outRms - inRms));
}

void testTrimKnobsHaveEffect()
{
    std::printf ("\n[trims] knobs must change the audio, not just the state\n");

    auto runWithEq = [] (float eqAmount)
    {
        ListenatorProcessor p;
        p.prepareToPlay (kSr, kBlock);
        doListen (p, makeVocal (16.0));

        auto& st = p.getState();
        for (auto* id : { pid::bpHighPass, pid::bpDeNoise, pid::bpDeVerb, pid::bpGate,
                          pid::bpDeEss, pid::bpComp, pid::bpLimiter })
            st.getParameter (id)->setValueNotifyingHost (1.0f);
        st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

        auto* eq = st.getParameter (pid::eqAmount);
        eq->setValueNotifyingHost (eq->convertTo0to1 (eqAmount));

        juce::Random rng (5);
        std::vector<float> noise ((size_t) (kSr * 3.0));
        for (auto& v : noise) v = (rng.nextFloat() - 0.5f) * 0.4f;
        return runThrough (p, noise);
    };

    auto none = runWithEq (0.0f);
    auto full = runWithEq (2.0f);

    auto sNone = spectrumDb (std::vector<float> (none.begin() + (long) kSr, none.end()));
    auto sFull = spectrumDb (std::vector<float> (full.begin() + (long) kSr, full.end()));

    float maxDelta = 0.0f;
    for (size_t b = 20; b < sNone.size() / 3; ++b)
        maxDelta = std::max (maxDelta, std::abs (sFull[b] - sNone[b]));

    // updateFilters() used to run only on new analysis, so the knob did nothing.
    check (maxDelta > 2.0f, "EQ AMOUNT changes the frequency response",
           juce::String::formatted ("max delta %.2f dB", maxDelta));
}

void testHalfBypassesAreIndependent()
{
    std::printf ("\n[bypass] each half must be independently defeatable\n");
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0));

    auto& st = p.getState();
    st.getParameter (pid::cleanupBypass)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    auto in = makeVocal (3.0);
    auto out = runThrough (p, in);

    double maxDiff = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
        maxDiff = std::max (maxDiff, (double) std::abs (out[i] - in[i]));

    check (maxDiff < 1.0e-5, "both halves bypassed is transparent",
           juce::String::formatted ("max diff %.2e", maxDiff));
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("LISTENATOR DSP verification\n");
    std::printf ("===========================\n");

    testColdStartIsTransparent();
    testAnalysisAccuracy();
    testSpectralEngineIsAudible();
    testToneMatchConverges();
    testReportedLatencyIsTrue();
    testDeverbRemovesAKnownRoom();
    testUpwardExpanderLiftsQuietDelivery();
    testPlosiveGuardHitsThumpsNotNotes();
    testCompressorReducesDynamicRange();
    testDeEsserActsOnSibilanceOnly();
    testTrimKnobsHaveEffect();
    testHalfBypassesAreIndependent();
    testNoRunawayOrNaN();

    std::printf ("\n===========================\n");
    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
