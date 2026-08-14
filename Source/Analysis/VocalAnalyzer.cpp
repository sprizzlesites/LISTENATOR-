#include "VocalAnalyzer.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace listenator
{

namespace
{
    constexpr float kMinDb = -120.0f;

    inline float toDb (float power) noexcept
    {
        return power > 1.0e-12f ? 10.0f * std::log10 (power) : kMinDb;
    }

    float percentile (std::vector<float> v, float p)
    {
        if (v.empty()) return 0.0f;
        const size_t idx = (size_t) std::clamp ((int) (p * (float) (v.size() - 1)),
                                                0, (int) v.size() - 1);
        std::nth_element (v.begin(), v.begin() + (long) idx, v.end());
        return v[idx];
    }

    /** Target long-term average spectrum for a professionally mixed lead vocal,
        in dB relative to the 200 Hz-2 kHz average. Derived from the spectral
        balance conventions the mixing literature agrees on: steep sub rolloff,
        controlled low-mid body, a presence region that sits above the natural
        rolloff, and a gently declining air shelf.

        This is the single universal curve. `toneMatchDb` is the difference
        between this and what actually came in. */
    constexpr std::array<float, numToneBands> kTargetLtasDb {
        -42.0f, -38.0f, -34.0f, -29.0f, -24.0f, -18.0f, -12.0f,  -7.0f,  // 20..100
         -3.5f,  -1.5f,  -0.5f,   0.0f,   0.3f,   0.3f,   0.0f,  -0.8f,  // 125..630
         -1.8f,  -2.8f,  -3.8f,  -4.8f,  -5.4f,  -5.8f,  -6.0f,  -6.6f,  // 800..4k
         -8.5f, -10.5f, -13.0f, -16.0f, -19.5f, -24.0f, -32.0f           // 5k..20k
    };

    /** RBJ biquad, used for the K-weighting pre-filter. */
    struct Biquad
    {
        double b0=1, b1=0, b2=0, a1=0, a2=0, z1=0, z2=0;

        void highShelf (double fs, double f0, double gainDb, double q)
        {
            const double A = std::pow (10.0, gainDb / 40.0);
            const double w = 2.0 * juce::MathConstants<double>::pi * f0 / fs;
            const double cw = std::cos (w), sw = std::sin (w);
            const double alpha = sw / (2.0 * q);
            const double sq = 2.0 * std::sqrt (A) * alpha;
            const double a0 = (A + 1.0) - (A - 1.0) * cw + sq;

            b0 =  A * ((A + 1.0) + (A - 1.0) * cw + sq) / a0;
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0;
            b2 =  A * ((A + 1.0) + (A - 1.0) * cw - sq) / a0;
            a1 =  2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0;
            a2 = ((A + 1.0) - (A - 1.0) * cw - sq) / a0;
        }

        void highPass (double fs, double f0, double q)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * f0 / fs;
            const double cw = std::cos (w), sw = std::sin (w);
            const double alpha = sw / (2.0 * q);
            const double a0 = 1.0 + alpha;

            b0 =  (1.0 + cw) / 2.0 / a0;
            b1 = -(1.0 + cw) / a0;
            b2 =  (1.0 + cw) / 2.0 / a0;
            a1 = -2.0 * cw / a0;
            a2 =  (1.0 - alpha) / a0;
        }

        inline double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
}

//==============================================================================
VocalAnalyzer::VocalAnalyzer() : juce::Thread ("LISTENATOR analysis")
{
    // Thread is a private base, so the owner can't start it: do it here.
    startThread (juce::Thread::Priority::low);
}

VocalAnalyzer::~VocalAnalyzer()
{
    stopThread (2000);
}

void VocalAnalyzer::prepare (double sampleRate, int)
{
    sr = sampleRate;
    captureLen = (int) (captureSeconds * sr);
    capture.assign ((size_t) captureLen, 0.0f);
    fftScratch.assign ((size_t) fftSize * 2, 0.0f);
    avgPower.assign ((size_t) fftSize / 2, 0.0f);
    sibilantAvgPower.assign ((size_t) fftSize / 2, 0.0f);
    noisePower.assign ((size_t) fftSize / 2, 0.0f);
    pitchTracker.prepare (sampleRate, 2048);
    reset();
}

void VocalAnalyzer::reset()
{
    writePos.store (0, std::memory_order_release);
    listening.store (false, std::memory_order_release);
}

void VocalAnalyzer::startListening (double tempoBpm, bool tempoValid)
{
    if (analysing.load (std::memory_order_acquire))
        return;

    pendingTempo = tempoBpm;
    pendingTempoValid = tempoValid;
    writePos.store (0, std::memory_order_release);
    listening.store (true, std::memory_order_release);
}

float VocalAnalyzer::getProgress() const noexcept
{
    if (captureLen <= 0) return 0.0f;
    return std::clamp ((float) writePos.load (std::memory_order_acquire)
                       / (float) captureLen, 0.0f, 1.0f);
}

void VocalAnalyzer::pushSamples (const float* mono, int numSamples) noexcept
{
    if (! listening.load (std::memory_order_acquire))
        return;

    int pos = writePos.load (std::memory_order_relaxed);
    const int room = captureLen - pos;
    const int n = std::min (numSamples, room);

    if (n > 0)
    {
        std::memcpy (capture.data() + pos, mono, sizeof (float) * (size_t) n);
        pos += n;
        writePos.store (pos, std::memory_order_release);
    }

    if (pos >= captureLen)
    {
        listening.store (false, std::memory_order_release);
        analysing.store (true, std::memory_order_release);
        // hand off to the background thread; never analyse on the audio thread
        const_cast<VocalAnalyzer*> (this)->notify();
    }
}

void VocalAnalyzer::run()
{
    while (! threadShouldExit())
    {
        wait (-1);
        if (threadShouldExit()) break;

        if (analysing.load (std::memory_order_acquire))
        {
            analyse();
            analysing.store (false, std::memory_order_release);
            generation.fetch_add (1, std::memory_order_release);
        }
    }
}

//==============================================================================
void VocalAnalyzer::analyse()
{
    AnalysisResult r;
    r.tempoBpm   = pendingTempo;
    r.tempoValid = pendingTempoValid;

    computeLtasAndNoise (r);
    computeLoudness     (r);
    computePitchStats   (r);
    computeFormants     (r);
    computeSibilance    (r);
    computeResonances   (r);
    computeReverb       (r);
    deriveSettings      (r);

    r.valid = true;
    published = r;
}

//==============================================================================
void VocalAnalyzer::powerToBands (const std::vector<float>& power,
                                  std::array<float, numToneBands>& outDb) const
{
    for (int b = 0; b < numToneBands; ++b)
    {
        const float fc = toneBandHz[(size_t) b];
        const float lo = fc / 1.122462f;   // 2^(1/6)
        const float hi = fc * 1.122462f;

        const int binLo = std::max (1, (int) std::floor (lo * fftSize / sr));
        const int binHi = std::min ((int) power.size() - 1,
                                    (int) std::ceil  (hi * fftSize / sr));

        float sum = 0.0f;
        int   cnt = 0;
        for (int i = binLo; i <= binHi; ++i) { sum += power[(size_t) i]; ++cnt; }

        outDb[(size_t) b] = cnt > 0 ? toDb (sum / (float) cnt) : kMinDb;
    }
}

/** Welch-averaged LTAS plus a minimum-statistics noise floor. */
void VocalAnalyzer::computeLtasAndNoise (AnalysisResult& r)
{
    std::fill (avgPower.begin(), avgPower.end(), 0.0f);

    std::vector<float> frameRmsDb;
    int numFrames = 0;

    for (int start = 0; start + fftSize <= captureLen; start += hopSize)
    {
        std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);
        std::memcpy (fftScratch.data(), capture.data() + start,
                     sizeof (float) * (size_t) fftSize);

        // frame RMS before windowing, for the noise-floor statistics
        double acc = 0.0;
        for (int i = 0; i < fftSize; ++i)
            acc += (double) fftScratch[(size_t) i] * fftScratch[(size_t) i];
        frameRmsDb.push_back (toDb ((float) (acc / fftSize)));

        window.multiplyWithWindowingTable (fftScratch.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftScratch.data());

        std::vector<float> framePower ((size_t) fftSize / 2);
        for (int i = 0; i < fftSize / 2; ++i)
        {
            const float mag = fftScratch[(size_t) i];
            const float p   = mag * mag;
            framePower[(size_t) i] = p;
            avgPower[(size_t) i]  += p;
        }

        ++numFrames;
    }

    if (numFrames > 0)
        for (auto& p : avgPower) p /= (float) numFrames;

    powerToBands (avgPower, r.measuredLtasDb);

    // Minimum statistics: the quietest 5% of frames are the noise floor.
    // Robust against the fact that a vocal take is mostly silence between phrases.
    r.noiseFloorDb = percentile (frameRmsDb, 0.05f);
    const float speechDb = percentile (frameRmsDb, 0.90f);
    r.snrDb = speechDb - r.noiseFloorDb;

    // Second pass over the quiet frames only, to get the SHAPE of the noise.
    // Spectral subtraction needs the noise spectrum; handing it the full-signal
    // LTAS instead makes it subtract the vocal from itself.
    std::fill (noisePower.begin(), noisePower.end(), 0.0f);
    const float quietCutDb = r.noiseFloorDb + 3.0f;
    int noiseFrames = 0, frameIdx = 0;

    for (int start = 0; start + fftSize <= captureLen; start += hopSize, ++frameIdx)
    {
        if (frameIdx >= (int) frameRmsDb.size() || frameRmsDb[(size_t) frameIdx] > quietCutDb)
            continue;

        std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);
        std::memcpy (fftScratch.data(), capture.data() + start,
                     sizeof (float) * (size_t) fftSize);
        window.multiplyWithWindowingTable (fftScratch.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftScratch.data());

        for (int i = 0; i < fftSize / 2; ++i)
            noisePower[(size_t) i] += fftScratch[(size_t) i] * fftScratch[(size_t) i];
        ++noiseFrames;
    }

    if (noiseFrames > 0)
    {
        for (auto& v : noisePower) v /= (float) noiseFrames;
        powerToBands (noisePower, r.noiseLtasDb);
    }
    else
    {
        // No frame was quiet enough to profile: assume a flat floor.
        r.noiseLtasDb.fill (r.noiseFloorDb);
    }
}

