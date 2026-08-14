#pragma once
#include <vector>

namespace listenator
{

/** YIN fundamental-frequency estimator (de Cheveigne & Kawahara 2002).

    Used in two places: offline during the LISTEN pass to build the pitch
    histogram and intonation statistics, and per-block in the pitch-repair /
    autotune stages. Returns 0 for unvoiced frames.
*/
class PitchTracker
{
public:
    void prepare (double sampleRate, int frameSize);

    /** @param data   frameSize samples
        @param outConfidence  1 - aperiodicity, 0..1
        @returns f0 in Hz, or 0 if unvoiced */
    float process (const float* data, float* outConfidence = nullptr);

    void setThreshold (float t) noexcept   { threshold = t; }
    void setRange (float minHz, float maxHz) noexcept { fMin = minHz; fMax = maxHz; }

private:
    double sr = 44100.0;
    int    n  = 2048;
    float  threshold = 0.15f;   // YIN's absolute threshold
    float  fMin = 60.0f, fMax = 1600.0f;

    std::vector<float> diff, cmnd;

    void   differenceFunction (const float* x);
    void   cumulativeMeanNormalise();
    int    absoluteThreshold() const;
    float  parabolicInterpolate (int tau) const;
};

} // namespace listenator
