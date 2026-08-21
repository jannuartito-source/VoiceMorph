#include "NeuralVoiceConverter.h"

#include <algorithm>
#include <array>
#include <cmath>

#if VM_ENABLE_ONNX
 #include <onnxruntime_cxx_api.h>
#endif

namespace
{
    constexpr float kTwoPi = 6.283185307179586476925286766559f;

    /** Content and speaker encoders in this family are all 16 kHz models.
        This is fixed by the published checkpoints, not a choice. */
    constexpr double kModelSampleRate = 16000.0;

    /** Cap on how much of a reference file gets analysed. Speaker embeddings
        converge after a few seconds; feeding a whole song wastes time and, if
        the file has more than one person in it, blurs the identity. */
    constexpr double kMaxReferenceSeconds = 15.0;

    int resample (juce::LagrangeInterpolator& interp,
                  const float* source, int numSource,
                  float* destination, int maxDestination,
                  double sourceRate, double destRate)
    {
        if (std::abs (sourceRate - destRate) < 1.0)
        {
            const int n = juce::jmin (numSource, maxDestination);
            std::copy (source, source + n, destination);
            return n;
        }

        const double speedRatio = sourceRate / destRate;
        const int    numOut     = juce::jmin (maxDestination,
                                              static_cast<int> (static_cast<double> (numSource) / speedRatio));

        interp.process (speedRatio, source, destination, numOut);
        return numOut;
    }

    void normalise (std::vector<float>& v)
    {
        double sumOfSquares = 0.0;
        for (auto x : v) sumOfSquares += static_cast<double> (x) * x;

        const auto norm = std::sqrt (sumOfSquares);
        if (norm < 1.0e-9) return;

        const auto scale = static_cast<float> (1.0 / norm);
        for (auto& x : v) x *= scale;
    }
}

// ===========================================================================

struct NeuralVoiceConverter::Impl
{
#if VM_ENABLE_ONNX
    Ort::Env env { ORT_LOGGING_LEVEL_WARNING, "VoiceMorph" };
    Ort::SessionOptions options;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);

    std::unique_ptr<Ort::Session> content;   // speech -> phonetic frames
    std::unique_ptr<Ort::Session> speaker;   // speech -> one identity vector
    std::unique_ptr<Ort::Session> decoder;   // frames + identity -> speech

    // Export scripts disagree on tensor names. Open your .onnx files in
    // netron.app, read the real names, and edit these four lines.
    const char* contentIn  [1] { "audio" };
    const char* contentOut [1] { "features" };
    const char* speakerIn  [1] { "audio" };
    const char* speakerOut [1] { "embedding" };
    const char* decoderIn  [2] { "features", "speaker" };
    const char* decoderOut [1] { "audio" };

    Impl()
    {
        options.SetIntraOpNumThreads (2);
        options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
    }
#endif

    std::vector<float> modelRateIn, modelRateOut;
    juce::LagrangeInterpolator downsampler, upsampler;
};

// ===========================================================================

NeuralVoiceConverter::NeuralVoiceConverter()
    : juce::Thread ("VoiceMorph inference"),
      impl (std::make_unique<Impl>())
{
    formatManager.registerBasicFormats();
}

NeuralVoiceConverter::~NeuralVoiceConverter()
{
    releaseResources();
}

bool NeuralVoiceConverter::isBuiltWithOnnx() const noexcept
{
#if VM_ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

bool NeuralVoiceConverter::isReady() const noexcept
{
    if (! modelsLoaded.load())
        return false;

    for (int i = 0; i < numReferenceSlots; ++i)
        if (references[i].loaded)
            return true;

    return false;
}

bool NeuralVoiceConverter::hasReferenceVoice (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, numReferenceSlots) && references[slot].loaded;
}

juce::String NeuralVoiceConverter::getReferenceName (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, numReferenceSlots))
        return {};

    return references[slot].loaded ? references[slot].name : juce::String();
}

// ---------------------------------------------------------------------------

