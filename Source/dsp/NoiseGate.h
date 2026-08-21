#pragma once

#include <cmath>
#include <juce_core/juce_core.h>

/**
    A one-pole gate placed before the vocoder.

    Spectral processing raises the noise floor: the excitation/envelope split
    divides by small numbers in quiet bins, so untreated room tone comes back
    as a metallic wash. Gating first is cheaper and sounds better than trying
    to clean it up afterwards.
*/
class NoiseGate
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        setTimes (2.0f, 80.0f);
        envelope = 0.0f;
        gain     = 0.0f;
    }

    void setTimes (float attackMs, float releaseMs)
    {
        attackCoeff  = std::exp (-1.0f / (0.001f * attackMs  * static_cast<float> (fs)));
        releaseCoeff = std::exp (-1.0f / (0.001f * releaseMs * static_cast<float> (fs)));
    }

    void setThresholdDb (float db) noexcept
    {
        threshold = juce::Decibels::decibelsToGain (db, -100.0f);
    }

    void process (float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float rectified = std::abs (samples[i]);
            const float coeff     = rectified > envelope ? attackCoeff : releaseCoeff;

            envelope = rectified + coeff * (envelope - rectified);

            // Hysteresis: open at the threshold, stay open until 6 dB below it,
            // so a voice trailing off does not chatter the gate.
            if (envelope > threshold)              gateOpen = true;
            else if (envelope < threshold * 0.5f)  gateOpen = false;

            const float target = gateOpen ? 1.0f : 0.0f;
            const float slew   = target > gain ? 0.01f : 0.0008f;

            gain = juce::jlimit (0.0f, 1.0f, gain + slew * (target - gain));

            samples[i] *= gain;
        }
    }

private:
    double fs = 48000.0;

    float attackCoeff  = 0.0f;
    float releaseCoeff = 0.0f;
    float threshold    = 0.003f;
    float envelope     = 0.0f;
    float gain         = 0.0f;
    bool  gateOpen     = false;
};