/** ITU-R BS.1770 K-weighted loudness, plus crest factor and loudness range. */
void VocalAnalyzer::computeLoudness (AnalysisResult& r)
{
    Biquad shelf, hp;
    shelf.highShelf (sr, 1681.974450955533, 3.999843853973347, 0.7071752369554196);
    hp.highPass     (sr, 38.13547087602444, 0.5003270373238773);

    // 400 ms blocks with 75% overlap, per the spec
    const int blockLen = (int) (0.4 * sr);
    const int blockHop = blockLen / 4;

    std::vector<float> blockLoudness;
    std::vector<double> weighted ((size_t) captureLen);

    double peak = 0.0, sumSq = 0.0;
    for (int i = 0; i < captureLen; ++i)
    {
        const double x = capture[(size_t) i];
        peak  = std::max (peak, std::abs (x));
        sumSq += x * x;
        weighted[(size_t) i] = hp.process (shelf.process (x));
    }

    r.peakDb = (float) (20.0 * std::log10 (std::max (peak, 1.0e-9)));
    r.rmsDb  = (float) (10.0 * std::log10 (std::max (sumSq / captureLen, 1.0e-12)));
    r.crestFactorDb = r.peakDb - r.rmsDb;

    for (int start = 0; start + blockLen <= captureLen; start += blockHop)
    {
        double acc = 0.0;
        for (int i = 0; i < blockLen; ++i)
        {
            const double v = weighted[(size_t) (start + i)];
            acc += v * v;
        }
        const double meanSq = acc / blockLen;
        if (meanSq > 1.0e-12)
            blockLoudness.push_back ((float) (-0.691 + 10.0 * std::log10 (meanSq)));
    }

    if (! blockLoudness.empty())
    {
        // absolute gate at -70 LUFS, then relative gate at -10 LU
        std::vector<float> gated;
        for (float l : blockLoudness) if (l > -70.0f) gated.push_back (l);

        if (! gated.empty())
        {
            const float mean = std::accumulate (gated.begin(), gated.end(), 0.0f)
                             / (float) gated.size();
            std::vector<float> gated2;
            for (float l : gated) if (l > mean - 10.0f) gated2.push_back (l);

            if (! gated2.empty())
                r.integratedLufs = std::accumulate (gated2.begin(), gated2.end(), 0.0f)
                                 / (float) gated2.size();

            r.loudnessRangeDb = percentile (gated, 0.95f) - percentile (gated, 0.10f);
        }
    }
}