void NeuralVoiceConverter::prepare (double newHostRate, int blockSizeMilliseconds)
{
    releaseResources();

    hostRate = newHostRate;

    const int requested = static_cast<int> (newHostRate * blockSizeMilliseconds / 1000.0);

    blockSize      = juce::jmax (256, requested + (requested & 1));
    hopSize        = blockSize / 2;
    latencySamples = blockSize;

    const int fifoCapacity = blockSize * 8;

    inputStore.assign  (static_cast<size_t> (fifoCapacity), 0.0f);
    outputStore.assign (static_cast<size_t> (fifoCapacity), 0.0f);
    inputFifo.setTotalSize  (fifoCapacity);
    outputFifo.setTotalSize (fifoCapacity);

    history.assign      (static_cast<size_t> (hopSize),   0.0f);
    workBuffer.assign   (static_cast<size_t> (blockSize), 0.0f);
    modelOutput.assign  (static_cast<size_t> (blockSize), 0.0f);
    overlapAccum.assign (static_cast<size_t> (blockSize), 0.0f);
    emitBuffer.assign   (static_cast<size_t> (hopSize),   0.0f);

    hannWindow.resize (static_cast<size_t> (blockSize));
    for (int n = 0; n < blockSize; ++n)
        hannWindow[static_cast<size_t> (n)] =
            0.5f * (1.0f - std::cos (kTwoPi * static_cast<float> (n) / static_cast<float> (blockSize)));

    const int modelBlock = static_cast<int> (std::ceil (blockSize * kModelSampleRate / hostRate)) + 64;
    impl->modelRateIn.assign  (static_cast<size_t> (modelBlock), 0.0f);
    impl->modelRateOut.assign (static_cast<size_t> (modelBlock), 0.0f);

    reset();
    startThread (juce::Thread::Priority::high);
}

void NeuralVoiceConverter::reset()
{
    blocksConverted.store (0);
    inferenceLoad.store (0.0f);

    {
        const juce::ScopedLock sl (errorLock);
        lastError.clear();
    }

    inputFifo.reset();
    outputFifo.reset();

    std::fill (history.begin(),      history.end(),      0.0f);
    std::fill (overlapAccum.begin(), overlapAccum.end(), 0.0f);
    std::fill (outputStore.begin(),  outputStore.end(),  0.0f);

    impl->downsampler.reset();
    impl->upsampler.reset();

    int start1, size1, start2, size2;
    outputFifo.prepareToWrite (latencySamples, start1, size1, start2, size2);

    if (size1 > 0) std::fill (outputStore.begin() + start1, outputStore.begin() + start1 + size1, 0.0f);
    if (size2 > 0) std::fill (outputStore.begin() + start2, outputStore.begin() + start2 + size2, 0.0f);

    outputFifo.finishedWrite (size1 + size2);
}

void NeuralVoiceConverter::releaseResources()
{
    signalThreadShouldExit();
    notify();
    stopThread (2000);
}

// ---------------------------------------------------------------------------
//  Audio thread
// ---------------------------------------------------------------------------

bool NeuralVoiceConverter::process (float* samples, int numSamples)
{
    if (! enabled.load())
        return false;

    {
        int start1, size1, start2, size2;
        inputFifo.prepareToWrite (numSamples, start1, size1, start2, size2);

        if (size1 > 0) std::copy (samples,         samples + size1,         inputStore.begin() + start1);
        if (size2 > 0) std::copy (samples + size1, samples + size1 + size2, inputStore.begin() + start2);

        inputFifo.finishedWrite (size1 + size2);
    }

    notify();

    if (outputFifo.getNumReady() < numSamples)
        return false;

    int start1, size1, start2, size2;
    outputFifo.prepareToRead (numSamples, start1, size1, start2, size2);

    if (size1 > 0) std::copy (outputStore.begin() + start1, outputStore.begin() + start1 + size1, samples);
    if (size2 > 0) std::copy (outputStore.begin() + start2, outputStore.begin() + start2 + size2, samples + size1);

    outputFifo.finishedRead (size1 + size2);
    return true;
}

// ---------------------------------------------------------------------------
//  Worker thread
// ---------------------------------------------------------------------------

void NeuralVoiceConverter::run()
{
    while (! threadShouldExit())
    {
        if (inputFifo.getNumReady() >= hopSize && outputFifo.getFreeSpace() >= hopSize)
        {
            // An exception escaping here would kill the worker outright. The
            // audio thread would then find the FIFO permanently dry, fall back
            // to the vocoder, and show no sign that anything had gone wrong.
            try
            {
                processOneBlock();
            }
            catch (const std::exception& e)
            {
                reportError ("Worker: " + juce::String (e.what()));
                wait (200);
            }
            catch (...)
            {
                reportError ("Worker: unknown exception");
                wait (200);
            }
        }
        else
        {
            wait (5);
        }
    }
}

