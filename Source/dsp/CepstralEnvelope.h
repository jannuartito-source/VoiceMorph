#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <memory>
#include <vector>

/**
    Estimates the spectral envelope of a magnitude spectrum using real-cepstrum
    liftering.

    Why this matters: a voice is a pitched excitation (the vocal folds) passed
    through a resonant filter (the vocal tract). The excitation sets the pitch;
    the filter's resonances -- the formants -- set the perceived size of the
    speaker. Shifting both together gives the chipmunk effect. Separating them
    is what makes a convincing male <-> female transformation.

    The log magnitude spectrum is the sum of a slowly varying part (the
    envelope / formants) and a fast ripple (the harmonic comb, spaced at f0).
    Transforming the log spectrum gives the cepstrum, where those two live at
    different quefrencies: the envelope near zero, the harmonic comb at the
    pitch period. Zeroing everything above a cutoff keeps only the envelope.
*/
class CepstralEnvelope
{
public:
    void prepare (int fftOrder)
    {
        order   = fftOrder;
        fftSize = 1 << order;
        fft     = std::make_unique<juce::dsp::FFT> (order);
        work.assign (static_cast<size_t> (fftSize) * 2, 0.0f);
    }

    /** @param magnitude        input, fftSize/2 + 1 linear magnitudes
        @param envelope         output, fftSize/2 + 1 linear magnitudes
        @param quefrencyCutoff  lifter length in samples. Must sit below the
                                pitch period (sampleRate / lowest expected f0),
                                otherwise the harmonics leak into the envelope.
    */
    void compute (const float* magnitude, float* envelope, int quefrencyCutoff)
    {
        const int numBins = fftSize / 2 + 1;

        std::fill (work.begin(), work.end(), 0.0f);

        // 1. Log magnitude, mirrored into a real, even sequence.
        for (int k = 0; k < numBins; ++k)
        {
            const float logMag = std::log (magnitude[k] + 1.0e-7f);
            work[static_cast<size_t> (k)] = logMag;

            if (k > 0 && k < fftSize / 2)
                work[static_cast<size_t> (fftSize - k)] = logMag;
        }

        // 2. Forward transform -> real cepstrum (imaginary parts are ~0).
        fft->performRealOnlyForwardTransform (work.data());

        // 3. Lifter: keep low quefrencies only, preserving the mirror.
        const int q = juce::jlimit (2, fftSize / 2 - 1, quefrencyCutoff);

        for (int n = q + 1; n < fftSize - q; ++n)
        {
            work[static_cast<size_t> (2 * n)]     = 0.0f;
            work[static_cast<size_t> (2 * n + 1)] = 0.0f;
        }

        for (int n = 0; n <= q; ++n)
            work[static_cast<size_t> (2 * n + 1)] = 0.0f;

        for (int n = fftSize - q; n < fftSize; ++n)
            work[static_cast<size_t> (2 * n + 1)] = 0.0f;

        // 4. Back to the log-spectral domain, then out of the log.
        fft->performRealOnlyInverseTransform (work.data());

        for (int k = 0; k < numBins; ++k)
            envelope[k] = std::exp (juce::jlimit (-30.0f, 30.0f, work[static_cast<size_t> (k)]));
    }

    /** Resamples an envelope along the frequency axis.

        A ratio above 1 pushes the formants upward, which is heard as a smaller
        vocal tract -- a shorter throat, a younger or more feminine timbre.
    */
    static void warp (const float* source, float* destination, int numBins, float ratio)
    {
        const float safeRatio = juce::jlimit (0.25f, 4.0f, ratio);

        for (int k = 0; k < numBins; ++k)
        {
            const float pos = static_cast<float> (k) / safeRatio;

            if (pos >= static_cast<float> (numBins - 1))
            {
                destination[k] = source[numBins - 1];
                continue;
            }

            const int   i0    = static_cast<int> (pos);
            const float frac  = pos - static_cast<float> (i0);

            destination[k] = source[i0] + frac * (source[i0 + 1] - source[i0]);
        }
    }

    int getFFTSize() const noexcept { return fftSize; }

private:
    int order   = 11;
    int fftSize = 2048;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float>              work;
};