void VocalAnalyzer::computePitchStats (AnalysisResult& r)
{
    const int frame = 2048;
    const int hop   = 512;

    pitchTracker.prepare (sr, frame);
    pitchTracker.setRange (60.0f, 1200.0f);

    f0Track.clear();
    std::vector<float> voiced;
    std::vector<float> centsDev;
    int total = 0;

    for (int start = 0; start + frame <= captureLen; start += hop)
    {
        float conf = 0.0f;
        const float f0 = pitchTracker.process (capture.data() + start, &conf);
        ++total;
        f0Track.push_back (f0);

        if (f0 > 0.0f && conf > 0.55f)
        {
            voiced.push_back (f0);

            // deviation from the nearest equal-tempered semitone (A440)
            const float midi = 69.0f + 12.0f * std::log2 (f0 / 440.0f);
            const float dev  = (midi - std::round (midi)) * 100.0f;
            centsDev.push_back (std::abs (dev));
        }
    }

    r.voicedFraction = total > 0 ? (float) voiced.size() / (float) total : 0.0f;

    if (! voiced.empty())
    {
        r.medianF0Hz = percentile (voiced, 0.50f);
        r.f0P05Hz    = percentile (voiced, 0.05f);
        r.f0P95Hz    = percentile (voiced, 0.95f);
    }
    if (! centsDev.empty())
        r.pitchStabilityCents = percentile (centsDev, 0.50f);
}