void NeuralVoiceConverter::processOneBlock()
{
    const auto startTime = juce::Time::getMillisecondCounterHiRes();

    std::copy (history.begin(), history.end(), workBuffer.begin());

    {
        int start1, size1, start2, size2;
        inputFifo.prepareToRead (hopSize, start1, size1, start2, size2);

        auto* dest = workBuffer.data() + hopSize;

        if (size1 > 0) std::copy (inputStore.begin() + start1, inputStore.begin() + start1 + size1, dest);
        if (size2 > 0) std::copy (inputStore.begin() + start2, inputStore.begin() + start2 + size2, dest + size1);

        inputFifo.finishedRead (size1 + size2);
    }

    std::copy (workBuffer.begin() + hopSize, workBuffer.end(), history.begin());

    if (embeddingDirty.exchange (false))
        buildBlendedEmbedding();

    runModel (workBuffer.data(), modelOutput.data(), blockSize);

    for (int n = 0; n < blockSize; ++n)
        overlapAccum[static_cast<size_t> (n)] +=
            modelOutput[static_cast<size_t> (n)] * hannWindow[static_cast<size_t> (n)];

    std::copy (overlapAccum.begin(), overlapAccum.begin() + hopSize, emitBuffer.begin());
    std::copy (overlapAccum.begin() + hopSize, overlapAccum.end(), overlapAccum.begin());
    std::fill (overlapAccum.begin() + hopSize, overlapAccum.end(), 0.0f);

    {
        int start1, size1, start2, size2;
        outputFifo.prepareToWrite (hopSize, start1, size1, start2, size2);

        if (size1 > 0) std::copy (emitBuffer.begin(),         emitBuffer.begin() + size1,         outputStore.begin() + start1);
        if (size2 > 0) std::copy (emitBuffer.begin() + size1, emitBuffer.begin() + size1 + size2, outputStore.begin() + start2);

        outputFifo.finishedWrite (size1 + size2);
    }

    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - startTime;
    const auto budget  = 1000.0 * hopSize / hostRate;

    inferenceLoad.store (0.9f * inferenceLoad.load()
                       + 0.1f * static_cast<float> (elapsed / juce::jmax (1.0, budget)));

    blocksConverted.fetch_add (1);
}

// ---------------------------------------------------------------------------
//  Identity vectors
// ---------------------------------------------------------------------------

void NeuralVoiceConverter::buildBlendedEmbedding()
{
    const juce::ScopedLock sl (modelLock);

    const bool haveA = references[0].loaded;
    const bool haveB = references[1].loaded;

    if (! haveA && ! haveB)
    {
        blendedEmbedding.clear();
        return;
    }

    if (haveA && ! haveB) { blendedEmbedding = references[0].embedding; return; }
    if (haveB && ! haveA) { blendedEmbedding = references[1].embedding; return; }

    const auto& a = references[0].embedding;
    const auto& b = references[1].embedding;

    if (a.size() != b.size())
    {
        blendedEmbedding = a;
        return;
    }

    const float t = morph.load();

    blendedEmbedding.resize (a.size());
    for (size_t i = 0; i < a.size(); ++i)
        blendedEmbedding[i] = a[i] + t * (b[i] - a[i]);

    normalise (blendedEmbedding);
}

