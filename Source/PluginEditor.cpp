#include "PluginEditor.h"

#include <array>
#include <cmath>
#include <utility>

namespace
{
    constexpr float kDisplayLowHz  = 70.0f;
    constexpr float kDisplayHighHz = 11000.0f;
    constexpr float kDisplayRangeDb = 60.0f;

    juce::Font monoFont (float height, bool bold = false)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                              height,
                                              bold ? juce::Font::bold : juce::Font::plain));
    }
}

// ===========================================================================
//  Look and feel
// ===========================================================================

InstrumentLookAndFeel::InstrumentLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Palette::ink);
    setColour (juce::Label::textColourId,                 Palette::text);
    setColour (juce::Slider::textBoxTextColourId,         Palette::text);
    setColour (juce::Slider::textBoxBackgroundColourId,   juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
    setColour (juce::TextButton::buttonColourId,          Palette::panel);
    setColour (juce::TextButton::textColourOffId,         Palette::text);
    setColour (juce::ComboBox::outlineColourId,           Palette::rule);
}

juce::Font InstrumentLookAndFeel::getLabelFont (juce::Label& label)
{
    return monoFont (label.getFont().getHeight());
}

void InstrumentLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                              float sliderPos, float startAngle, float endAngle,
                                              juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (5.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle  = startAngle + sliderPos * (endAngle - startAngle);
    const auto thickness = 2.5f;

    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (Palette::rule);
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Bipolar parameters read from the centre; unipolar ones from the start.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float origin = bipolar ? 0.5f : 0.0f;
    const float originAngle = startAngle + origin * (endAngle - startAngle);

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                         juce::jmin (originAngle, angle), juce::jmax (originAngle, angle), true);
    g.setColour (accent);
    g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    juce::Path pointer;
    pointer.startNewSubPath (centre.x, centre.y - radius * 0.42f);
    pointer.lineTo (centre.x, centre.y - radius * 0.92f);
    pointer.applyTransform (juce::AffineTransform::rotation (angle, centre.x, centre.y));

    g.setColour (accent.withAlpha (0.9f));
    g.strokePath (pointer, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
}

void InstrumentLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                              float sliderPos, float, float,
                                              juce::Slider::SliderStyle, juce::Slider& slider)
{
    const auto accent = slider.findColour (juce::Slider::trackColourId);
    const float cy = static_cast<float> (y) + static_cast<float> (height) * 0.5f;

    g.setColour (Palette::rule);
    g.fillRect (juce::Rectangle<float> (static_cast<float> (x), cy - 1.0f,
                                        static_cast<float> (width), 2.0f));

    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float origin = bipolar ? static_cast<float> (x) + static_cast<float> (width) * 0.5f
                                 : static_cast<float> (x);

    g.setColour (accent);
    g.fillRect (juce::Rectangle<float> (juce::jmin (origin, sliderPos), cy - 1.5f,
                                        std::abs (sliderPos - origin), 3.0f));

    g.fillRoundedRectangle (sliderPos - 2.0f, cy - 8.0f, 4.0f, 16.0f, 1.5f);
}

void InstrumentLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                              bool shouldDrawAsHighlighted, bool)
{
    const auto bounds = button.getLocalBounds().toFloat();
    const auto box    = juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - 6.0f, 12.0f, 12.0f);

    g.setColour (button.getToggleState() ? Palette::tract : Palette::rule);
    g.drawRoundedRectangle (box, 2.0f, 1.4f);

    if (button.getToggleState())
    {
        g.setColour (Palette::tract);
        g.fillRoundedRectangle (box.reduced (3.5f), 1.0f);
    }

    g.setColour (shouldDrawAsHighlighted ? Palette::text : Palette::muted);
    g.setFont (monoFont (11.5f));
    g.drawText (button.getButtonText(),
                bounds.withTrimmedLeft (20.0f),
                juce::Justification::centredLeft, false);
}

// ===========================================================================
//  Envelope display
// ===========================================================================

EnvelopeDisplay::EnvelopeDisplay (VoiceMorphAudioProcessor& p)
    : processor (p)
{
    startTimerHz (30);
}

