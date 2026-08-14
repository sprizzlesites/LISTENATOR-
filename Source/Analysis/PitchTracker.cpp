#include "PitchTracker.h"
#include <cmath>
#include <algorithm>

namespace listenator
{

void PitchTracker::prepare (double sampleRate, int frameSize)
{
    sr = sampleRate;
    n  = frameSize;
    diff.assign ((size_t) (n / 2), 0.0f);
    cmnd.assign ((size_t) (n / 2), 0.0f);
}

// d(tau) = sum_j (x[j] - x[j+tau])^2   -- YIN step 2
void PitchTracker::differenceFunction (const float* x)
{
    const int half = n / 2;
    for (int tau = 0; tau < half; ++tau)
    {
        float sum = 0.0f;
        for (int j = 0; j < half; ++j)
        {
            const float d = x[j] - x[j + tau];
            sum += d * d;
        }
        diff[(size_t) tau] = sum;
    }
}

// d'(tau) = d(tau) / ((1/tau) * sum_{j=1..tau} d(j))   -- YIN step 3
void PitchTracker::cumulativeMeanNormalise()
{
    const int half = n / 2;
    cmnd[0] = 1.0f;
    float running = 0.0f;

    for (int tau = 1; tau < half; ++tau)
    {
        running += diff[(size_t) tau];
        cmnd[(size_t) tau] = running > 0.0f
                           ? diff[(size_t) tau] * (float) tau / running
                           : 1.0f;
    }
}

// First local minimum below the threshold -- YIN step 4
int PitchTracker::absoluteThreshold() const
{
    const int half   = n / 2;
    const int tauMin = std::max (2, (int) (sr / fMax));
    const int tauMax = std::min (half - 1, (int) (sr / fMin));

    for (int tau = tauMin; tau < tauMax; ++tau)
    {
        if (cmnd[(size_t) tau] < threshold)
        {
            // walk down to the actual local minimum
            while (tau + 1 < tauMax && cmnd[(size_t) (tau + 1)] < cmnd[(size_t) tau])
                ++tau;

            return tau;
        }
    }
    return -1;
}

// Parabolic interpolation around tau for sub-sample period -- YIN step 5
float PitchTracker::parabolicInterpolate (int tau) const
{
    const int half = n / 2;
    if (tau <= 0 || tau >= half - 1)
        return (float) tau;

    const float s0 = cmnd[(size_t) (tau - 1)];
    const float s1 = cmnd[(size_t) tau];
    const float s2 = cmnd[(size_t) (tau + 1)];

    const float denom = 2.0f * (2.0f * s1 - s2 - s0);
    if (std::abs (denom) < 1.0e-12f)
        return (float) tau;

    return (float) tau + (s2 - s0) / denom;
}

float PitchTracker::process (const float* data, float* outConfidence)
{
    if ((int) diff.size() != n / 2)
        prepare (sr, n);

    differenceFunction (data);
    cumulativeMeanNormalise();

    const int tau = absoluteThreshold();

    if (tau < 0)
    {
        if (outConfidence != nullptr) *outConfidence = 0.0f;
        return 0.0f;
    }

    const float refined = parabolicInterpolate (tau);
    if (refined <= 0.0f)
    {
        if (outConfidence != nullptr) *outConfidence = 0.0f;
        return 0.0f;
    }

    if (outConfidence != nullptr)
        *outConfidence = std::clamp (1.0f - cmnd[(size_t) tau], 0.0f, 1.0f);

    return (float) (sr / (double) refined);
}

} // namespace listenator
