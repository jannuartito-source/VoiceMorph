#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    /** How far the gender macro pushes each axis at full travel.

        These are not arbitrary. Adult male and female speaking f0 differ by
        roughly an octave in the extremes but more typically a fifth, and the
        vocal tract length difference works out near 15 %, which is about four
        semitones of formant shift. Pushing formants harder than pitch is what
        makes the macro read as "different person" rather than "same person,
        different note".
    */
    constexpr float kGenderPitchRange   = 7.0f;   // semitones
    constexpr float kGenderFormantRange = 4.0f;   // semitones
}

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

    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { ParamID::aiSpeaker, 1 }, "Target voice", 0, 63, 0));

    return layout;
}

void VoiceMorphAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // 2048 at 48 kHz is the usual compromise: enough frequency resolution to
    // separate formants from harmonics, short enough that consonants survive.
    engine.prepare (sampleRate, 11, 4);
    gate.prepare (sampleRate);
    neural.prepare (sampleRate, 200);

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

void VoiceMorphAudioProcessor::handleAsyncUpdate()
{
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
    const int   aiSpeaker    = static_cast<int> (apvts.getRawParameterValue (ParamID::aiSpeaker)->load());

    if (getLatencySamples() != engine.getLatencySamples() + (aiOn ? neural.getLatencySamples() : 0))
        triggerAsyncUpdate();

    engine.setPitchSemitones   (pitchParam   + genderParam * kGenderPitchRange);
    engine.setFormantSemitones (formantParam + genderParam * kGenderFormantRange);
    engine.setLinkFormantsToPitch (linkParam);
    engine.setEnvelopeDetail (detailParam);

    gate.setThresholdDb (gateParam);

    neural.setEnabled (aiOn);
    neural.setTargetSpeaker (aiSpeaker);
    neural.setPitchOffsetSemitones (pitchParam + genderParam * kGenderPitchRange);

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

bool VoiceMorphAudioProcessor::loadNeuralModel (const juce::File& encoder,
                                                const juce::File& decoder,
                                                juce::String& errorOut)
{
    const bool ok = neural.loadModel (encoder, decoder, errorOut);

    statusMessage = ok ? "Model loaded: " + decoder.getFileName()
                       : "Load failed: " + errorOut;

    updateLatency();
    return ok;
}

juce::String VoiceMorphAudioProcessor::getStatusMessage() const
{
    if (! neural.isBuiltWithOnnx())
        return "DSP build. Neural stage not compiled in.";

    if (! neural.isModelLoaded())
        return "No model loaded. Pitch and formant still work.";

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
