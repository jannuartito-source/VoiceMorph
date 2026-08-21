#pragma once

#include "CepstralEnvelope.h"

#include <atomic>
#include <juce_dsp/juce_dsp.h>
#include <vector>

/**
    Source-filter voice transformer built on a phase vocoder.

    Per analysis frame:
      1. STFT, and estimate each bin's true frequency from the phase advance.
      2. Split the magnitude spectrum into a formant envelope and a flat
         excitation.
      3. Pitch-shift the excitation only. Because it carries no formant
         information, moving it does not move the formants.
      4. Warp the envelope by its own, separate ratio and reapply it.
      5. Resynthesise with accumulated phase, then overlap-add.

    Latency is fftSize - hopSize samples.
*/
class PhaseVocoderEngine
{
public:
    void prepare (double sampleRate, int fftOrder = 11, int overlapFactor = 4);
    void reset();

    /** Pitch transposition in semitones. */
    void setPitchSemitones (float semitones) noexcept;

    /** Formant transposition in semitones, independent of pitch. */
    void setFormantSemitones (float semitones) noexcept;

    /** When true the formants follow the pitch, giving the classic
        tape-speed / chipmunk sound instead of a natural transformation. */
    void setLinkFormantsToPitch (bool shouldLink) noexcept { linkFormants = shouldLink; }

    /** Lifter length in samples. Lower is smoother, higher tracks narrower
        resonances but risks picking up the pitch harmonics. */
    void setEnvelopeDetail (int quefrencyCutoff) noexcept { envelopeDetail = quefrencyCutoff; }

    /** Processes mono audio in place. Any block size is accepted. */
    void process (float* samples, int numSamples);

    int getLatencySamples() const noexcept { return fftSize - hopSize; }

    /** Snapshot of the most recent envelope, for the editor's display.
        Written on the audio thread, read on the message thread; a torn read
        only ever costs one slightly stale frame of a decorative curve. */
    const std::vector<float>& getEnvelopeSnapshot() const noexcept { return envelopeSnapshot; }
    const std::vector<float>& getWarpedSnapshot()   const noexcept { return warpedSnapshot; }
    int  getSnapshotBinCount() const noexcept { return numBins; }
    bool consumeSnapshotDirtyFlag() noexcept    { return snapshotDirty.exchange (false); }

private:
    void processFrame();

    double fs      = 48000.0;
    int    order   = 11;
    int    fftSize = 2048;
    int    hopSize = 512;
    int    osamp   = 4;
    int    numBins = 1025;

    float freqPerBin    = 0.0f;
    float expectedPhase = 0.0f;
    float windowScale   = 1.0f;

    int rover          = 0;
    int inFifoLatency  = 0;

    std::unique_ptr<juce::dsp::FFT> fft;
    CepstralEnvelope                cepstrum;

    std::vector<float> window;
    std::vector<float> inFifo, outFifo, outputAccum, fftBuffer;
    std::vector<float> lastPhase, sumPhase;
    std::vector<float> magnitude, trueFrequency;
    std::vector<float> envelope, warpedEnvelope, excitation;
    std::vector<float> synthMagnitude, synthFrequency;

    std::vector<float> envelopeSnapshot, warpedSnapshot;
    std::atomic<bool>  snapshotDirty { false };

    std::atomic<float> pitchRatio   { 1.0f };
    std::atomic<float> formantRatio { 1.0f };
    std::atomic<bool>  linkFormants { false };
    std::atomic<int>   envelopeDetail { 70 };
};
