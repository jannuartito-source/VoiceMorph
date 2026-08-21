#include "PluginProcessor.h"
#include "PluginEditor.h"

VoiceMorphAudioProcessor::VoiceMorphAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "state", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout VoiceMorphAudioProcessor::createLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::pitch, 1 }, "Pitch",
        NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("st")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::formant, 1 }, "Formant",
        NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("st")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::gender, 1 }, "Gender",
        NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ParamID::link, 1 }, "Link formants to pitch", false));

    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { ParamID::detail, 1 }, "Formant detail", 20, 150, 70));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::gate, 1 }, "Gate",
        NormalisableRange<float> (-80.0f, -10.0f, 0.1f), -50.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::mix, 1 }, "Mix",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::output, 1 }, "Output",
        NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ParamID::aiEnable, 1 }, "Neural conversion", false));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::aiAmount, 1 }, "Neural amount",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::morph, 1 }, "Voice morph",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));

    // Latency is almost entirely a choice, not a constraint. Both of these
    // trade it against quality, and the honest place to make that trade is
    // the user's ears rather than a constant in the source.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamID::fftMode, 1 }, "Vocoder window",
        StringArray { "Fast (16 ms)", "Balanced (32 ms)", "Smooth (64 ms)" }, 1));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamID::nnBlock, 1 }, "Neural block",
        StringArray { "80 ms", "120 ms", "200 ms", "320 ms" }, 1));

    // Halves the context window. Less speech for the encoder to work with,
    // but a third off the inference bill and no change to latency.
    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ParamID::nnLight, 1 }, "CPU saver", false));

    return layout;
}

void VoiceMorphAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    gate.prepare (sampleRate);
    applyQualitySettings();

    const juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32> (samplesPerBlock), 1 };
    dryDelay.prepare (spec);
    dspDelay.prepare (spec);
    dryDelay.reset();
    dspDelay.reset();

    monoBuffer.setSize (1, samplesPerBlock);
    dryBuffer.setSize  (1, samplesPerBlock);
    aiBuffer.setSize   (1, samplesPerBlock);

    mixSmoothed.reset      (sampleRate, 0.02);
    outputSmoothed.reset   (sampleRate, 0.02);
    aiAmountSmoothed.reset (sampleRate, 0.05);

    updateLatency();
}

void VoiceMorphAudioProcessor::releaseResources()
{
    cancelPendingUpdate();
    neural.releaseResources();
    engine.reset();
}

void VoiceMorphAudioProcessor::applyQualitySettings()
{
    // FFT order 10/11/12 -> 16/32/64 ms of vocoder latency at 48 kHz. Smaller
    // windows resolve formants less precisely, which shows up as a slightly
    // rougher timbre rather than as anything obviously broken.
    static constexpr int orders[]   = { 10, 11, 12 };
    static constexpr int blockMs[]  = { 80, 120, 200, 320 };

    const int fftChoice = static_cast<int> (apvts.getRawParameterValue (ParamID::fftMode)->load());
    const int nnChoice  = static_cast<int> (apvts.getRawParameterValue (ParamID::nnBlock)->load());

    const double rate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;

    const int light = apvts.getRawParameterValue (ParamID::nnLight)->load() > 0.5f ? 1 : 0;

    engine.prepare (rate, orders[juce::jlimit (0, 2, fftChoice)], 4);
    neural.prepare (rate, blockMs[juce::jlimit (0, 3, nnChoice)], light ? 50 : 100);

    cachedFftMode = fftChoice;
    cachedNnBlock = nnChoice;
    cachedNnLight = light;
}

void VoiceMorphAudioProcessor::handleAsyncUpdate()
{
    if (reconfigurePending.exchange (false))
    {
        const juce::ScopedLock sl (getCallbackLock());
        applyQualitySettings();
    }

    updateLatency();
}

void VoiceMorphAudioProcessor::updateLatency()
{
    const bool aiOn = apvts.getRawParameterValue (ParamID::aiEnable)->load() > 0.5f;

    const int engineLatency = engine.getLatencySamples();
    const int neuralLatency = aiOn ? neural.getLatencySamples() : 0;

    dspDelay.setDelay (static_cast<float> (neuralLatency));
    dryDelay.setDelay (static_cast<float> (engineLatency + neuralLatency));

    setLatencySamples (engineLatency + neuralLatency);
    latencyNeedsUpdate.store (false);
}

bool VoiceMorphAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    const auto& in  = layouts.getMainInputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return in == out || in == juce::AudioChannelSet::mono();
}

void VoiceMorphAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numIn       = getTotalNumInputChannels();
    const int numOut      = getTotalNumOutputChannels();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    if (numSamples == 0 || numIn == 0)
        return;

    // --- Read parameters ----------------------------------------------------
    const float pitchParam   = apvts.getRawParameterValue (ParamID::pitch)->load();
    const float formantParam = apvts.getRawParameterValue (ParamID::formant)->load();
    const float genderParam  = apvts.getRawParameterValue (ParamID::gender)->load();
    const bool  linkParam    = apvts.getRawParameterValue (ParamID::link)->load() > 0.5f;
    const int   detailParam  = static_cast<int> (apvts.getRawParameterValue (ParamID::detail)->load());
    const float gateParam    = apvts.getRawParameterValue (ParamID::gate)->load();
    const float mixParam     = apvts.getRawParameterValue (ParamID::mix)->load();
    const float outParam     = apvts.getRawParameterValue (ParamID::output)->load();
    const bool  aiOn         = apvts.getRawParameterValue (ParamID::aiEnable)->load() > 0.5f;
    const float aiAmtParam   = apvts.getRawParameterValue (ParamID::aiAmount)->load();
    const float morphParam   = apvts.getRawParameterValue (ParamID::morph)->load();

    const int fftChoice = static_cast<int> (apvts.getRawParameterValue (ParamID::fftMode)->load());
    const int nnChoice  = static_cast<int> (apvts.getRawParameterValue (ParamID::nnBlock)->load());

    const int lightChoice = apvts.getRawParameterValue (ParamID::nnLight)->load() > 0.5f ? 1 : 0;

    if (fftChoice != cachedFftMode || nnChoice != cachedNnBlock || lightChoice != cachedNnLight)
    {
        reconfigurePending.store (true);
        triggerAsyncUpdate();
    }
    else if (getLatencySamples() != engine.getLatencySamples() + (aiOn ? neural.getLatencySamples() : 0))
    {
        triggerAsyncUpdate();
    }

    engine.setPitchSemitones   (pitchParam   + genderParam * GenderMacro::pitchRange);
    engine.setFormantSemitones (formantParam + genderParam * GenderMacro::formantRange);
    engine.setLinkFormantsToPitch (linkParam);
    engine.setEnvelopeDetail (detailParam);

    gate.setThresholdDb (gateParam);

    neural.setEnabled (aiOn);
    neural.setMorph (morphParam);
    neural.setPitchOffsetSemitones (pitchParam + genderParam * GenderMacro::pitchRange);

    mixSmoothed.setTargetValue (mixParam);
    outputSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outParam));
    aiAmountSmoothed.setTargetValue (aiOn ? aiAmtParam : 0.0f);

    // --- Sum to mono. A voice changer is a mono device; keeping two
    //     independently vocoded channels would only smear the image. --------
    monoBuffer.setSize (1, numSamples, false, false, true);
    auto* mono = monoBuffer.getWritePointer (0);

    monoBuffer.copyFrom (0, 0, buffer, 0, 0, numSamples);

    if (numIn > 1)
    {
        monoBuffer.addFrom (0, 0, buffer, 1, 0, numSamples);
        monoBuffer.applyGain (0.5f);
    }

    dryBuffer.setSize (1, numSamples, false, false, true);
    dryBuffer.copyFrom (0, 0, monoBuffer, 0, 0, numSamples);

    // --- Wet chain ----------------------------------------------------------
    gate.process (mono, numSamples);
    engine.process (mono, numSamples);

    aiBuffer.setSize (1, numSamples, false, false, true);
    auto* ai = aiBuffer.getWritePointer (0);
    std::copy (mono, mono + numSamples, ai);

    const bool aiProduced = aiOn && neural.process (ai, numSamples);

    // Align the vocoder path with the neural path so the blend is coherent.
    for (int i = 0; i < numSamples; ++i)
    {
        dspDelay.pushSample (0, mono[i]);
        mono[i] = dspDelay.popSample (0);
    }

    if (aiProduced)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float amount = aiAmountSmoothed.getNextValue();
            mono[i] += amount * (ai[i] - mono[i]);
        }
    }
    else
    {
        aiAmountSmoothed.skip (numSamples);
    }

    // --- Dry / wet and output trim -----------------------------------------
    const auto* dry = dryBuffer.getReadPointer (0);

    for (int i = 0; i < numSamples; ++i)
    {
        dryDelay.pushSample (0, dry[i]);
        const float delayedDry = dryDelay.popSample (0);

        const float m = mixSmoothed.getNextValue();
        mono[i] = (delayedDry + m * (mono[i] - delayedDry)) * outputSmoothed.getNextValue();
    }

    // --- Fan out ------------------------------------------------------------
    for (int ch = 0; ch < numOut; ++ch)
        buffer.copyFrom (ch, 0, monoBuffer, 0, 0, numSamples);
}

bool VoiceMorphAudioProcessor::loadNeuralModels (const juce::File& folder, juce::String& errorOut)
{
    const bool ok = neural.loadModels (folder, errorOut);

    statusMessage = ok ? "Models loaded. Now load a reference voice."
                       : "Load failed: " + errorOut;

    triggerAsyncUpdate();
    return ok;
}

bool VoiceMorphAudioProcessor::loadReferenceVoice (int slot, const juce::File& audioFile,
                                                   juce::String& errorOut)
{
    const bool ok = neural.loadReferenceVoice (slot, audioFile, errorOut);

    statusMessage = ok ? "Reference " + juce::String (slot == 0 ? "A" : "B") + ": "
                             + audioFile.getFileNameWithoutExtension()
                       : "Reference failed: " + errorOut;

    return ok;
}

juce::String VoiceMorphAudioProcessor::getStatusMessage() const
{
    if (! neural.isBuiltWithOnnx())
        return "DSP build. Neural stage not compiled in.";

    if (! neural.areModelsLoaded())
        return "No models loaded. Pitch and formant still work.";

    if (! neural.isReady())
        return "Models loaded. Load a reference voice to convert.";

    // A failure on the worker thread is otherwise silent: the plugin keeps
    // producing perfectly good vocoder output and nothing looks wrong.
    const auto error = neural.getLastError();

    if (error.isNotEmpty())
        return error;

    return statusMessage;
}

juce::AudioProcessorEditor* VoiceMorphAudioProcessor::createEditor()
{
    return new VoiceMorphAudioProcessorEditor (*this);
}

void VoiceMorphAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VoiceMorphAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoiceMorphAudioProcessor();
}
