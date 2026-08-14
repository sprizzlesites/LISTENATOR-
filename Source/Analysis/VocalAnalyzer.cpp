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

    // The quietest genuine delivery, as opposed to the quietest silence. Frames
    // more than 6 dB above the floor are treated as speech; the low percentile
    // of those is what the gate must stay underneath.
    {
        std::vector<float> speech;
        speech.reserve (frameRmsDb.size());
        for (float d : frameRmsDb)
            if (d > r.noiseFloorDb + 6.0f) speech.push_back (d);

        r.speechFloorDb = speech.empty() ? r.noiseFloorDb + 12.0f
                                         : percentile (speech, 0.08f);
    }

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

    // Energy-weighted centroid of the EXCESS -- how much louder the sibilant
    // frames are than average, weighted by how much energy is actually there.
    //
    // Picking the peak of the bare ratio puts the band wherever the proportional
    // jump is largest, which on a noisy recording is the top of the range where
    // there is barely any signal. That lands male sibilance up around 9 kHz when
    // it really sits between 5 and 8.
    double num = 0.0, den = 0.0;
    for (int i = loBin; i <= hiBin; ++i)
    {
        const float ref = std::max (avgPower[(size_t) i], 1.0e-20f);
        const float excess = sibilantAvgPower[(size_t) i] - ref;
        if (excess <= 0.0f) continue;
        const double w = (double) excess;
        num += w * binToHz (i);
        den += w;
    }

    int peakBin = loBin;
    if (den > 0.0)
    {
        r.deEssCentreHz = std::clamp ((float) (num / den), 3500.0f, 11000.0f);
        peakBin = std::clamp ((int) std::round (r.deEssCentreHz * fftSize / sr), loBin, hiBin);
    }
    else
    {
        r.deEssCentreHz = 6500.0f;
    }

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

/** Resonances = frequencies that poke above the local spectral envelope AND
    stay put across the whole take.

    That second condition is what separates a room resonance from the singer's
    own voice. A harmonic moves with the pitch, so across sub-windows of the
    capture it wanders; a wall mode sits at the same frequency all the way
    through. Without the consistency test the detector notches the fundamental
    and its first few harmonics, which is the fastest way to make a voice sound
    thin and hollow. */