bool NeuralVoiceConverter::loadReferenceVoice (int slot, const juce::File& audioFile, juce::String& errorOut)
{
    if (! juce::isPositiveAndBelow (slot, numReferenceSlots))
    {
        errorOut = "Bad reference slot";
        return false;
    }

#if VM_ENABLE_ONNX
    if (! modelsLoaded.load())
    {
        errorOut = "Load the models before loading a reference voice.";
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (audioFile));

    if (reader == nullptr)
    {
        errorOut = "Could not read " + audioFile.getFileName();
        return false;
    }

    const auto numToRead = static_cast<int> (juce::jmin (reader->lengthInSamples,
                                                         static_cast<juce::int64> (reader->sampleRate * kMaxReferenceSeconds)));

    if (numToRead < static_cast<int> (reader->sampleRate))
    {
        errorOut = "Reference is too short. Aim for five seconds or more.";
        return false;
    }

    juce::AudioBuffer<float> fileBuffer (static_cast<int> (reader->numChannels), numToRead);
    reader->read (&fileBuffer, 0, numToRead, 0, true, true);

    // Sum to mono.
    std::vector<float> mono (static_cast<size_t> (numToRead), 0.0f);
    for (int ch = 0; ch < fileBuffer.getNumChannels(); ++ch)
    {
        const auto* src = fileBuffer.getReadPointer (ch);
        for (int i = 0; i < numToRead; ++i)
            mono[static_cast<size_t> (i)] += src[i];
    }

    if (fileBuffer.getNumChannels() > 1)
        for (auto& s : mono) s /= static_cast<float> (fileBuffer.getNumChannels());

    // Down to the model's rate.
    juce::LagrangeInterpolator interp;
    const int maxOut = static_cast<int> (numToRead * kModelSampleRate / reader->sampleRate) + 64;
    std::vector<float> atModelRate (static_cast<size_t> (maxOut), 0.0f);

    const int numModelSamples = resample (interp, mono.data(), numToRead,
                                          atModelRate.data(), maxOut,
                                          reader->sampleRate, kModelSampleRate);

    try
    {
        const juce::ScopedLock sl (modelLock);

        if (impl->speaker == nullptr)
        {
            errorOut = "Speaker encoder is not loaded.";
            return false;
        }

        const std::array<int64_t, 2> shape { 1, numModelSamples };

        auto input = Ort::Value::CreateTensor<float> (impl->memory, atModelRate.data(),
                                                      static_cast<size_t> (numModelSamples),
                                                      shape.data(), shape.size());

        auto out = impl->speaker->Run (Ort::RunOptions { nullptr },
                                       impl->speakerIn, &input, 1,
                                       impl->speakerOut, 1);

        const auto  count = out.front().GetTensorTypeAndShapeInfo().GetElementCount();
        const auto* data  = out.front().GetTensorData<float>();

        references[slot].embedding.assign (data, data + count);
        normalise (references[slot].embedding);
        references[slot].name   = audioFile.getFileNameWithoutExtension();
        references[slot].loaded = true;
    }
    catch (const Ort::Exception& e)
    {
        errorOut = juce::String (e.what());
        return false;
    }

    embeddingDirty.store (true);
    errorOut.clear();
    return true;
#else
    juce::ignoreUnused (audioFile);
    errorOut = "This build has no neural stage. Rebuild with the neural option turned on.";
    return false;
#endif
}

void NeuralVoiceConverter::clearReferenceVoice (int slot)
{
    if (! juce::isPositiveAndBelow (slot, numReferenceSlots))
        return;

    const juce::ScopedLock sl (modelLock);

    references[slot].embedding.clear();
    references[slot].name.clear();
    references[slot].loaded = false;

    embeddingDirty.store (true);
}

// ---------------------------------------------------------------------------
//  Inference
// ---------------------------------------------------------------------------

void NeuralVoiceConverter::runModel (const float* input, float* output, int numSamples)
{
    if (! isReady())
    {
        std::copy (input, input + numSamples, output);
        return;
    }

#if VM_ENABLE_ONNX
    const juce::ScopedLock sl (modelLock);

    if (impl->content == nullptr || impl->decoder == nullptr || blendedEmbedding.empty())
    {
        std::copy (input, input + numSamples, output);
        return;
    }

    try
    {
        // 1. Host rate down to the model's rate.
        const int numModelIn = resample (impl->downsampler, input, numSamples,
                                         impl->modelRateIn.data(),
                                         static_cast<int> (impl->modelRateIn.size()),
                                         hostRate, kModelSampleRate);

        // 2. Content: what was said, with identity removed.
        const std::array<int64_t, 2> audioShape { 1, numModelIn };

        auto audioTensor = Ort::Value::CreateTensor<float> (impl->memory,
                                                            impl->modelRateIn.data(),
                                                            static_cast<size_t> (numModelIn),
                                                            audioShape.data(), audioShape.size());

        auto contentOut = impl->content->Run (Ort::RunOptions { nullptr },
                                              impl->contentIn, &audioTensor, 1,
                                              impl->contentOut, 1);

        auto  contentShape = contentOut.front().GetTensorTypeAndShapeInfo().GetShape();
        auto* contentData  = contentOut.front().GetTensorMutableData<float>();
        const auto contentCount = contentOut.front().GetTensorTypeAndShapeInfo().GetElementCount();

        if (contentCount == 0)
        {
            std::copy (input, input + numSamples, output);
            return;
        }

        // 3. Decode content plus the blended identity vector.
        //    Rank matters: FreeVC's generator appends the trailing axis itself,
        //    so this stays 2-D. Sending [1, 256, 1] makes it 4-D inside and
        //    conv1d rejects it.
        const std::array<int64_t, 2> embShape { 1, static_cast<int64_t> (blendedEmbedding.size()) };

        std::array<Ort::Value, 2> decoderInputs {
            Ort::Value::CreateTensor<float> (impl->memory, contentData, contentCount,
                                             contentShape.data(), contentShape.size()),
            Ort::Value::CreateTensor<float> (impl->memory, blendedEmbedding.data(),
                                             blendedEmbedding.size(),
                                             embShape.data(), embShape.size())
        };

        auto audioOut = impl->decoder->Run (Ort::RunOptions { nullptr },
                                            impl->decoderIn, decoderInputs.data(), 2,
                                            impl->decoderOut, 1);

        auto* outData = audioOut.front().GetTensorMutableData<float>();
        const auto numModelOut = static_cast<int> (audioOut.front().GetTensorTypeAndShapeInfo().GetElementCount());

        // 4. Back up to the host rate. The generator's output rate is derived
        //    from the sample count rather than assumed, so 16k, 24k and 48k
        //    decoders all work without a setting.
        const double producedRate = kModelSampleRate * static_cast<double> (numModelOut)
                                                     / juce::jmax (1.0, static_cast<double> (numModelIn));

        const int written = resample (impl->upsampler, outData, numModelOut,
                                      output, numSamples, producedRate, hostRate);

        if (written < numSamples)
            std::fill (output + written, output + numSamples, 0.0f);
    }
    catch (const Ort::Exception& e)
    {
        reportError ("ONNX: " + juce::String (e.what()));
        std::copy (input, input + numSamples, output);
    }
    catch (const std::exception& e)
    {
        reportError ("Inference: " + juce::String (e.what()));
        std::copy (input, input + numSamples, output);
    }
    catch (...)
    {
        reportError ("Inference: unknown exception");
        std::copy (input, input + numSamples, output);
    }
#else
    std::copy (input, input + numSamples, output);
#endif
}

