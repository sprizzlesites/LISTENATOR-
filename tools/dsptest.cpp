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

void testToneMatchCurveIsApplied()
{
    std::printf ("\n[tone match] applied EQ must track the intended curve\n");

    // A deliberately dull source: the tone stage should lift its top end.
    ListenatorProcessor p;
    p.prepareToPlay (kSr, kBlock);
    doListen (p, makeVocal (16.0));
    const auto intended = p.getAnalysisResult().toneMatchDb;

    // Isolate the tone stage: everything else off.
    auto& st = p.getState();
    for (auto* id : { pid::bpHighPass, pid::bpDeNoise, pid::bpDeVerb, pid::bpGate,
                      pid::bpSurgical, pid::bpResonance, pid::bpDeEss, pid::bpComp,
                      pid::bpLimiter })
        st.getParameter (id)->setValueNotifyingHost (1.0f);
    st.getParameter (pid::bpTone)->setValueNotifyingHost (0.0f);
    st.getParameter (pid::effectsBypass)->setValueNotifyingHost (1.0f);

    // White noise in, so the measured difference IS the filter response.
    juce::Random rng (11);
    std::vector<float> noise ((size_t) (kSr * 12.0));
    for (auto& v : noise) v = (rng.nextFloat() - 0.5f) * 0.4f;

    auto out = runThrough (p, noise);
    const size_t skip = (size_t) kSr;      // let filters settle
    std::vector<float> inTail (noise.begin() + (long) skip, noise.end());
    std::vector<float> outTail (out.begin() + (long) skip, out.end());

    auto sIn  = spectrumDb (inTail);
    auto sOut = spectrumDb (outTail);

    // Compare at each 1/3-octave centre against what the analysis asked for.
    float worstErr = 0.0f;
    juce::String worstAt;
    int compared = 0;

    for (int b = 0; b < numToneBands; ++b)
    {
        const float f = toneBandHz[(size_t) b];
        if (f < 100.0f || f > 12000.0f) continue;

        // Average across a 1/6-octave window in LOG space: a fixed +/-2 bins is
        // far too narrow at 200 Hz and far too wide at 10 kHz.
        const int fftLen = 1 << 14;
        const int b0 = (int) std::round (f / std::pow (2.0f, 1.0f / 12.0f) * fftLen / kSr);
        const int b1 = (int) std::round (f * std::pow (2.0f, 1.0f / 12.0f) * fftLen / kSr);
        if (b0 < 2 || b1 >= (int) sIn.size() - 1 || b1 <= b0) continue;

        float di = 0.0f, doo = 0.0f; int cnt = 0;
        for (int k = b0; k <= b1; ++k)
        { di += sIn[(size_t) k]; doo += sOut[(size_t) k]; ++cnt; }
        const float measured = (doo - di) / (float) cnt;
        const float want = intended[(size_t) b];

        const float err = std::abs (measured - want);
        if (err > worstErr) { worstErr = err; worstAt = juce::String ((int) f) + " Hz"; }
        ++compared;
    }

    check (compared > 10, "enough bands compared", juce::String (compared));

    // The old Q=1.6 stacking made this error many dB. Narrow filters at the
    // correct 1/3-octave Q should track the intended curve closely.
    check (worstErr < 3.0f, "applied curve tracks intended within 3 dB",
           juce::String::formatted ("worst %.2f dB at %s", worstErr, worstAt.toRawUTF8()));
}

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
    testToneMatchCurveIsApplied();
    testCompressorReducesDynamicRange();
    testDeEsserActsOnSibilanceOnly();
    testTrimKnobsHaveEffect();
    testHalfBypassesAreIndependent();
    testNoRunawayOrNaN();

    std::printf ("\n===========================\n");
    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
