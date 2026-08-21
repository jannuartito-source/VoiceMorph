#pragma once

#include "PluginProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>

/** Palette. Two accents, because the algorithm has exactly two independent
    axes: the excitation that carries pitch, and the filter that carries
    timbre. Amber is source, cyan is tract. Nothing else gets a colour. */
namespace Palette
{
    const juce::Colour ink      { 0xff10141c };
    const juce::Colour panel    { 0xff171d28 };
    const juce::Colour rule     { 0xff2a3444 };
    const juce::Colour text     { 0xffc9d3e0 };
    const juce::Colour muted    { 0xff6b7a90 };
    const juce::Colour source   { 0xffe8a33d };   // pitch axis
    const juce::Colour tract    { 0xff4fc3d9 };   // formant axis
    const juce::Colour warn     { 0xffd96b5a };
}

/** Thin-arc rotary. No bevels, no gradients: this is a measuring instrument. */
class InstrumentLookAndFeel : public juce::LookAndFeel_V4
{
public:
    InstrumentLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawAsHighlighted, bool shouldDrawAsDown) override;

    juce::Font getLabelFont (juce::Label&) override;
};

/** The signature element: the measured vocal-tract envelope drawn against the
    warped one, updating in real time. Turning the formant knob visibly
    stretches the curve, which is the clearest possible explanation of what
    separates this from a pitch shifter. */
class EnvelopeDisplay : public juce::Component,
                        private juce::Timer
{
public:
    explicit EnvelopeDisplay (VoiceMorphAudioProcessor&);

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void buildPath (const std::vector<float>& data, int numBins, double sampleRate, juce::Path&) const;

    VoiceMorphAudioProcessor& processor;
    juce::Path measuredPath, warpedPath;
};

class VoiceMorphAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
    explicit VoiceMorphAudioProcessorEditor (VoiceMorphAudioProcessor&);
    ~VoiceMorphAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void timerCallback() override;
    void styleRotary (juce::Slider&, juce::Label&, const juce::String& name, juce::Colour accent);
    void chooseModelFolder();
    void chooseReferenceVoice (int slot);
    void refreshVoiceButtons();

    VoiceMorphAudioProcessor& processor;
    InstrumentLookAndFeel     lookAndFeel;

    EnvelopeDisplay display;

    juce::Slider pitchSlider, formantSlider, genderSlider, detailSlider,
                 gateSlider, mixSlider, outputSlider, aiAmountSlider, morphSlider;

    juce::Label pitchLabel, formantLabel, genderLabel, detailLabel,
                gateLabel, mixLabel, outputLabel, aiAmountLabel, morphLabel;

    juce::ToggleButton linkButton { "Link formants to pitch" };
    juce::ToggleButton aiButton   { "Neural conversion" };

    juce::ComboBox fftBox, nnBox;
    juce::Label    fftBoxLabel, nnBoxLabel;

    juce::TextButton modelsButton { "Load models folder" };
    juce::TextButton voiceAButton { "Voice A: empty" };
    juce::TextButton voiceBButton { "Voice B: empty" };

    juce::Label statusLabel, latencyLabel;

    std::unique_ptr<juce::FileChooser> chooser;

    // Section geometry, computed in resized() and read by paint(). Hardcoding
    // these was how the neural header ended up drawn on top of a knob.
    int lastBlockCount = -1;

    int topDividerY    = 0;
    int neuralHeaderY  = 0;
    int footerDividerY = 0;

    std::unique_ptr<SliderAttachment> pitchAtt, formantAtt, genderAtt, detailAtt,
                                      gateAtt, mixAtt, outputAtt, aiAmountAtt, morphAtt;
    std::unique_ptr<ButtonAttachment> linkAtt, aiAtt;
    std::unique_ptr<ComboAttachment>  fftAtt, nnAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceMorphAudioProcessorEditor)
};