// ---------------------------------------------------------------------------
//  Model loading
// ---------------------------------------------------------------------------

bool NeuralVoiceConverter::loadModels (const juce::File& folder, juce::String& errorOut)
{
#if VM_ENABLE_ONNX
    const auto contentFile = folder.getChildFile ("content.onnx");
    const auto speakerFile = folder.getChildFile ("speaker.onnx");
    const auto decoderFile = folder.getChildFile ("decoder.onnx");

    for (const auto& f : { contentFile, speakerFile, decoderFile })
    {
        if (! f.existsAsFile())
        {
            errorOut = "Missing " + f.getFileName() + " in " + folder.getFileName();
            return false;
        }
    }

    try
    {
        const juce::ScopedLock sl (modelLock);

       #if JUCE_WINDOWS
        impl->content = std::make_unique<Ort::Session> (impl->env, contentFile.getFullPathName().toWideCharPointer(), impl->options);
        impl->speaker = std::make_unique<Ort::Session> (impl->env, speakerFile.getFullPathName().toWideCharPointer(), impl->options);
        impl->decoder = std::make_unique<Ort::Session> (impl->env, decoderFile.getFullPathName().toWideCharPointer(), impl->options);
       #else
        impl->content = std::make_unique<Ort::Session> (impl->env, contentFile.getFullPathName().toRawUTF8(), impl->options);
        impl->speaker = std::make_unique<Ort::Session> (impl->env, speakerFile.getFullPathName().toRawUTF8(), impl->options);
        impl->decoder = std::make_unique<Ort::Session> (impl->env, decoderFile.getFullPathName().toRawUTF8(), impl->options);
       #endif

        modelsLoaded.store (true);
        errorOut.clear();
        return true;
    }
    catch (const Ort::Exception& e)
    {
        errorOut = juce::String (e.what());
        unloadModels();
        return false;
    }
#else
    juce::ignoreUnused (folder);
    errorOut = "This build has no neural stage. Rebuild with the neural option turned on.";
    return false;
#endif
}

void NeuralVoiceConverter::reportError (const juce::String& message)
{
    {
        const juce::ScopedLock sl (errorLock);

        if (lastError == message)
            return;                       // do not spam an identical failure

        lastError = message;
    }

    juce::Logger::writeToLog ("VoiceMorph: " + message);
}

juce::String NeuralVoiceConverter::getLastError() const
{
    const juce::ScopedLock sl (errorLock);
    return lastError;
}

void NeuralVoiceConverter::unloadModels()
{
    const juce::ScopedLock sl (modelLock);

    modelsLoaded.store (false);

#if VM_ENABLE_ONNX
    impl->content.reset();
    impl->speaker.reset();
    impl->decoder.reset();
#endif
}
