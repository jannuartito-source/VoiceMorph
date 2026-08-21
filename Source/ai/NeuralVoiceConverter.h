#pragma once

#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

/**
    Neural voice-conversion stage.

    This is the part that does what Vocoflex does: instead of moving formants
    around with a filter, it encodes speech into a content representation that
    has had the speaker's identity stripped out, then re-synthesises it with a
    different identity. The phase vocoder can make you sound taller or shorter.
    This can make you sound like a different person.

    The plugin cannot do that on its own. It needs two trained models, which
    you supply as ONNX files:

      1. A content encoder (ContentVec or HuBERT, 16 kHz in, ~768-dim frames
         out). This throws away timbre and keeps phonetic content.
      2. A decoder / generator that takes those frames plus a speaker embedding
         and an f0 contour, and produces audio.

    Both are exportable from the RVC and so-vits-svc toolchains. See README.md.

    Threading: the audio thread only ever pushes to and pops from lock-free
    FIFOs. Inference runs on a worker thread, in Hann-windowed overlapping
    blocks that are overlap-added back together. This costs one block of
    latency, reported to the host.
*/
class NeuralVoiceConverter : private juce::Thread
{
public:
    NeuralVoiceConverter();
    ~NeuralVoiceConverter() override;

    void prepare (double hostSampleRate, int blockSizeMilliseconds = 200);
    void reset();
    void releaseResources();

    /** Loads the two ONNX graphs. Safe to call while audio is running: the
        worker is suspended for the swap. Returns false and fills @p errorOut
        on failure. */
    bool loadModel (const juce::File& contentEncoder,
                    const juce::File& decoder,
                    juce::String& errorOut);

    void unloadModel();

    bool isModelLoaded() const noexcept { return modelLoaded.load(); }
    bool isBuiltWithOnnx() const noexcept;

    void  setEnabled (bool shouldBeEnabled) noexcept { enabled.store (shouldBeEnabled); }
    void  setTargetSpeaker (int speakerIndex) noexcept { targetSpeaker.store (speakerIndex); }
    void  setPitchOffsetSemitones (float semitones) noexcept { pitchOffset.store (semitones); }

    int getLatencySamples() const noexcept { return latencySamples; }

    /** Pushes @p numSamples of mono input and pulls the same number of
        converted samples back. Returns false if the output FIFO has run dry,
        in which case @p samples is left untouched and the caller should keep
        using its dry or DSP-processed signal. */
    bool process (float* samples, int numSamples);

    /** Rough load figure, 0 to 1, for the editor. */
    float getInferenceLoad() const noexcept { return inferenceLoad.load(); }

private:
    void run() override;
    void processOneBlock();

    /** Runs the loaded graphs over one block at host sample rate. Falls back
        to a straight copy when no model is available, so the signal path is
        always testable. */
    void runModel (const float* input, float* output, int numSamples);

    double hostRate    = 48000.0;
    int    blockSize   = 9600;   // N
    int    hopSize     = 4800;   // N / 2
    int    latencySamples = 9600;

    juce::AbstractFifo inputFifo  { 1 };
    juce::AbstractFifo outputFifo { 1 };
    std::vector<float> inputStore, outputStore;

    std::vector<float> history;       // previous hop, for the 50 % overlap
    std::vector<float> workBuffer;    // N samples fed to the model
    std::vector<float> modelOutput;   // N samples back from the model
    std::vector<float> overlapAccum;  // N-sample overlap-add accumulator
    std::vector<float> hannWindow;
    std::vector<float> emitBuffer;

    std::atomic<bool>  enabled       { false };
    std::atomic<bool>  modelLoaded   { false };
    std::atomic<int>   targetSpeaker { 0 };
    std::atomic<float> pitchOffset   { 0.0f };
    std::atomic<float> inferenceLoad { 0.0f };

    juce::CriticalSection modelLock;

    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeuralVoiceConverter)
};