void EnvelopeDisplay::timerCallback()
{
    if (processor.getEngine().consumeSnapshotDirtyFlag())
    {
        const auto  numBins = processor.getEngine().getSnapshotBinCount();
        const auto  rate    = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;

        buildPath (processor.getEngine().getEnvelopeSnapshot(), numBins, rate, measuredPath);
        buildPath (processor.getEngine().getWarpedSnapshot(),   numBins, rate, warpedPath);

        repaint();
    }
}

void EnvelopeDisplay::buildPath (const std::vector<float>& data, int numBins,
                                 double sampleRate, juce::Path& path) const
{
    path.clear();

    if (numBins < 2 || static_cast<int> (data.size()) < numBins)
        return;

    const auto bounds     = getLocalBounds().toFloat().reduced (1.0f);
    const int  fftSize    = (numBins - 1) * 2;
    const auto freqPerBin = static_cast<float> (sampleRate) / static_cast<float> (fftSize);

    float peakDb = -200.0f;
    for (int k = 1; k < numBins; ++k)
        peakDb = juce::jmax (peakDb, juce::Decibels::gainToDecibels (data[static_cast<size_t> (k)], -200.0f));

    const auto logLow  = std::log (kDisplayLowHz);
    const auto logSpan = std::log (kDisplayHighHz) - logLow;

    bool started = false;

    for (int k = 1; k < numBins; ++k)
    {
        const float freq = static_cast<float> (k) * freqPerBin;

        if (freq < kDisplayLowHz || freq > kDisplayHighHz)
            continue;

        const float nx = (std::log (freq) - logLow) / logSpan;
        const float db = juce::Decibels::gainToDecibels (data[static_cast<size_t> (k)], -200.0f) - peakDb;
        const float ny = juce::jlimit (0.0f, 1.0f, 1.0f + db / kDisplayRangeDb);

        const float px = bounds.getX() + nx * bounds.getWidth();
        const float py = bounds.getBottom() - ny * bounds.getHeight();

        if (! started) { path.startNewSubPath (px, py); started = true; }
        else           { path.lineTo (px, py); }
    }
}

void EnvelopeDisplay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (Palette::panel);
    g.fillRoundedRectangle (bounds, 3.0f);

    // Decade grid, labelled. The frequency axis is the one thing the reader
    // needs to orient a formant curve.
    g.setFont (monoFont (9.0f));

    const auto logLow  = std::log (kDisplayLowHz);
    const auto logSpan = std::log (kDisplayHighHz) - logLow;

    for (float freq : { 100.0f, 250.0f, 500.0f, 1000.0f, 2500.0f, 5000.0f, 10000.0f })
    {
        const float nx = (std::log (freq) - logLow) / logSpan;
        const float px = bounds.getX() + nx * bounds.getWidth();

        g.setColour (Palette::rule.withAlpha (0.7f));
        g.drawVerticalLine (juce::roundToInt (px), bounds.getY() + 4.0f, bounds.getBottom() - 14.0f);

        g.setColour (Palette::muted);
        g.drawText (freq >= 1000.0f ? juce::String (freq / 1000.0f, 1) + "k"
                                    : juce::String (juce::roundToInt (freq)),
                    juce::Rectangle<float> (px - 20.0f, bounds.getBottom() - 13.0f, 40.0f, 12.0f),
                    juce::Justification::centred, false);
    }

    g.setColour (Palette::muted.withAlpha (0.55f));
    g.strokePath (measuredPath, juce::PathStrokeType (1.2f));

    g.setColour (Palette::tract);
    g.strokePath (warpedPath, juce::PathStrokeType (1.8f));

    g.setColour (Palette::muted);
    g.setFont (monoFont (9.5f));
    g.drawText ("measured tract", bounds.reduced (8.0f, 6.0f), juce::Justification::topLeft, false);
    g.setColour (Palette::tract);
    g.drawText ("shifted", bounds.reduced (8.0f, 6.0f), juce::Justification::topRight, false);

    g.setColour (Palette::rule);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
}

// ===========================================================================
//  Editor
// ===========================================================================