/** Formants from a cepstrally-smoothed average voiced spectrum.

    Cheaper and far more numerically robust here than LPC root-solving, and we
    only need F1-F3 to characterise the voice for de-ess placement and to sanity
    check the tone match. */
void VocalAnalyzer::computeFormants (AnalysisResult& r)
{
    std::vector<float> logSpec ((size_t) fftSize / 2);
    for (size_t i = 0; i < logSpec.size(); ++i)
        logSpec[i] = toDb (avgPower[i]);

    // moving average in log-frequency ~ 1/3 octave smooths harmonics away
    std::vector<float> smooth (logSpec.size());
    for (size_t i = 1; i < logSpec.size(); ++i)
    {
        const float f  = binToHz ((int) i);
        const float lo = f / 1.12f, hi = f * 1.12f;
        const int   bl = std::max (1, (int) (lo * fftSize / sr));
        const int   bh = std::min ((int) logSpec.size() - 1, (int) (hi * fftSize / sr));

        float sum = 0.0f; int c = 0;
        for (int b = bl; b <= bh; ++b) { sum += logSpec[(size_t) b]; ++c; }
        smooth[i] = c > 0 ? sum / (float) c : logSpec[i];
    }

    auto findPeak = [&] (float loHz, float hiHz) -> float
    {
        const int bl = std::max (1, (int) (loHz * fftSize / sr));
        const int bh = std::min ((int) smooth.size() - 2, (int) (hiHz * fftSize / sr));
        int best = -1; float bestVal = kMinDb;

        for (int b = bl; b <= bh; ++b)
            if (smooth[(size_t) b] > bestVal
                && smooth[(size_t) b] >= smooth[(size_t) b - 1]
                && smooth[(size_t) b] >= smooth[(size_t) b + 1])
            { bestVal = smooth[(size_t) b]; best = b; }

        return best > 0 ? binToHz (best) : 0.0f;
    };

    r.f1Hz = findPeak (250.0f,  900.0f);
    r.f2Hz = findPeak (900.0f,  2600.0f);
    r.f3Hz = findPeak (2600.0f, 3800.0f);

    // spectral centroid and tilt over the musically relevant span
    double num = 0.0, den = 0.0;
    for (size_t i = 1; i < avgPower.size(); ++i)
    {
        const float f = binToHz ((int) i);
        if (f < 50.0f || f > 16000.0f) continue;
        num += (double) f * avgPower[i];
        den += avgPower[i];
    }
    r.spectralCentroidHz = den > 0.0 ? (float) (num / den) : 0.0f;

    // least-squares slope of dB against log2(f), 200 Hz .. 8 kHz
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (int b = 0; b < numToneBands; ++b)
    {
        const float f = toneBandHz[(size_t) b];
        if (f < 200.0f || f > 8000.0f) continue;
        const double x = std::log2 (f), y = r.measuredLtasDb[(size_t) b];
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
    }
    if (n > 1)
    {
        const double d = n * sxx - sx * sx;
        if (std::abs (d) > 1.0e-9)
            r.spectralTiltDbPerOct = (float) ((n * sxy - sx * sy) / d);
    }
}

/** Locate the singer's actual sibilance band rather than assuming one.

    Male sibilance typically centres 5-6 kHz and female 7-8 kHz, so a fixed band
    would be wrong about half the time. We isolate the frames that are actually
    sibilant and read the peak straight off their average spectrum. */