void VocalAnalyzer::computeResonances (AnalysisResult& r)
{
    r.resonances.clear();

    constexpr int kSub  = 8;      // sub-windows across the capture
    constexpr int kGrid = 256;    // log-frequency points
    const float fLo = 80.0f, fHi = 12000.0f;

    std::vector<float> gridHz ((size_t) kGrid);
    for (int i = 0; i < kGrid; ++i)
        gridHz[(size_t) i] = fLo * std::pow (fHi / fLo, (float) i / (float) (kGrid - 1));

    // The envelope must span several harmonics of this voice, or every harmonic
    // reads as a peak. Below ~4*F0 there aren't enough harmonics to average, so
    // widen further there.
    const float f0 = r.medianF0Hz > 40.0f ? r.medianF0Hz : 140.0f;
    const int pointsPerOctave = (int) ((kGrid - 1) / std::log2 (fHi / fLo));

    std::vector<std::vector<float>> excess ((size_t) kSub,
                                            std::vector<float> ((size_t) kGrid, 0.0f));

    const int subLen = captureLen / kSub;

    for (int w = 0; w < kSub; ++w)
    {
        std::vector<float> power ((size_t) fftSize / 2, 0.0f);
        int frames = 0;

        for (int start = w * subLen;
             start + fftSize <= (w + 1) * subLen && start + fftSize <= captureLen;
             start += hopSize)
        {
            std::fill (fftScratch.begin(), fftScratch.end(), 0.0f);
            std::memcpy (fftScratch.data(), capture.data() + start,
                         sizeof (float) * (size_t) fftSize);
            window.multiplyWithWindowingTable (fftScratch.data(), (size_t) fftSize);
            fft.performFrequencyOnlyForwardTransform (fftScratch.data());

            for (int i = 0; i < fftSize / 2; ++i)
                power[(size_t) i] += fftScratch[(size_t) i] * fftScratch[(size_t) i];
            ++frames;
        }
        if (frames == 0) continue;
        for (auto& v : power) v /= (float) frames;

        std::vector<float> gridDb ((size_t) kGrid);
        for (int i = 0; i < kGrid; ++i)
        {
            const int bin = std::clamp ((int) std::round (gridHz[(size_t) i] * fftSize / sr),
                                        1, fftSize / 2 - 1);
            gridDb[(size_t) i] = toDb (power[(size_t) bin]);
        }

        for (int i = 0; i < kGrid; ++i)
        {
            // wider window where harmonics are sparse relative to frequency
            const float harmonicsHere = gridHz[(size_t) i] / f0;
            const float octaves = harmonicsHere < 6.0f ? 2.0f : 1.2f;
            const int half = std::max (3, (int) (octaves * pointsPerOctave * 0.5f));

            const int lo = std::max (0, i - half), hi = std::min (kGrid - 1, i + half);
            float sum = 0.0f;
            for (int j = lo; j <= hi; ++j) sum += gridDb[(size_t) j];
            excess[(size_t) w][(size_t) i] = gridDb[(size_t) i] - sum / (float) (hi - lo + 1);
        }
    }

    // A candidate must clear the threshold in most sub-windows to count.
    constexpr float kProminenceDb = 3.0f;
    constexpr int   kMinAgreement = 4;   // of 8

    struct Cand { float hz, db; int agree; };
    std::vector<Cand> cands;

    for (int i = 1; i < kGrid - 1; ++i)
    {
        int agree = 0;
        float meanExcess = 0.0f;

        for (int w = 0; w < kSub; ++w)
        {
            const float e = excess[(size_t) w][(size_t) i];
            if (e > kProminenceDb
                && e >= excess[(size_t) w][(size_t) (i - 1)]
                && e >= excess[(size_t) w][(size_t) (i + 1)])
                ++agree;
            meanExcess += e;
        }
        meanExcess /= (float) kSub;

        if (agree >= kMinAgreement && meanExcess > kProminenceDb * 0.5f)
            cands.push_back ({ gridHz[(size_t) i], meanExcess, agree });
    }

    // Worst first, then reject anything sitting on top of an accepted notch --
    // the log grid finds the same peak several points running otherwise.
    std::sort (cands.begin(), cands.end(),
               [] (const Cand& a, const Cand& b) { return a.db > b.db; });

    r.resonanceCandidates.clear();
    for (size_t i = 0; i < cands.size() && i < 10; ++i)
        r.resonanceCandidates.push_back ({ cands[i].hz, -cands[i].db, (float) cands[i].agree });

    for (const auto& c : cands)
    {
        if ((int) r.resonances.size() >= 6) break;

        bool tooClose = false;
        for (const auto& acc : r.resonances)
            if (std::abs (std::log2 (c.hz / acc.frequencyHz)) < 0.33f)   // 1/3 octave
            { tooClose = true; break; }
        if (tooClose) continue;

        Resonance res;
        res.frequencyHz = c.hz;
        // Take out most of the excess, never all of it: full removal sounds gutted.
        res.gainDb = -std::min ((c.db - kProminenceDb * 0.5f) * 0.8f, 8.0f);
        res.q      = std::clamp (4.0f + (c.db - kProminenceDb), 3.0f, 9.0f);
        if (res.gainDb < -0.8f)
            r.resonances.push_back (res);
    }
}

/** RT60 from decay slopes in the gaps of continuous delivery.

    The textbook method wants a loud impulse followed by 30 dB of clean
    monotonic decay. A rapper never provides that: delivery is continuous, gaps
    are short, and the tail rarely gets 30 dB down before the next syllable. The
    original constraints found almost nothing on real material and reported an
    anechoic room for a take recorded in an obviously live booth.

    So: shorter analysis frames, a much shorter fit range, and a percentile that
    favours the SLOW decays -- the fast ones are the singer stopping, the slow
    ones are the room. */