VoiceMorphAudioProcessorEditor::VoiceMorphAudioProcessorEditor (VoiceMorphAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), display (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (display);

    styleRotary (pitchSlider,   pitchLabel,   "PITCH",   Palette::source);
    styleRotary (formantSlider, formantLabel, "FORMANT", Palette::tract);
    styleRotary (detailSlider,  detailLabel,  "DETAIL",  Palette::muted);
    styleRotary (gateSlider,    gateLabel,    "GATE",    Palette::muted);
    styleRotary (mixSlider,     mixLabel,     "MIX",     Palette::muted);
    styleRotary (outputSlider,  outputLabel,  "OUTPUT",  Palette::muted);

    genderSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    genderSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    genderSlider.setColour (juce::Slider::trackColourId, Palette::source);
    addAndMakeVisible (genderSlider);

    genderLabel.setText ("MASCULINE  <-  GENDER  ->  FEMININE", juce::dontSendNotification);
    genderLabel.setJustificationType (juce::Justification::centred);
    genderLabel.setFont (monoFont (10.0f));
    genderLabel.setColour (juce::Label::textColourId, Palette::muted);
    addAndMakeVisible (genderLabel);

    styleRotary (aiAmountSlider,  aiAmountLabel,  "AMOUNT", Palette::tract);
    styleRotary (aiSpeakerSlider, aiSpeakerLabel, "VOICE",  Palette::tract);

    addAndMakeVisible (linkButton);
    addAndMakeVisible (aiButton);

    encoderButton.onClick = [this] { chooseModelFile (true); };
    decoderButton.onClick = [this] { chooseModelFile (false); };
    addAndMakeVisible (encoderButton);
    addAndMakeVisible (decoderButton);

    for (auto* label : { &statusLabel, &latencyLabel })
    {
        label->setFont (monoFont (10.5f));
        label->setColour (juce::Label::textColourId, Palette::muted);
        addAndMakeVisible (*label);
    }

    latencyLabel.setJustificationType (juce::Justification::centredRight);

    auto& state = processor.apvts;
    pitchAtt     = std::make_unique<SliderAttachment> (state, ParamID::pitch,     pitchSlider);
    formantAtt   = std::make_unique<SliderAttachment> (state, ParamID::formant,   formantSlider);
    genderAtt    = std::make_unique<SliderAttachment> (state, ParamID::gender,    genderSlider);
    detailAtt    = std::make_unique<SliderAttachment> (state, ParamID::detail,    detailSlider);
    gateAtt      = std::make_unique<SliderAttachment> (state, ParamID::gate,      gateSlider);
    mixAtt       = std::make_unique<SliderAttachment> (state, ParamID::mix,       mixSlider);
    outputAtt    = std::make_unique<SliderAttachment> (state, ParamID::output,    outputSlider);
    aiAmountAtt  = std::make_unique<SliderAttachment> (state, ParamID::aiAmount,  aiAmountSlider);
    aiSpeakerAtt = std::make_unique<SliderAttachment> (state, ParamID::aiSpeaker, aiSpeakerSlider);
    linkAtt      = std::make_unique<ButtonAttachment> (state, ParamID::link,      linkButton);
    aiAtt        = std::make_unique<ButtonAttachment> (state, ParamID::aiEnable,  aiButton);

    setSize (760, 470);
    startTimerHz (4);
}

VoiceMorphAudioProcessorEditor::~VoiceMorphAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void VoiceMorphAudioProcessorEditor::styleRotary (juce::Slider& slider, juce::Label& label,
                                                  const juce::String& name, juce::Colour accent)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 14);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    addAndMakeVisible (slider);

    label.setText (name, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (monoFont (9.5f, true));
    label.setColour (juce::Label::textColourId, Palette::muted);
    addAndMakeVisible (label);
}

void VoiceMorphAudioProcessorEditor::chooseModelFile (bool isEncoder)
{
    chooser = std::make_unique<juce::FileChooser> (
        isEncoder ? "Choose the content encoder (.onnx)" : "Choose the decoder (.onnx)",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.onnx");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (flags, [this, isEncoder] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file == juce::File())
            return;

        (isEncoder ? encoderFile : decoderFile) = file;

        (isEncoder ? encoderButton : decoderButton).setButtonText (file.getFileNameWithoutExtension());

        tryLoadModels();
    });
}

void VoiceMorphAudioProcessorEditor::tryLoadModels()
{
    if (! encoderFile.existsAsFile() || ! decoderFile.existsAsFile())
        return;

    juce::String error;
    processor.loadNeuralModel (encoderFile, decoderFile, error);
}

