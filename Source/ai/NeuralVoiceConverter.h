#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

/**
    Zero-shot voice conversion — the Vocoflex mechanic.

    The important idea, and the one that separates this from RVC or
    so-vits-svc: nothing here is trained on your target voice. Three
    general-purpose models do the work.

      1. A **content encoder** turns your speech into frames that describe
         *what was said* with the speaker's identity stripped out.
      2. A **speaker encoder** listens to a few seconds of any voice and
         squeezes it into one vector, typically 256 numbers. That vector is
         the voice's identity: its "tone colour".
      3. A **decoder** puts the two back together.

    Because identity is just a vector, you can do arithmetic on it. Load two
    reference voices, interpolate between their vectors, and you get a voice
    that exists between them. That is exactly what the 2D map in Vocoflex is:
    a space of speaker embeddings you navigate by interpolation. This class
    exposes a one-dimensional version of it — a morph slider between two
    loaded references — because a line is honest about what the maths does
    and a plane mostly just looks better in screenshots.

    You supply the three models as ONNX files. See README.md for which
    published models work and how to export them.

    Threading: the audio thread only touches lock-free FIFOs. Inference runs
    on a worker thread in overlapping blocks, costing one block of latency.
*/
class NeuralVoiceConverter : private juce::Thread
{
public:
    static constexpr int numReferenceSlots = 2;

    NeuralVoiceConverter();
    ~NeuralVoiceConverter() override;

    /** @param hopMilliseconds  how much new audio each inference produces.
                                 This alone sets latency.
        @param contextPercent    how much *past* audio is prepended, as a
                                 percentage of the hop. Context costs CPU and
                                 improves quality but adds no latency, because
                                 it is audio that already went by. */
    void prepare (double hostSampleRate, int hopMilliseconds = 120,
                  int contextPercent = 100);
    void reset();
    void releaseResources();

    // --- Models -------------------------------------------------------------

    /** Loads all three graphs from one folder. Expects the files to be named
        `content.onnx`, `speaker.onnx` and `decoder.onnx`. */
    bool loadModels (const juce::File& folder, juce::String& errorOut);

    void unloadModels();
    bool areModelsLoaded() const noexcept { return modelsLoaded.load(); }
    bool isBuiltWithOnnx() const noexcept;

    // --- Reference voices ---------------------------------------------------

    /** Reads an audio file, runs the speaker encoder over it, and stores the
        resulting identity vector in @p slot. Any format JUCE can read works;
        five to fifteen seconds of clean speech is ideal. */
    bool loadReferenceVoice (int slot, const juce::File& audioFile, juce::String& errorOut);

    void clearReferenceVoice (int slot);
    bool hasReferenceVoice (int slot) const noexcept;
    juce::String getReferenceName (int slot) const;

    /** 0 puts you fully on slot A, 1 fully on slot B, anything between is an
        interpolation of the two identity vectors. */
    void setMorph (float zeroToOne) noexcept
    {
        const auto clamped = juce::jlimit (0.0f, 1.0f, zeroToOne);

        if (std::abs (clamped - morph.load()) > 1.0e-4f)
        {
            morph.store (clamped);
            embeddingDirty.store (true);
        }
    }

    // --- Runtime ------------------------------------------------------------

    void setEnabled (bool shouldBeEnabled) noexcept { enabled.store (shouldBeEnabled); }
    void setPitchOffsetSemitones (float semitones) noexcept { pitchOffset.store (semitones); }

    int   getLatencySamples() const noexcept { return latencySamples; }
    float getInferenceLoad()  const noexcept { return inferenceLoad.load(); }

    /** Last failure from the worker thread, empty if none. Inference runs off
        the audio thread, so without this a thrown exception is invisible: the
        plugin just quietly keeps passing the vocoder output through. */
    juce::String getLastError() const;

    /** Blocks successfully converted. A frozen counter with the enable box
        ticked means the worker has stopped, which no other reading shows. */
    int getBlocksConverted() const noexcept { return blocksConverted.load(); }

    /** True when there is enough loaded to actually convert: models plus at
        least one reference voice. */
    bool isReady() const noexcept;

    /** Pushes mono input and pulls the same number of converted samples.
        Returns false when the output FIFO has run dry, in which case
        @p samples is untouched and the caller keeps its own signal. */
    bool process (float* samples, int numSamples);

private:
    void run() override;
    void processOneBlock();
    void runModel (const float* input, float* output, int numSamples);

    /** Interpolates the loaded identity vectors, then renormalises. Speaker
        embeddings live on a unit sphere; a plain average of two points on a
        sphere falls inside it, which reads as a washed-out, characterless
        voice. Pushing the result back out to the surface fixes that. */
    void buildBlendedEmbedding();

    double hostRate       = 48000.0;
    int    hopSize        = 5760;   // new audio per inference; sets latency
    int    contextSamples = 5760;   // past audio prepended for quality, free
    int    modelWindow    = 11520;  // context + hop, what the model actually sees
    int    fadeSamples    = 240;    // 5 ms seam between consecutive outputs
    int    tailTrim       = 960;    // discard the encoder's ragged tail
    int    latencySamples = 6000;

    juce::AbstractFifo inputFifo  { 1 };
    juce::AbstractFifo outputFifo { 1 };
    std::vector<float> inputStore, outputStore;

    std::vector<float> history, workBuffer, modelOutput, prevTail, fadeCurve, emitBuffer;

    struct Reference
    {
        std::vector<float> embedding;
        juce::String       name;
        bool               loaded = false;
    };

    Reference          references[numReferenceSlots];
    std::vector<float> blendedEmbedding;

    std::atomic<bool>  enabled       { false };
    std::atomic<bool>  modelsLoaded  { false };
    std::atomic<float> morph         { 0.0f };
    std::atomic<float> pitchOffset   { 0.0f };
    std::atomic<float> inferenceLoad { 0.0f };
    std::atomic<bool>  embeddingDirty  { true };
    std::atomic<int>   blocksConverted  { 0 };

    juce::String          lastError;
    juce::CriticalSection errorLock;
    void reportError (const juce::String& message);

    juce::CriticalSection    modelLock;
    juce::AudioFormatManager formatManager;

    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeuralVoiceConverter)
};