void VocalAnalyzer::computeReverb (AnalysisResult& r)
{
    const int frameLen = std::max (16, (int) (0.005 * sr));   // 5 ms
    std::vector<float> envDb;
    envDb.reserve ((size_t) (captureLen / frameLen + 1));

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

    if (envDb.size() < 40) return;

    const float frameSec = (float) frameLen / (float) sr;
    std::vector<float> rt60s;

    // A usable decay only needs to clear the floor, not dominate it.
    const float minPeak = r.noiseFloorDb + 12.0f;

    for (size_t i = 1; i + 12 < envDb.size(); ++i)
    {
        if (envDb[i] < envDb[i - 1]) continue;      // want a local maximum
        if (envDb[i] < minPeak) continue;

        const float peak = envDb[i];
        size_t j = i, startFit = 0, endFit = 0;

        // allow small upticks so a little noise doesn't abort the run
        int rises = 0;
        while (j + 1 < envDb.size() && rises < 2)
        {
            if (envDb[j + 1] > envDb[j] + 0.5f) ++rises;

            const float drop = peak - envDb[j];
            if (startFit == 0 && drop >= 3.0f)  startFit = j;
            if (drop >= 15.0f)                  { endFit = j; break; }
            if (envDb[j] < r.noiseFloorDb + 3.0f) break;   // into the floor
            ++j;
        }

        if (startFit == 0 || endFit == 0 || endFit < startFit + 4) continue;

        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const int n = (int) (endFit - startFit + 1);
        for (size_t k = startFit; k <= endFit; ++k)
        {
            const double x = (double) k * frameSec, y = envDb[k];
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double d = n * sxx - sx * sx;
        if (std::abs (d) < 1.0e-9) continue;

        const double slope = (n * sxy - sx * sy) / d;   // dB/s, negative
        if (slope > -2.0) continue;

        const float rt = (float) (-60.0 / slope);
        if (rt > 0.05f && rt < 3.0f)
            rt60s.push_back (rt);

        i = endFit;
    }

    r.rt60Seconds = rt60s.empty() ? 0.0f
                                  : std::clamp (percentile (rt60s, 0.85f), 0.0f, 2.0f);

    // ---- how much tail is actually audible ---------------------------------
    // RT60 describes the SHAPE of a decay; it says nothing about how much
    // energy is still ringing when the next word lands. On continuous delivery
    // that second quantity is what makes a booth sound washy, and it is what
    // de-verb should be driven by. Measure it directly: at each speech offset,
    // compare the energy 80-250 ms later against the energy just before.
    const int pre    = std::max (1, (int) (0.100 / frameSec));
    const int gapLo  = std::max (1, (int) (0.080 / frameSec));
    const int gapHi  = std::max (2, (int) (0.250 / frameSec));

    // Two windows across the same gap. Their difference is the decay rate, and
    // that is a far more trustworthy RT60 than a slope fit over a run that the
    // next syllable keeps cutting short.
    const int earlyLo = std::max (1, (int) (0.060 / frameSec));
    const int earlyHi = std::max (2, (int) (0.120 / frameSec));
    const int lateLo  = std::max (3, (int) (0.150 / frameSec));
    const int lateHi  = std::max (4, (int) (0.280 / frameSec));
    const float windowGapSec = 0.215f - 0.090f;

    std::vector<float> tailRatios, decayRates;

    for (size_t i = (size_t) pre; i + (size_t) gapHi < envDb.size(); ++i)
    {
        // an offset: was loud, drops away sharply
        const float before = envDb[i];
        if (before < r.noiseFloorDb + 18.0f) continue;
        if (envDb[i + (size_t) gapLo] > before - 6.0f) continue;

        float preAcc = 0.0f;
        for (int k = 0; k < pre; ++k) preAcc += envDb[i - (size_t) k];
        preAcc /= (float) pre;

        // The window must be a REAL gap. On continuous delivery the next
        // syllable usually lands inside 250 ms, and averaging it in reports a
        // wildly reverberant room for a dry one. Require the whole window to
        // stay down before believing any of it.
        float tailAcc = 0.0f, tailMax = -200.0f; int n = 0;
        for (int k = gapLo; k <= gapHi; ++k)
        {
            const float v = envDb[i + (size_t) k];
            tailAcc += v; tailMax = std::max (tailMax, v); ++n;
        }
        tailAcc /= (float) std::max (1, n);

        if (tailMax > before - 6.0f) continue;          // next word started
        if (tailAcc < r.noiseFloorDb + 3.0f) continue;  // ran into the floor

        // decay rate across the gap
        if (i + (size_t) lateHi < envDb.size())
        {
            float e = 0.0f, l = 0.0f; int ne = 0, nl = 0;
            for (int k = earlyLo; k <= earlyHi; ++k) { e += envDb[i + (size_t) k]; ++ne; }
            for (int k = lateLo;  k <= lateHi;  ++k) { l += envDb[i + (size_t) k]; ++nl; }
            e /= (float) std::max (1, ne);
            l /= (float) std::max (1, nl);

            if (l > r.noiseFloorDb + 3.0f && e > l)
                decayRates.push_back ((l - e) / windowGapSec);   // dB/s, negative
        }

        tailRatios.push_back (tailAcc - preAcc);
    }

    // Prefer the two-window decay rate for RT60. The slope fit reads short on
    // continuous delivery because it only ever sees the start of a decay before
    // the next syllable lands.
    if (! decayRates.empty())
    {
        const float slope = percentile (decayRates, 0.5f);
        if (slope < -1.0f)
            r.rt60Seconds = std::clamp (-60.0f / slope, 0.05f, 2.0f);
    }

    if (! tailRatios.empty())
    {
        // -35 dB and below is a dead room; -15 dB is a very live one.
        const float tailDb = percentile (tailRatios, 0.5f);
        r.tailDb = tailDb;
        r.tailSamples = (int) tailRatios.size();
        // -40 dB is a dead room, -12 dB is a very live one. The earlier 20 dB
        // span pinned at 1.0 on anything real and lost all resolution.
        r.reverbRatio = std::clamp ((tailDb + 40.0f) / 28.0f, 0.0f, 1.0f);

        // tailDb is measured in a window centred ~165 ms after the offset, by
        // which point the room has already decayed a long way. What the de-verb
        // needs is the reverberant level DURING delivery, which is that figure
        // extrapolated back to the moment the voice stopped.
        //
        // Skipping this step understates the reverb by the whole decay -- here
        // 8.8 dB, a factor of 2.8 in amplitude -- and the subtractor then removes
        // almost nothing and looks broken.
        const float windowCentreSec = 0.165f;
        const float decayDbPerSec = r.rt60Seconds > 0.05f ? 60.0f / r.rt60Seconds : 120.0f;
        r.directToReverbDb = std::clamp (tailDb + decayDbPerSec * windowCentreSec,
                                         -24.0f, -3.0f);
    }
    else
    {
        r.reverbRatio = std::clamp ((r.rt60Seconds - 0.12f) / 0.6f, 0.0f, 1.0f);
    }
}

//==============================================================================
/** Turn measurements into processor settings.

    Every number below traces back to something measured, not a preset. */
void VocalAnalyzer::deriveSettings (AnalysisResult& r)
{
    // ---- high-pass ---------------------------------------------------------
    // Two independent estimates, because on rap the pitch track is only voiced
    // maybe a fifth of the time and octave errors push f0P05 far too high.
    //   1. below the lowest sung note
    //   2. where the measured spectrum has genuinely fallen away
    // Take the lower. Cutting into the chest register to chase a bad pitch
    // reading is how an automatic HPF hollows out a male voice.
    float fromPitch = 110.0f;
    if (r.f0P05Hz > 0.0f && r.voicedFraction > 0.10f)
        fromPitch = std::clamp (r.f0P05Hz * 0.62f, 45.0f, 110.0f);

    float fromSpectrum = 80.0f;
    {
        // reference level across the body of the voice
        float bodyDb = kMinDb; int cnt = 0; float acc = 0.0f;
        for (int b = 0; b < numToneBands; ++b)
        {
            const float f = toneBandHz[(size_t) b];
            if (f < 150.0f || f > 500.0f) continue;
            acc += r.measuredLtasDb[(size_t) b]; ++cnt;
        }
        if (cnt > 0) bodyDb = acc / (float) cnt;

        // walk down until the spectrum sits well below the body
        for (int b = numToneBands - 1; b >= 0; --b)
        {
            const float f = toneBandHz[(size_t) b];
            if (f > 200.0f) continue;
            if (r.measuredLtasDb[(size_t) b] < bodyDb - 14.0f)
            { fromSpectrum = std::clamp (f * 1.15f, 40.0f, 110.0f); break; }
        }
    }

    // The corner stays where the voice says it should. What changed is the
    // slope: at 24 dB/oct the filter is 0.7 dB down at 100 Hz but 20 dB down at
    // 50 Hz, where a single section left a -6.6 dB hole in the chest register
    // AND +4.7 dB of surviving rumble.
    r.highPassHz = std::clamp (std::min (fromPitch, fromSpectrum), 45.0f, 110.0f);

    // ---- gate --------------------------------------------------------------
    // The threshold has to clear the noise floor AND stay under the quietest
    // real delivery. Punch-ins recorded at wildly different levels mean the
    // quiet takes can sit only a few dB above the floor, and a gate placed by
    // the floor alone swallows them whole.
    {
        const float aboveFloor = r.noiseFloorDb + 8.0f;
        const float underQuiet = r.speechFloorDb - 8.0f;
        r.gateThresholdDb = std::clamp (std::min (aboveFloor, underQuiet), -80.0f, -24.0f);
    }

    // Depth follows how much noise there actually is. A clean-ish take gets a
    // token amount of downward expansion, not a hard gate.
    // Depth has to cover the room as well as the hiss: on a live booth the
    // thing being pulled down between words is reverb, not noise.
    r.gateRangeDb   = -std::clamp (std::max ((40.0f - r.snrDb) * 0.6f,
                                             r.reverbRatio * 18.0f), 3.0f, 24.0f);
    r.gateAttackMs  = 1.5f;
    // release tracks the room so gating doesn't chop the natural tail
    // Deliberately NOT tied to RT60. The old rule stretched the release to
    // 600 ms on a live room, which is exactly the case where the expander needs
    // to be closing inside the gap -- it could never act before the next word.
    // What stops it chopping the natural decay is the soft expansion ratio, not
    // a slow release.
    // Fast enough to dig into the valleys BETWEEN syllables, which is where
    // both room and residual noise live. Syllable gaps run 50-200 ms, so a
    // release much beyond that never acts before the next word arrives.
    r.gateReleaseMs = std::clamp (45.0f + r.reverbRatio * 35.0f, 45.0f, 90.0f);

    // ---- de-noise / de-verb: aggression scales with how bad the input is ----
    // Clean source -> near zero. Bad bedroom recording -> pushes hard.
    // Measured this way, a decent booth lands around 22-28 dB SNR; the old
    // curve read that as "badly broken" and pushed spectral subtraction to 0.76,
    // which costs far more in artifacts than it removes in hiss.
    r.denoiseAmount = std::clamp ((28.0f - r.snrDb) / 22.0f, 0.0f, 0.7f);

    // Driven by how much tail is audible rather than by RT60. A tight-sounding
    // RT60 with a lot of energy still ringing between words is exactly the
    // booth-echo case, and keying off RT60 alone misses it completely.
    const float fromTail  = r.reverbRatio;
    const float fromRt60  = std::clamp ((r.rt60Seconds - 0.10f) / 0.35f, 0.0f, 1.0f);
    r.deverbAmount = std::clamp (std::max (fromTail, fromRt60) * 0.75f, 0.0f, 0.7f);

    // ---- compression -------------------------------------------------------
    // Attack/release from crest factor, following the parameter-automation
    // literature: peaky signals want faster attack and slower release.
    const float crest = std::clamp (r.crestFactorDb, 6.0f, 30.0f);

    // Stage 1: slow leveller against an RMS detector, so its threshold is
    // directly comparable to the measured integrated loudness. Sitting it
    // slightly BELOW the programme level is what makes it level rather than
    // just catch occasional peaks.
    // Under the quietest real delivery, not near the average. With the
    // threshold at LUFS-3 the quiet punch-ins sat entirely below it and got no
    // levelling at all, which is why the short-term spread stalled well above
    // target however hard the loud takes were squeezed.
    r.compLevelThreshDb = std::min (r.speechFloorDb + 2.0f, r.integratedLufs - 4.0f);

    // Ratio chosen to land the loudness range near 5 dB, which is about where a
    // rap vocal stops moving around in a mix.
    {
        // Aimed below the figure actually wanted. The stage is deliberately slow,
        // so within any short-term window it has only partly acted; targeting
        // 5 dB directly measured out at 6.7.
        constexpr float targetRangeDb = 3.5f;
        const float lra = std::max (r.loudnessRangeDb, 1.0f);
        const float wanted = lra > targetRangeDb ? lra / targetRangeDb : 1.0f;
        r.compLevelRatio = std::clamp (wanted, 1.5f, 4.0f);
    }

    // Slow on purpose. This stage exists to reconcile takes punched in at
    // different levels, which is a change over SECONDS. Timings fast enough to
    // follow syllables level the performance instead of the recording, and the
    // envelope modulation that goes with it is what makes a vocal read as dry --
    // flattening it sounds like more room, not less.
    r.compLevelAttackMs  = std::clamp (crest * 12.0f, 120.0f, 400.0f);
    r.compLevelReleaseMs = std::clamp (crest * 110.0f, 1200.0f, 3000.0f);

    // Stage 2: peak control, and deliberately restrained.
    //
    // Compression flattens the envelope, and a flattened envelope fills the
    // valleys between syllables -- perceptually the same thing reverberation
    // does. Measured on a live-booth take, an aggressive peak stage cost more
    // in envelope modulation (0.81 -> 0.62, where higher is drier) than the
    // whole de-verb chain recovered. This stage exists to catch occasional
    // peaks, not to level; the leveller above does the levelling, slowly enough
    // that syllable-rate modulation survives it.
    r.compPeakThreshDb  = r.integratedLufs + std::clamp (crest * 0.75f, 6.0f, 18.0f);
    r.compPeakRatio     = std::clamp (1.8f + crest / 14.0f, 2.0f, 3.2f);
    r.compPeakAttackMs  = std::clamp (60.0f / crest, 2.0f, 15.0f);
    r.compPeakReleaseMs = std::clamp (crest * 5.0f, 40.0f, 160.0f);

    // Makeup restores what the stages remove at a typical loud moment, not at
    // the mean: estimating at the mean gives zero whenever the threshold sits
    // above it, which is exactly when makeup is needed most.
    const float loudRms  = r.integratedLufs + r.loudnessRangeDb * 0.5f;
    const float est1 = std::max (0.0f, loudRms - r.compLevelThreshDb)
                     * (1.0f - 1.0f / r.compLevelRatio);
    const float est2 = std::max (0.0f, r.peakDb - r.compPeakThreshDb)
                     * (1.0f - 1.0f / r.compPeakRatio) * 0.4f;
    r.makeupGainDb = std::clamp (est1 + est2, 0.0f, 10.0f);

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

        // Asymmetric on purpose: a cut can only ever remove something that is
        // measurably too loud, while a boost lifts whatever else lives in that
        // band -- noise, room, sibilance. Boosts are also tightened further up
        // top, where there is least signal and most junk.
        const float maxBoost = toneBandHz[(size_t) b] > 8000.0f ? 2.5f : 4.0f;
        raw[(size_t) b] = std::clamp (tgt - meas, -8.0f, maxBoost);
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
    solveToneGainsInto (r, true,  r.toneFilterGainDb);
    solveToneGainsInto (r, false, r.toneFilterGainDbNoNotch);
}

void VocalAnalyzer::solveToneGainsInto (const AnalysisResult& r, bool withNotches,
                                        std::array<float, numToneBands>& result) const
{
    // A 1/3-octave bandwidth corresponds to Q ~ 4.32.
    const float p = std::pow (2.0f, 1.0f / 3.0f);
    const float q = std::sqrt (p) / (p - 1.0f);

    // What the rest of the chain will do to the spectrum AFTER this curve was
    // measured. The target has to be adjusted by it, or the tone stage spends
    // its effort fighting the high-pass and the notches instead of shaping tone.
    std::array<float, numToneBands> chain {};
    if (withNotches) accumulateChainResponse (r, chain);
    else             chain.fill (0.0f);

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

            const float target = r.toneMatchDb[(size_t) i] - chain[(size_t) i];
            const float want = (target - sum) / diag;
            // damped update keeps the sweep stable on the wide low bands
            g[(size_t) i] = std::clamp (g[(size_t) i] + 0.7f * (want - g[(size_t) i]),
                                        -12.0f, 12.0f);
        }
    }

    result = g;
}

void VocalAnalyzer::accumulateChainResponse (const AnalysisResult& r,
                                             std::array<float, numToneBands>& out) const
{
    out.fill (0.0f);

    // Deliberately excludes the high-pass. Compensating for it would have the
    // tone stage boost back exactly what the filter is there to remove -- which
    // measured as +19.6 dB of surviving rumble at 50 Hz when tried. Only the
    // notches belong here: their skirts reach into neighbouring bands as a side
    // effect, and that part is worth cancelling.
    for (int b = 0; b < numToneBands; ++b)
    {
        const float f = toneBandHz[(size_t) b];
        if (f >= sr * 0.45) continue;

        for (const auto& res : r.resonances)
        {
            auto c = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                         sr, res.frequencyHz, res.q, std::pow (10.0f, res.gainDb / 20.0f));
            const float rm = (float) c->getMagnitudeForFrequency (f, sr);
            if (rm > 1.0e-6f)
                out[(size_t) b] += 20.0f * std::log10 (rm);
        }
    }
}

} // namespace listenator