void VoiceMorphAudioProcessorEditor::timerCallback()
{
    statusLabel.setText (processor.getStatusMessage(), juce::dontSendNotification);

    const auto latencyMs = 1000.0 * processor.getLatencySamples()
                         / juce::jmax (1.0, processor.getSampleRate());

    juce::String right = juce::String (latencyMs, 1) + " ms latency";

    if (processor.getNeural().isModelLoaded())
        right += "   load " + juce::String (juce::roundToInt (processor.getNeural().getInferenceLoad() * 100.0f)) + "%";

    latencyLabel.setText (right, juce::dontSendNotification);

    const bool overloaded = processor.getNeural().getInferenceLoad() > 0.95f;
    latencyLabel.setColour (juce::Label::textColourId, overloaded ? Palette::warn : Palette::muted);
}

void VoiceMorphAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Palette::ink);

    g.setColour (Palette::text);
    g.setFont (monoFont (15.0f, true));
    g.drawText ("VOICEMORPH", 20, 14, 220, 20, juce::Justification::centredLeft, false);

    g.setColour (Palette::muted);
    g.setFont (monoFont (9.5f));
    g.drawText ("source / filter voice transformer", 20, 32, 320, 14,
                juce::Justification::centredLeft, false);

    g.setColour (Palette::rule);
    g.drawHorizontalLine (330, 20.0f, static_cast<float> (getWidth() - 20));
    g.drawHorizontalLine (getHeight() - 34, 20.0f, static_cast<float> (getWidth() - 20));

    g.setColour (Palette::muted);
    g.setFont (monoFont (9.5f, true));
    g.drawText ("NEURAL CONVERSION", 20, 338, 200, 14, juce::Justification::centredLeft, false);
}

void VoiceMorphAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (38);

    // --- Display and the two main axes -------------------------------------
    auto top = area.removeFromTop (170);
    display.setBounds (top.removeFromLeft (top.getWidth() - 210).reduced (0, 2));

    auto knobs = top.reduced (10, 0);
    auto placeRotary = [] (juce::Rectangle<int> cell, juce::Slider& s, juce::Label& l)
    {
        l.setBounds (cell.removeFromTop (12));
        s.setBounds (cell);
    };

    placeRotary (knobs.removeFromLeft (knobs.getWidth() / 2).reduced (4, 0), pitchSlider, pitchLabel);
    placeRotary (knobs.reduced (4, 0), formantSlider, formantLabel);

    area.removeFromTop (10);

    // --- Gender macro -------------------------------------------------------
    auto macro = area.removeFromTop (44);
    genderLabel.setBounds (macro.removeFromTop (14));
    genderSlider.setBounds (macro.reduced (60, 4));

    // --- Utility row --------------------------------------------------------
    auto utility = area.removeFromTop (74);
    linkButton.setBounds (utility.removeFromRight (190).withTrimmedTop (24).withHeight (20));

    const int cellWidth = utility.getWidth() / 4;

    const std::array<std::pair<juce::Slider*, juce::Label*>, 4> utilityControls {{
        { &detailSlider, &detailLabel },
        { &gateSlider,   &gateLabel   },
        { &mixSlider,    &mixLabel    },
        { &outputSlider, &outputLabel }
    }};

    for (const auto& control : utilityControls)
        placeRotary (utility.removeFromLeft (cellWidth).reduced (6, 0), *control.first, *control.second);

    area.removeFromTop (26);

    // --- Neural section -----------------------------------------------------
    auto ai = area.removeFromTop (78);

    auto aiKnobs = ai.removeFromRight (170);
    placeRotary (aiKnobs.removeFromLeft (85).reduced (6, 0), aiAmountSlider,  aiAmountLabel);
    placeRotary (aiKnobs.reduced (6, 0),                     aiSpeakerSlider, aiSpeakerLabel);

    auto controls = ai.reduced (0, 6);
    aiButton.setBounds (controls.removeFromTop (22));
    controls.removeFromTop (6);

    auto buttons = controls.removeFromTop (24);
    encoderButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 5));
    buttons.removeFromLeft (10);
    decoderButton.setBounds (buttons);

    // --- Footer -------------------------------------------------------------
    auto footer = getLocalBounds().removeFromBottom (28).reduced (20, 6);
    latencyLabel.setBounds (footer.removeFromRight (240));
    statusLabel.setBounds (footer);
}
