#include "PhaseVocoderEngine.h"

#include <algorithm>
#include <cstring>

namespace
{
    constexpr float kTwoPi = 6.283185307179586476925286766559f;

    /** Wraps a phase difference into (-pi, pi]. */
    inline float wrapPhase (float phase) noexcept
    {
        long quotient = static_cast<long> (phase / juce::MathConstants<float>::pi);

        if (quotient >= 0)
            quotient += quotient & 1;
        else
            quotient -= quotient & 1;

        return phase - juce::MathConstants<float>::pi * static_cast<float> (quotient);
    }
}

void PhaseVocoderEngine::prepare (double sampleRate, int fftOrder, int overlapFactor)
{
    fs      = sampleRate;
    order   = juce::jlimit (8, 13, fftOrder);
    fftSize = 1 << order;
    osamp   = juce::jlimit (2, 8, overlapFactor);
    hopSize = fftSize / osamp;
    numBins = fftSize / 2 + 1;

    freqPerBin    = static_cast<float> (fs) / static_cast<float> (fftSize);
    expectedPhase = kTwoPi * static_cast<float> (hopSize) / static_cast<float> (fftSize);

    // Hann is applied on analysis and again on synthesis, so the overlap sums
    // to osamp * mean(hann^2) = osamp * 3/8.
    windowScale = 1.0f / (0.375f * static_cast<float> (osamp));

    fft = std::make_unique<juce::dsp::FFT> (order);
    cepstrum.prepare (order);

    window.resize (static_cast<size_t> (fftSize));
    for (int n = 0; n < fftSize; ++n)
        window[static_cast<size_t> (n)] =
            0.5f * (1.0f - std::cos (kTwoPi * static_cast<float> (n) / static_cast<float> (fftSize)));

    inFifo.assign      (static_cast<size_t> (fftSize), 0.0f);
    outFifo.assign     (static_cast<size_t> (fftSize), 0.0f);
    outputAccum.assign (static_cast<size_t> (fftSize) * 2, 0.0f);
    fftBuffer.assign   (static_cast<size_t> (fftSize) * 2, 0.0f);

    lastPhase.assign      (static_cast<size_t> (numBins), 0.0f);
    sumPhase.assign       (static_cast<size_t> (numBins), 0.0f);
    magnitude.assign      (static_cast<size_t> (numBins), 0.0f);
    trueFrequency.assign  (static_cast<size_t> (numBins), 0.0f);
    envelope.assign       (static_cast<size_t> (numBins), 1.0f);
    warpedEnvelope.assign (static_cast<size_t> (numBins), 1.0f);
    excitation.assign     (static_cast<size_t> (numBins), 0.0f);
    synthMagnitude.assign (static_cast<size_t> (numBins), 0.0f);
    synthFrequency.assign (static_cast<size_t> (numBins), 0.0f);

    envelopeSnapshot.assign (static_cast<size_t> (numBins), 1.0f);
    warpedSnapshot.assign   (static_cast<size_t> (numBins), 1.0f);

    inFifoLatency = fftSize - hopSize;
    rover         = inFifoLatency;
}

void PhaseVocoderEngine::reset()
{
    std::fill (inFifo.begin(),      inFifo.end(),      0.0f);
    std::fill (outFifo.begin(),     outFifo.end(),     0.0f);
    std::fill (outputAccum.begin(), outputAccum.end(), 0.0f);
    std::fill (lastPhase.begin(),   lastPhase.end(),   0.0f);
    std::fill (sumPhase.begin(),    sumPhase.end(),    0.0f);

    rover = inFifoLatency;
}

void PhaseVocoderEngine::setPitchSemitones (float semitones) noexcept
{
    pitchRatio.store (std::pow (2.0f, juce::jlimit (-24.0f, 24.0f, semitones) / 12.0f));
}

void PhaseVocoderEngine::setFormantSemitones (float semitones) noexcept
{
    formantRatio.store (std::pow (2.0f, juce::jlimit (-12.0f, 12.0f, semitones) / 12.0f));
}

void PhaseVocoderEngine::process (float* samples, int numSamples)
{
    if (fft == nullptr)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        inFifo[static_cast<size_t> (rover)] = samples[i];
        samples[i] = outFifo[static_cast<size_t> (rover - inFifoLatency)];
        ++rover;

        if (rover >= fftSize)
        {
            rover = inFifoLatency;
            processFrame();

            for (int k = 0; k < hopSize; ++k)
                outFifo[static_cast<size_t> (k)] = outputAccum[static_cast<size_t> (k)];

            std::memmove (outputAccum.data(),
                          outputAccum.data() + hopSize,
                          sizeof (float) * static_cast<size_t> (fftSize));

            std::fill (outputAccum.begin() + fftSize,
                       outputAccum.begin() + fftSize + hopSize,
                       0.0f);

            std::memmove (inFifo.data(),
                          inFifo.data() + hopSize,
                          sizeof (float) * static_cast<size_t> (inFifoLatency));
        }
    }
}

