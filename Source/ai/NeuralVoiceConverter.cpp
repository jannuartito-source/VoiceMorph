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

    /** Sample rate the content encoder expects. ContentVec and HuBERT are
        both 16 kHz models; this is fixed by the checkpoints, not a choice. */
    constexpr double kModelSampleRate = 16000.0;

    /** Resamples with a Lagrange interpolator. Returns samples written. */
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
}

// ===========================================================================
//  Impl: everything that depends on ONNX Runtime lives behind this pointer so
//  the DSP-only build has no dependency on it at all.
// ===========================================================================
struct NeuralVoiceConverter::Impl
{
#if VM_ENABLE_ONNX
    Ort::Env env { ORT_LOGGING_LEVEL_WARNING, "VoiceMorph" };
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> encoder;
    std::unique_ptr<Ort::Session> decoder;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);

    // Tensor names vary between export scripts. These match the common RVC
    // ONNX exports; adjust them to whatever `netron` shows for your files.
    const char* encoderInputNames[1]  { "source" };
    const char* encoderOutputNames[1] { "embed" };
    const char* decoderInputNames[4]  { "phone", "pitchf", "ds", "rnd" };
    const char* decoderOutputNames[1] { "audio" };

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

void NeuralVoiceConverter::prepare (double newHostRate, int blockSizeMilliseconds)
{
    releaseResources();

    hostRate = newHostRate;

    const int requested = static_cast<int> (newHostRate * blockSizeMilliseconds / 1000.0);

    // Force an even block so the 50 % overlap lands on a whole sample.
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
    inputFifo.reset();
    outputFifo.reset();

    std::fill (history.begin(),      history.end(),      0.0f);
    std::fill (overlapAccum.begin(), overlapAccum.end(), 0.0f);
    std::fill (outputStore.begin(),  outputStore.end(),  0.0f);

    impl->downsampler.reset();
    impl->upsampler.reset();

    // Prime the output with one block of silence. The host compensates for it
    // via the reported latency, and it keeps the audio thread from starving
    // while the first inference is still running.
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

        if (size1 > 0) std::copy (samples,         samples + size1,             inputStore.begin() + start1);
        if (size2 > 0) std::copy (samples + size1, samples + size1 + size2,     inputStore.begin() + start2);

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
        if (inputFifo.getNumReady() >= hopSize
            && outputFifo.getFreeSpace() >= hopSize)
        {
            processOneBlock();
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

    // Assemble N samples: the previous hop plus a fresh hop.
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

    runModel (workBuffer.data(), modelOutput.data(), blockSize);

    // Hann overlap-add at 50 %: the window pair sums to unity.
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

    const auto elapsed  = juce::Time::getMillisecondCounterHiRes() - startTime;
    const auto budget   = 1000.0 * hopSize / hostRate;
    const auto measured = static_cast<float> (elapsed / juce::jmax (1.0, budget));

    inferenceLoad.store (0.9f * inferenceLoad.load() + 0.1f * measured);
}

void NeuralVoiceConverter::runModel (const float* input, float* output, int numSamples)
{
    if (! modelLoaded.load())
    {
        std::copy (input, input + numSamples, output);
        return;
    }

#if VM_ENABLE_ONNX
    const juce::ScopedLock sl (modelLock);

    if (impl->encoder == nullptr || impl->decoder == nullptr)
    {
        std::copy (input, input + numSamples, output);
        return;
    }

    try
    {
        // --- 1. Host rate -> 16 kHz -------------------------------------
        const int numModelIn = resample (impl->downsampler,
                                         input, numSamples,
                                         impl->modelRateIn.data(),
                                         static_cast<int> (impl->modelRateIn.size()),
                                         hostRate, kModelSampleRate);

        // --- 2. Content encoder -----------------------------------------
        const std::array<int64_t, 3> sourceShape { 1, 1, numModelIn };

        auto sourceTensor = Ort::Value::CreateTensor<float> (
            impl->memory, impl->modelRateIn.data(), static_cast<size_t> (numModelIn),
            sourceShape.data(), sourceShape.size());

        auto embedOut = impl->encoder->Run (Ort::RunOptions { nullptr },
                                            impl->encoderInputNames, &sourceTensor, 1,
                                            impl->encoderOutputNames, 1);

        auto  embedInfo  = embedOut.front().GetTensorTypeAndShapeInfo();
        auto  embedShape = embedInfo.GetShape();          // [1, T, D]
        auto* embedData  = embedOut.front().GetTensorMutableData<float>();

        const int64_t numFrames = embedShape.size() >= 2 ? embedShape[1] : 0;

        if (numFrames <= 0)
        {
            std::copy (input, input + numSamples, output);
            return;
        }

        // --- 3. f0 contour ----------------------------------------------
        // A real build should run an f0 estimator here -- RMVPE exports to
        // ONNX and is what RVC uses. A flat contour is a placeholder: it will
        // produce intelligible but monotone speech.
        const float f0Base = 220.0f * std::pow (2.0f, pitchOffset.load() / 12.0f);
        std::vector<float> pitchContour (static_cast<size_t> (numFrames), f0Base);

        std::vector<int64_t> speakerId { static_cast<int64_t> (targetSpeaker.load()) };
        std::vector<float>   randomNoise (static_cast<size_t> (numFrames) * 192, 0.0f);

        const std::array<int64_t, 3> embedInShape { 1, numFrames, embedShape.back() };
        const std::array<int64_t, 2> pitchShape   { 1, numFrames };
        const std::array<int64_t, 1> speakerShape { 1 };
        const std::array<int64_t, 3> noiseShape   { 1, 192, numFrames };

        std::array<Ort::Value, 4> decoderInputs {
            Ort::Value::CreateTensor<float> (impl->memory, embedData,
                                             static_cast<size_t> (numFrames * embedShape.back()),
                                             embedInShape.data(), embedInShape.size()),
            Ort::Value::CreateTensor<float> (impl->memory, pitchContour.data(), pitchContour.size(),
                                             pitchShape.data(), pitchShape.size()),
            Ort::Value::CreateTensor<int64_t> (impl->memory, speakerId.data(), speakerId.size(),
                                               speakerShape.data(), speakerShape.size()),
            Ort::Value::CreateTensor<float> (impl->memory, randomNoise.data(), randomNoise.size(),
                                             noiseShape.data(), noiseShape.size())
        };

        auto audioOut = impl->decoder->Run (Ort::RunOptions { nullptr },
                                            impl->decoderInputNames, decoderInputs.data(), 4,
                                            impl->decoderOutputNames, 1);

        auto  audioInfo  = audioOut.front().GetTensorTypeAndShapeInfo();
        auto* audioData  = audioOut.front().GetTensorMutableData<float>();
        const auto numModelOut = static_cast<int> (audioInfo.GetElementCount());

        // --- 4. Model rate -> host rate ---------------------------------
        // RVC generators usually emit 40 kHz or 48 kHz; the ratio is derived
        // from the actual sample count so it stays correct either way.
        const double producedRate = kModelSampleRate * static_cast<double> (numModelOut)
                                                     / juce::jmax (1.0, static_cast<double> (numModelIn));

        const int written = resample (impl->upsampler,
                                      audioData, numModelOut,
                                      output, numSamples,
                                      producedRate, hostRate);

        if (written < numSamples)
            std::fill (output + written, output + numSamples, 0.0f);
    }
    catch (const Ort::Exception& e)
    {
        juce::Logger::writeToLog ("VoiceMorph inference failed: " + juce::String (e.what()));
        std::copy (input, input + numSamples, output);
    }
#else
    std::copy (input, input + numSamples, output);
#endif
}

// ---------------------------------------------------------------------------
//  Model loading
// ---------------------------------------------------------------------------

bool NeuralVoiceConverter::loadModel (const juce::File& contentEncoder,
                                      const juce::File& decoder,
                                      juce::String& errorOut)
{
#if VM_ENABLE_ONNX
    if (! contentEncoder.existsAsFile())
    {
        errorOut = "Content encoder not found: " + contentEncoder.getFullPathName();
        return false;
    }

    if (! decoder.existsAsFile())
    {
        errorOut = "Decoder not found: " + decoder.getFullPathName();
        return false;
    }

    try
    {
        const juce::ScopedLock sl (modelLock);

       #if JUCE_WINDOWS
        auto encPath = contentEncoder.getFullPathName().toWideCharPointer();
        auto decPath = decoder.getFullPathName().toWideCharPointer();
       #else
        auto encPath = contentEncoder.getFullPathName().toRawUTF8();
        auto decPath = decoder.getFullPathName().toRawUTF8();
       #endif

        impl->encoder = std::make_unique<Ort::Session> (impl->env, encPath, impl->options);
        impl->decoder = std::make_unique<Ort::Session> (impl->env, decPath, impl->options);

        modelLoaded.store (true);
        errorOut.clear();
        return true;
    }
    catch (const Ort::Exception& e)
    {
        errorOut = juce::String (e.what());
        unloadModel();
        return false;
    }
#else
    juce::ignoreUnused (contentEncoder, decoder);
    errorOut = "This build has no neural stage. Reconfigure with -DVM_ENABLE_ONNX=ON.";
    return false;
#endif
}

void NeuralVoiceConverter::unloadModel()
{
    const juce::ScopedLock sl (modelLock);

    modelLoaded.store (false);

#if VM_ENABLE_ONNX
    impl->encoder.reset();
    impl->decoder.reset();
#endif
}
