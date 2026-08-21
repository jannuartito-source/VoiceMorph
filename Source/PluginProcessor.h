#pragma once

#include "ai/NeuralVoiceConverter.h"
#include "dsp/NoiseGate.h"
#include "dsp/PhaseVocoderEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

namespace ParamID
{
    inline constexpr const char* pitch      = "pitch";
    inline constexpr const char* formant    = "formant";
    inline constexpr const char* gender     = "gender";
    inline constexpr const char* link       = "link";
    inline constexpr const char* detail     = "detail";
    inline constexpr const char* gate       = "gate";
    inline constexpr const char* mix        = "mix";
    inline constexpr const char* output     = "output";
    inline constexpr const char* aiEnable   = "aiEnable";
    inline constexpr const char* aiAmount   = "aiAmount";
    inline constexpr const char* aiSpeaker  = "aiSpeaker";
}

class VoiceMorphAudioProcessor : public juce::AudioProcessor,
                                 private juce::AsyncUpdater
{
public:
    VoiceMorphAudioProcessor();
    ~VoiceMorphAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "VoiceMorph"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    PhaseVocoderEngine&   getEngine() noexcept { return engine; }
    NeuralVoiceConverter& getNeural() noexcept { return neural; }

    /** Loads a pair of ONNX graphs and updates the reported latency. */
    bool loadNeuralModel (const juce::File& encoder, const juce::File& decoder, juce::String& errorOut);

    juce::String getStatusMessage() const;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    /** setLatencySamples() calls back into the host, so it must never run on
        the audio thread. processBlock only raises a flag. */
    void handleAsyncUpdate() override;
    void updateLatency();

    std::atomic<bool> latencyNeedsUpdate { true };

    PhaseVocoderEngine   engine;
    NeuralVoiceConverter neural;
    NoiseGate            gate;

    juce::AudioBuffer<float>      monoBuffer, dryBuffer, aiBuffer;
    juce::dsp::DelayLine<float>   dryDelay { 96000 };  // aligns dry with the full chain
    juce::dsp::DelayLine<float>   dspDelay { 96000 };  // aligns the vocoder with the neural stage
    juce::SmoothedValue<float>    mixSmoothed, outputSmoothed, aiAmountSmoothed;

    juce::String statusMessage { "Ready" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceMorphAudioProcessor)
};