void VocalAnalyzer::computeSibilance (AnalysisResult& r)
{
    struct FrameRatio { int start; float ratio; };
    std::vector<FrameRatio> ratios;

    const int loBin = std::max (1, (int) (4000.0 * fftSize / sr));
    const int hiBin = std::min (fftSize / 2 - 1, (int) (12000.0 * fftSize / sr));

    for (int start = 0; start + fftSize <= captureLen; start += hopSize)
    {
        std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);
        std::memcpy (fftScratch.data(), capture.data() + start,
                     sizeof (float) * (size_t) fftSize);
        window.multiplyWithWindowingTable (fftScratch.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftScratch.data());

        double hf = 0.0, all = 0.0;
        for (int i = 1; i < fftSize / 2; ++i)
        {
            const double p = (double) fftScratch[(size_t) i] * fftScratch[(size_t) i];
            all += p;
            if (i >= loBin && i <= hiBin) hf += p;
        }
        if (all > 1.0e-10)
            ratios.push_back ({ start, (float) (hf / all) });
    }

    if (ratios.empty())
        return;

    std::vector<float> justRatios;
    justRatios.reserve (ratios.size());
    for (auto& fr : ratios) justRatios.push_back (fr.ratio);
    const float cut = percentile (justRatios, 0.85f);

    std::fill (sibilantAvgPower.begin(), sibilantAvgPower.end(), 0.0f);
    int count = 0;
    std::vector<float> sibLevels;

    for (auto& fr : ratios)
    {
        if (fr.ratio < cut) continue;

        std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);
        std::memcpy (fftScratch.data(), capture.data() + fr.start,
                     sizeof (float) * (size_t) fftSize);
        window.multiplyWithWindowingTable (fftScratch.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftScratch.data());

        double bandEnergy = 0.0;
        for (int i = 0; i < fftSize / 2; ++i)
        {
            const float p = fftScratch[(size_t) i] * fftScratch[(size_t) i];
            sibilantAvgPower[(size_t) i] += p;
            if (i >= loBin && i <= hiBin) bandEnergy += p;
        }
        sibLevels.push_back (toDb ((float) (bandEnergy / (hiBin - loBin + 1))));
        ++count;
    }

    if (count == 0)
        return;

    for (auto& p : sibilantAvgPower) p /= (float) count;

    // Pick the peak of the RATIO against the overall average spectrum, not of
    // raw energy. A vocal spectrum falls with frequency, so a raw peak search
    // lands at the low edge of the search window almost every time; the ratio
    // isolates what is actually different about the sibilant frames.
    int   peakBin = loBin;
    float peakRatio = -1.0f;
    for (int i = loBin; i <= hiBin; ++i)
    {
        const float ref = std::max (avgPower[(size_t) i], 1.0e-20f);
        const float ratio = sibilantAvgPower[(size_t) i] / ref;
        if (ratio > peakRatio) { peakRatio = ratio; peakBin = i; }
    }

    r.deEssCentreHz = binToHz (peakBin);

    // -6 dB width of the sibilant hump sets the filter Q
    const float half = sibilantAvgPower[(size_t) peakBin] * 0.25f;
    int lo = peakBin, hi = peakBin;
    while (lo > loBin && sibilantAvgPower[(size_t) lo] > half) --lo;
    while (hi < hiBin && sibilantAvgPower[(size_t) hi] > half) ++hi;

    const float loHz = std::max (binToHz (lo), 1.0f);
    const float hiHz = std::max (binToHz (hi), loHz * 1.05f);
    r.deEssBandwidthOct = std::clamp (std::log2 (hiHz / loHz), 0.5f, 2.5f);

    // The threshold has to be in the same units the de-esser's detector works
    // in -- peak amplitude of the filtered signal, in dBFS. Taking it from raw
    // FFT magnitudes (as an earlier version did) is off by tens of dB, because
    // an unnormalised N-point FFT of a windowed frame lives on a completely
    // different scale from the samples themselves. So: actually filter the
    // captured audio and measure the envelope we will really be looking at.
    measureSibilantLevel (r);
}

/** Run the capture through the chosen sibilant band and take percentiles of
    the resulting envelope, in dBFS. */
void VocalAnalyzer::measureSibilantLevel (AnalysisResult& r)
{
    juce::dsp::LinkwitzRileyFilter<float> hp;
    juce::dsp::ProcessSpec spec { sr, 512, 1 };
    hp.prepare (spec);
    hp.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
    hp.setCutoffFrequency (std::clamp (r.deEssCentreHz * 0.72f, 2500.0f, 9000.0f));
    hp.reset();

    // 5 ms peak envelope, matching the de-esser's fast detector
    const int   win = std::max (16, (int) (0.005 * sr));
    std::vector<float> envDb;
    envDb.reserve ((size_t) (captureLen / win + 1));

    float running = 0.0f;
    int   n = 0;

    for (int i = 0; i < captureLen; ++i)
    {
        const float y = std::abs (hp.processSample (0, capture[(size_t) i]));
        running = std::max (running, y);

        if (++n >= win)
        {
            envDb.push_back (running > 1.0e-9f ? 20.0f * std::log10 (running) : kMinDb);
            running = 0.0f;
            n = 0;
        }
    }

    if (envDb.empty())
        return;

    // Sibilant bursts are a small minority of frames -- often under 10% -- so a
    // percentile like p92 lands in the gap between the body's HF content and
    // the esses themselves and reads far too low. Take the peak from well
    // inside the sibilant population, and the reference from the body.
    const float sibPeak  = percentile (envDb, 0.995f);
    const float bodyHf   = percentile (envDb, 0.50f);
    const float spread   = std::max (0.0f, sibPeak - bodyHf);

    // Threshold sits proportionally below the peak: the spikier the sibilance,
    // the further down it has to reach to catch the whole ess.
    r.deEssThresholdDb = std::clamp (sibPeak - std::clamp (spread * 0.45f, 3.0f, 14.0f),
                                     -60.0f, -6.0f);
    r.sibilantPeakDb   = sibPeak;
    r.sibilantSpreadDb = spread;
}