void PhaseVocoderEngine::processFrame()
{
    const float pitch   = pitchRatio.load();
    const float formant = formantRatio.load() * (linkFormants.load() ? pitch : 1.0f);
    const int   detail  = envelopeDetail.load();

    // --- Analysis -----------------------------------------------------------
    for (int n = 0; n < fftSize; ++n)
        fftBuffer[static_cast<size_t> (n)] = inFifo[static_cast<size_t> (n)] * window[static_cast<size_t> (n)];

    std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);

    fft->performRealOnlyForwardTransform (fftBuffer.data());

    for (int k = 0; k < numBins; ++k)
    {
        const float re = fftBuffer[static_cast<size_t> (2 * k)];
        const float im = fftBuffer[static_cast<size_t> (2 * k + 1)];

        magnitude[static_cast<size_t> (k)] = std::sqrt (re * re + im * im);

        const float phase = std::atan2 (im, re);

        float delta = phase - lastPhase[static_cast<size_t> (k)];
        lastPhase[static_cast<size_t> (k)] = phase;

        delta -= static_cast<float> (k) * expectedPhase;
        delta  = wrapPhase (delta);
        delta  = static_cast<float> (osamp) * delta / kTwoPi;

        trueFrequency[static_cast<size_t> (k)] = (static_cast<float> (k) + delta) * freqPerBin;
    }

    // --- Source / filter separation ----------------------------------------
    cepstrum.compute (magnitude.data(), envelope.data(), detail);

    for (int k = 0; k < numBins; ++k)
        excitation[static_cast<size_t> (k)] =
            juce::jmin (100.0f, magnitude[static_cast<size_t> (k)]
                                    / juce::jmax (1.0e-6f, envelope[static_cast<size_t> (k)]));

    // --- Shift the excitation, warp the envelope ---------------------------
    std::fill (synthMagnitude.begin(), synthMagnitude.end(), 0.0f);
    std::fill (synthFrequency.begin(), synthFrequency.end(), 0.0f);

    for (int k = 0; k < numBins; ++k)
    {
        const int target = static_cast<int> (std::lround (static_cast<float> (k) * pitch));

        if (target >= 0 && target < numBins)
        {
            synthMagnitude[static_cast<size_t> (target)] += excitation[static_cast<size_t> (k)];
            synthFrequency[static_cast<size_t> (target)]  = trueFrequency[static_cast<size_t> (k)] * pitch;
        }
    }

    CepstralEnvelope::warp (envelope.data(), warpedEnvelope.data(), numBins, formant);

    // --- Synthesis ----------------------------------------------------------
    for (int k = 0; k < numBins; ++k)
    {
        const float mag = synthMagnitude[static_cast<size_t> (k)] * warpedEnvelope[static_cast<size_t> (k)];

        float delta = synthFrequency[static_cast<size_t> (k)] / freqPerBin - static_cast<float> (k);
        delta *= kTwoPi / static_cast<float> (osamp);
        delta += static_cast<float> (k) * expectedPhase;

        sumPhase[static_cast<size_t> (k)] += delta;

        const float p = sumPhase[static_cast<size_t> (k)];

        fftBuffer[static_cast<size_t> (2 * k)]     = mag * std::cos (p);
        fftBuffer[static_cast<size_t> (2 * k + 1)] = mag * std::sin (p);
    }

    // Restore Hermitian symmetry before the inverse transform.
    fftBuffer[1] = 0.0f;
    fftBuffer[static_cast<size_t> (2 * (numBins - 1) + 1)] = 0.0f;

    for (int k = 1; k < fftSize / 2; ++k)
    {
        fftBuffer[static_cast<size_t> (2 * (fftSize - k))]     =  fftBuffer[static_cast<size_t> (2 * k)];
        fftBuffer[static_cast<size_t> (2 * (fftSize - k) + 1)] = -fftBuffer[static_cast<size_t> (2 * k + 1)];
    }

    fft->performRealOnlyInverseTransform (fftBuffer.data());

    for (int n = 0; n < fftSize; ++n)
        outputAccum[static_cast<size_t> (n)] +=
            window[static_cast<size_t> (n)] * fftBuffer[static_cast<size_t> (n)] * windowScale;

    // --- Display snapshot ---------------------------------------------------
    std::copy (envelope.begin(),       envelope.end(),       envelopeSnapshot.begin());
    std::copy (warpedEnvelope.begin(), warpedEnvelope.end(), warpedSnapshot.begin());
    snapshotDirty.store (true);
}