/** Resonances = where the spectrum pokes above its own local envelope. */
void VocalAnalyzer::computeResonances (AnalysisResult& r)
{
    r.resonances.clear();

    // resample the spectrum onto a log-frequency grid so "one octave" is a
    // constant number of points
    constexpr int kGrid = 256;
    const float fLo = 80.0f, fHi = 16000.0f;

    std::vector<float> gridDb ((size_t) kGrid), gridHz ((size_t) kGrid);
    for (int i = 0; i < kGrid; ++i)
    {
        const float f = fLo * std::pow (fHi / fLo, (float) i / (float) (kGrid - 1));
        gridHz[(size_t) i] = f;

        const int bin = std::clamp ((int) std::round (f * fftSize / sr),
                                    1, fftSize / 2 - 1);
        gridDb[(size_t) i] = toDb (avgPower[(size_t) bin]);
    }

    // envelope = ~1 octave moving average on the log grid
    const int pointsPerOctave = (int) ((kGrid - 1) / std::log2 (fHi / fLo));
    const int halfWin = std::max (2, pointsPerOctave / 2);

    std::vector<float> env ((size_t) kGrid);
    for (int i = 0; i < kGrid; ++i)
    {
        const int a = std::max (0, i - halfWin);
        const int b = std::min (kGrid - 1, i + halfWin);
        float sum = 0.0f;
        for (int j = a; j <= b; ++j) sum += gridDb[(size_t) j];
        env[(size_t) i] = sum / (float) (b - a + 1);
    }

    // peaks poking >3 dB above the envelope become notches
    constexpr float kMinProminenceDb = 3.0f;
    for (int i = 1; i < kGrid - 1; ++i)
    {
        const float excess = gridDb[(size_t) i] - env[(size_t) i];
        if (excess < kMinProminenceDb) continue;
        if (gridDb[(size_t) i] < gridDb[(size_t) (i - 1)]) continue;
        if (gridDb[(size_t) i] < gridDb[(size_t) (i + 1)]) continue;

        // width at half prominence -> Q
        int lo = i, hi = i;
        const float halfEx = excess * 0.5f;
        while (lo > 0        && gridDb[(size_t) lo] - env[(size_t) lo] > halfEx) --lo;
        while (hi < kGrid - 1 && gridDb[(size_t) hi] - env[(size_t) hi] > halfEx) ++hi;

        const float bwOct = std::log2 (std::max (gridHz[(size_t) hi], 1.0f)
                                     / std::max (gridHz[(size_t) lo], 1.0f));
        if (bwOct <= 0.0f) continue;

        Resonance res;
        res.frequencyHz = gridHz[(size_t) i];
        // cut most of the excess but never fully - full removal sounds gutted
        res.gainDb = -std::min (excess * 0.7f, 9.0f);
        res.q      = std::clamp (1.0f / bwOct, 1.0f, 12.0f);
        r.resonances.push_back (res);
    }

    // keep the worst offenders only; a wall of notches is how auto-EQ sounds bad
    std::sort (r.resonances.begin(), r.resonances.end(),
               [] (const Resonance& a, const Resonance& b) { return a.gainDb < b.gainDb; });
    if (r.resonances.size() > 8)
        r.resonances.resize (8);
}

/** RT60 from note-offset decay slopes.

    A vocal take isn't an impulse response, but the decay after phrase endings
    carries the room. We fit dB-vs-time over the -5..-25 dB portion of each
    decay and extrapolate to 60 dB. */
void VocalAnalyzer::computeReverb (AnalysisResult& r)
{
    const int frameLen = (int) (0.010 * sr);   // 10 ms
    if (frameLen <= 0) return;

    std::vector<float> envDb;
    for (int start = 0; start + frameLen <= captureLen; start += frameLen)
    {
        double acc = 0.0;
        for (int i = 0; i < frameLen; ++i)
        {
            const double v = capture[(size_t) (start + i)];
            acc += v * v;
        }
        envDb.push_back (toDb ((float) (acc / frameLen)));
    }

    if (envDb.size() < 20) return;

    std::vector<float> slopes;
    const int minRun = 15;    // 150 ms

    for (size_t i = 1; i + (size_t) minRun < envDb.size(); ++i)
    {
        // a local maximum starts a candidate decay
        if (envDb[i] < envDb[i - 1]) continue;

        const float peak = envDb[i];
        if (peak < r.noiseFloorDb + 30.0f) continue;   // need headroom to measure

        size_t j = i, startFit = 0, endFit = 0;
        while (j + 1 < envDb.size() && envDb[j + 1] < envDb[j])
        {
            const float drop = peak - envDb[j];
            if (startFit == 0 && drop >= 5.0f)  startFit = j;
            if (drop >= 25.0f)                  { endFit = j; break; }
            ++j;
        }

        if (startFit == 0 || endFit == 0 || endFit <= startFit + 3) continue;

        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const int n = (int) (endFit - startFit + 1);
        for (size_t k = startFit; k <= endFit; ++k)
        {
            const double x = (double) k * 0.010;
            const double y = envDb[k];
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double d = n * sxx - sx * sx;
        if (std::abs (d) < 1.0e-9) continue;

        const double slope = (n * sxy - sx * sy) / d;   // dB per second
        if (slope < -1.0)
            slopes.push_back ((float) slope);

        i = endFit;
    }

    if (slopes.empty())
    {
        r.rt60Seconds = 0.0f;
        r.reverbRatio = 0.0f;
        return;
    }

    const float medianSlope = percentile (slopes, 0.50f);
    r.rt60Seconds = std::clamp (-60.0f / medianSlope, 0.0f, 3.0f);

    // crude direct-to-late split: anything decaying slower than 0.25 s reads as room
    r.reverbRatio = std::clamp ((r.rt60Seconds - 0.15f) / 0.85f, 0.0f, 1.0f);
}

//==============================================================================
/** Turn measurements into processor settings.

    Every number below traces back to something measured, not a preset. */
void VocalAnalyzer::deriveSettings (AnalysisResult& r)
{
    // ---- high-pass: below the singer's lowest sung note, not a fixed 80 Hz --
    if (r.f0P05Hz > 0.0f)
        r.highPassHz = std::clamp (r.f0P05Hz * 0.75f, 40.0f, 180.0f);
    else
        r.highPassHz = 80.0f;

    // ---- gate: sit above the measured noise floor, below the quietest phrase
    r.gateThresholdDb = std::clamp (r.noiseFloorDb + 8.0f, -80.0f, -20.0f);
    // shallow range when SNR is good; deep when there's real noise to kill
    r.gateRangeDb   = -std::clamp (r.snrDb * 0.35f, 6.0f, 30.0f);
    r.gateAttackMs  = 1.0f;
    // release tracks the room so gating doesn't chop the natural tail
    r.gateReleaseMs = std::clamp (r.rt60Seconds * 1000.0f * 0.6f, 60.0f, 500.0f);

    // ---- de-noise / de-verb: aggression scales with how bad the input is ----
    // Clean source -> near zero. Bad bedroom recording -> pushes hard.
    r.denoiseAmount = std::clamp ((45.0f - r.snrDb) / 30.0f, 0.0f, 1.0f);
    r.deverbAmount  = std::clamp ((r.rt60Seconds - 0.18f) / 0.6f, 0.0f, 1.0f);

    // ---- compression -------------------------------------------------------
    // Attack/release from crest factor, following the parameter-automation
    // literature: peaky signals want faster attack and slower release.
    const float crest = std::clamp (r.crestFactorDb, 6.0f, 30.0f);

    // Stage 1: slow leveller against an RMS detector, so its threshold is
    // directly comparable to the measured integrated loudness. Sitting it
    // slightly BELOW the programme level is what makes it level rather than
    // just catch occasional peaks.
    r.compLevelThreshDb  = r.integratedLufs - 3.0f;
    r.compLevelRatio     = std::clamp (1.0f + r.loudnessRangeDb / 8.0f, 1.5f, 4.0f);
    r.compLevelAttackMs  = std::clamp (60.0f - crest, 15.0f, 60.0f);
    r.compLevelReleaseMs = std::clamp (crest * 20.0f, 150.0f, 600.0f);

    // Stage 2: fast peak control against a peak detector. Its threshold has to
    // live on the peak scale, which sits a crest factor above the RMS one.
    r.compPeakThreshDb  = r.integratedLufs + std::clamp (crest * 0.55f, 4.0f, 14.0f);
    r.compPeakRatio     = std::clamp (2.0f + crest / 6.0f, 2.5f, 6.0f);
    r.compPeakAttackMs  = std::clamp (60.0f / crest, 1.0f, 15.0f);
    r.compPeakReleaseMs = std::clamp (crest * 6.0f, 40.0f, 200.0f);

    // Makeup restores what the stages remove at a typical loud moment, not at
    // the mean: estimating at the mean gives zero whenever the threshold sits
    // above it, which is exactly when makeup is needed most.
    const float loudRms  = r.integratedLufs + r.loudnessRangeDb * 0.5f;
    const float est1 = std::max (0.0f, loudRms - r.compLevelThreshDb)
                     * (1.0f - 1.0f / r.compLevelRatio);
    const float est2 = std::max (0.0f, r.peakDb - r.compPeakThreshDb)
                     * (1.0f - 1.0f / r.compPeakRatio) * 0.5f;
    r.makeupGainDb = std::clamp (est1 + est2, 0.0f, 12.0f);

    // ---- de-esser depth from how far the loud esses overshoot the threshold
    // Depth follows how far the esses stick out of the body, not a fixed value.
    r.deEssMaxReductionDb = -std::clamp (r.sibilantSpreadDb * 0.35f, 3.0f, 12.0f);

    // ---- saturation: dull/thin sources want more, bright ones want less ----
    {
        const float tiltNorm = std::clamp ((-r.spectralTiltDbPerOct - 2.0f) / 6.0f,
                                           0.0f, 1.0f);
        r.saturationDrive = std::clamp (tiltNorm * 0.6f
                                        + std::clamp (crest / 40.0f, 0.0f, 0.4f),
                                        0.0f, 1.0f);
    }

    // ---- tone match against the universal target ---------------------------
    // Align both curves on their 200 Hz-2 kHz average so we correct shape, not level.
    float measRef = 0.0f, tgtRef = 0.0f; int refCount = 0;
    for (int b = 0; b < numToneBands; ++b)
    {
        const float f = toneBandHz[(size_t) b];
        if (f < 200.0f || f > 2000.0f) continue;
        measRef += r.measuredLtasDb[(size_t) b];
        tgtRef  += kTargetLtasDb[(size_t) b];
        ++refCount;
    }
    if (refCount > 0) { measRef /= (float) refCount; tgtRef /= (float) refCount; }

    std::array<float, numToneBands> raw {};
    for (int b = 0; b < numToneBands; ++b)
    {
        const float meas = r.measuredLtasDb[(size_t) b] - measRef;
        const float tgt  = kTargetLtasDb[(size_t) b]    - tgtRef;

        // Don't chase bands that are essentially noise
        if (r.measuredLtasDb[(size_t) b] < r.noiseFloorDb + 6.0f)
        { raw[(size_t) b] = 0.0f; continue; }

        raw[(size_t) b] = std::clamp (tgt - meas, -8.0f, 8.0f);
    }

    // 3-band smoothing: broad tonal moves, never a comb
    for (int b = 0; b < numToneBands; ++b)
    {
        const int a = std::max (0, b - 1);
        const int c = std::min (numToneBands - 1, b + 1);
        r.toneMatchDb[(size_t) b] =
            (raw[(size_t) a] + raw[(size_t) b] + raw[(size_t) c]) / 3.0f;
    }

    solveToneFilterGains (r);
}

/** Adjacent 1/3-octave peaking filters overlap, so a cascade of them does NOT
    produce the curve you dialled in -- each band gets its own gain plus the
    skirts of its neighbours, and the result overshoots.

    Solve M * g = target, where M[i][j] is the dB that a 1 dB filter on band j
    contributes at band i's centre. Gauss-Seidel converges in a few sweeps
    because M is strongly diagonally dominant. Done here on the analysis
    thread so the audio thread never sees the cost. */
void VocalAnalyzer::solveToneFilterGains (AnalysisResult& r) const
{
    // A 1/3-octave bandwidth corresponds to Q ~ 4.32.
    const float p = std::pow (2.0f, 1.0f / 3.0f);
    const float q = std::sqrt (p) / (p - 1.0f);

    std::array<std::array<float, numToneBands>, numToneBands> m {};

    for (int j = 0; j < numToneBands; ++j)
    {
        const float f0 = toneBandHz[(size_t) j];
        if (f0 >= sr * 0.45)
        {
            m[(size_t) j][(size_t) j] = 1.0f;
            continue;
        }

        // response of a +1 dB peaking filter on band j
        auto c = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                     sr, f0, q, std::pow (10.0f, 1.0f / 20.0f));

        for (int i = 0; i < numToneBands; ++i)
        {
            const float fi = toneBandHz[(size_t) i];
            if (fi >= sr * 0.45) continue;
            const float mag = (float) c->getMagnitudeForFrequency (fi, sr);
            m[(size_t) i][(size_t) j] = mag > 0.0f ? 20.0f * std::log10 (mag) : 0.0f;
        }
    }

    std::array<float, numToneBands> g {};
    g.fill (0.0f);

    for (int iter = 0; iter < 40; ++iter)
    {
        for (int i = 0; i < numToneBands; ++i)
        {
            const float diag = m[(size_t) i][(size_t) i];
            if (std::abs (diag) < 1.0e-4f) { g[(size_t) i] = 0.0f; continue; }

            float sum = 0.0f;
            for (int j = 0; j < numToneBands; ++j)
                if (j != i) sum += m[(size_t) i][(size_t) j] * g[(size_t) j];

            const float want = (r.toneMatchDb[(size_t) i] - sum) / diag;
            // damped update keeps the sweep stable on the wide low bands
            g[(size_t) i] = std::clamp (g[(size_t) i] + 0.7f * (want - g[(size_t) i]),
                                        -12.0f, 12.0f);
        }
    }

    r.toneFilterGainDb = g;
}

} // namespace listenator
