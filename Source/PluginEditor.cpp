#include "PluginEditor.h"

#include <array>
#include <cmath>
#include <initializer_list>
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
    styleRotary (aiAmountSlider, aiAmountLabel, "AMOUNT", Palette::tract);

    genderSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    genderSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    genderSlider.setColour (juce::Slider::trackColourId, Palette::source);
    addAndMakeVisible (genderSlider);

    genderLabel.setText ("MASCULINE  <-  GENDER  ->  FEMININE", juce::dontSendNotification);
    genderLabel.setJustificationType (juce::Justification::centred);
    genderLabel.setFont (monoFont (10.0f));
    genderLabel.setColour (juce::Label::textColourId, Palette::muted);
    addAndMakeVisible (genderLabel);

    // A bare slider position means nothing here. What the reader needs is the
    // two numbers it is actually driving.
    genderReadout.setJustificationType (juce::Justification::centred);
    genderReadout.setFont (monoFont (10.5f));
    genderReadout.setColour (juce::Label::textColourId, Palette::text);
    addAndMakeVisible (genderReadout);

    genderSlider.onValueChange = [this] { updateGenderReadout(); };

    // The morph slider is the whole Vocoflex idea in one control: identity is
    // a vector, so you can stand between two of them.
    morphSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    morphSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    morphSlider.setColour (juce::Slider::trackColourId, Palette::tract);
    addAndMakeVisible (morphSlider);

    morphLabel.setText ("A  <-  MORPH  ->  B", juce::dontSendNotification);
    morphLabel.setJustificationType (juce::Justification::centred);
    morphLabel.setFont (monoFont (10.0f));
    morphLabel.setColour (juce::Label::textColourId, Palette::muted);
    addAndMakeVisible (morphLabel);

    addAndMakeVisible (linkButton);
    addAndMakeVisible (aiButton);

    auto styleBox = [this] (juce::ComboBox& box, juce::Label& label,
                            const juce::String& name, const char* paramID)
    {
        // The items have to come from the parameter, not a duplicate list here.
        // A ComboBoxAttachment silently does nothing on an empty box, which is
        // exactly how these shipped blank.
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (
                processor.apvts.getParameter (paramID)))
            box.addItemList (choice->choices, 1);

        box.setColour (juce::ComboBox::backgroundColourId, Palette::panel);
        box.setColour (juce::ComboBox::textColourId,       Palette::text);
        box.setColour (juce::ComboBox::outlineColourId,    Palette::rule);
        box.setColour (juce::ComboBox::arrowColourId,      Palette::muted);
        addAndMakeVisible (box);

        label.setText (name, juce::dontSendNotification);
        label.setFont (monoFont (9.0f, true));
        label.setColour (juce::Label::textColourId, Palette::muted);
        label.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (label);
    };

    styleBox (fftBox, fftBoxLabel, "VOCODER WINDOW", ParamID::fftMode);
    styleBox (nnBox,  nnBoxLabel,  "NEURAL BLOCK",   ParamID::nnBlock);

    addAndMakeVisible (lightButton);

    modelsButton.onClick = [this] { chooseModelFolder(); };
    voiceAButton.onClick = [this] { chooseReferenceVoice (0); };
    voiceBButton.onClick = [this] { chooseReferenceVoice (1); };

    addAndMakeVisible (modelsButton);
    addAndMakeVisible (voiceAButton);
    addAndMakeVisible (voiceBButton);

    for (auto* label : { &statusLabel, &latencyLabel })
    {
        label->setFont (monoFont (10.5f));
        label->setColour (juce::Label::textColourId, Palette::muted);
        addAndMakeVisible (*label);
    }

    latencyLabel.setJustificationType (juce::Justification::centredRight);

    auto& state = processor.apvts;
    pitchAtt    = std::make_unique<SliderAttachment> (state, ParamID::pitch,    pitchSlider);
    formantAtt  = std::make_unique<SliderAttachment> (state, ParamID::formant,  formantSlider);
    genderAtt   = std::make_unique<SliderAttachment> (state, ParamID::gender,   genderSlider);
    detailAtt   = std::make_unique<SliderAttachment> (state, ParamID::detail,   detailSlider);
    gateAtt     = std::make_unique<SliderAttachment> (state, ParamID::gate,     gateSlider);
    mixAtt      = std::make_unique<SliderAttachment> (state, ParamID::mix,      mixSlider);
    outputAtt   = std::make_unique<SliderAttachment> (state, ParamID::output,   outputSlider);
    aiAmountAtt = std::make_unique<SliderAttachment> (state, ParamID::aiAmount, aiAmountSlider);
    morphAtt    = std::make_unique<SliderAttachment> (state, ParamID::morph,    morphSlider);
    linkAtt     = std::make_unique<ButtonAttachment> (state, ParamID::link,     linkButton);
    lightAtt    = std::make_unique<ButtonAttachment> (state, ParamID::nnLight,  lightButton);
    fftAtt      = std::make_unique<ComboAttachment>  (state, ParamID::fftMode,  fftBox);
    nnAtt       = std::make_unique<ComboAttachment>  (state, ParamID::nnBlock,  nnBox);
    aiAtt       = std::make_unique<ButtonAttachment> (state, ParamID::aiEnable, aiButton);

    updateGenderReadout();

    // A DSP-only binary should not offer buttons that quietly do nothing.
    if (! processor.getNeural().isBuiltWithOnnx())
        for (auto* c : std::initializer_list<juce::Component*> {
                 &aiButton, &lightButton, &nnBox, &modelsButton,
                 &voiceAButton, &voiceBButton, &morphSlider, &aiAmountSlider })
            c->setEnabled (false);

    setSize (780, 640);
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
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 15);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    addAndMakeVisible (slider);

    label.setText (name, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (monoFont (9.5f, true));
    label.setColour (juce::Label::textColourId, Palette::muted);
    addAndMakeVisible (label);
}

// ---------------------------------------------------------------------------
//  Loading
// ---------------------------------------------------------------------------

void VoiceMorphAudioProcessorEditor::chooseModelFolder()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Select the folder holding content.onnx, speaker.onnx and decoder.onnx",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory));

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();

        if (folder == juce::File())
            return;

        juce::String error;

        if (processor.loadNeuralModels (folder, error))
            modelsButton.setButtonText ("Models: " + folder.getFileName());
        else
            modelsButton.setButtonText ("Load models folder");
    });
}

void VoiceMorphAudioProcessorEditor::chooseReferenceVoice (int slot)
{
    chooser = std::make_unique<juce::FileChooser> (
        "Choose a recording of the voice to imitate",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (flags, [this, slot] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file == juce::File())
            return;

        juce::String error;
        processor.loadReferenceVoice (slot, file, error);
        refreshVoiceButtons();
    });
}

void VoiceMorphAudioProcessorEditor::updateGenderReadout()
{
    const auto value = static_cast<float> (genderSlider.getValue());

    const auto pitch   = value * GenderMacro::pitchRange;
    const auto formant = value * GenderMacro::formantRange;

    auto signed2 = [] (float v)
    {
        return juce::String (v >= 0.0f ? "+" : "") + juce::String (v, 2);
    };

    genderReadout.setText (signed2 (value) + "    pitch " + signed2 (pitch)
                               + " st    formant " + signed2 (formant) + " st",
                           juce::dontSendNotification);

    genderReadout.setColour (juce::Label::textColourId,
                             std::abs (value) < 0.005f ? Palette::muted : Palette::source);
}

void VoiceMorphAudioProcessorEditor::refreshVoiceButtons()
{
    auto& neural = processor.getNeural();

    auto describe = [&neural] (int slot, const char* letter)
    {
        const auto name = neural.getReferenceName (slot);
        return juce::String ("Voice ") + letter + ": " + (name.isEmpty() ? "empty" : name);
    };

    voiceAButton.setButtonText (describe (0, "A"));
    voiceBButton.setButtonText (describe (1, "B"));
}

// ---------------------------------------------------------------------------

void VoiceMorphAudioProcessorEditor::timerCallback()
{
    statusLabel.setText (processor.getStatusMessage(), juce::dontSendNotification);

    const auto latencyMs = 1000.0 * processor.getLatencySamples()
                         / juce::jmax (1.0, processor.getSampleRate());

    auto& neural = processor.getNeural();

    juce::String right = juce::String (latencyMs, 1) + " ms";

    bool stalled = false;

    const bool neuralOn = processor.apvts.getRawParameterValue (ParamID::aiEnable)->load() > 0.5f
                       && neural.isBuiltWithOnnx();

    if (neural.isReady() && neuralOn)
    {
        const int blocks = neural.getBlocksConverted();

        // A load figure alone cannot distinguish "busy" from "died three
        // minutes ago holding its last reading", so show the block count too.
        right += "   load " + juce::String (juce::roundToInt (neural.getInferenceLoad() * 100.0f)) + "%"
               + "   blk " + juce::String (blocks);

        stalled = (blocks == lastBlockCount);
        lastBlockCount = blocks;
    }
    else
    {
        // Leaving the last reading on screen made a switched-off stage look
        // like a running one.
        lastBlockCount = -1;
    }

    latencyLabel.setText (right, juce::dontSendNotification);

    const bool trouble = stalled
                      || neural.getInferenceLoad() > 0.95f
                      || neural.getLastError().isNotEmpty();

    latencyLabel.setColour (juce::Label::textColourId, trouble ? Palette::warn : Palette::muted);

    // Morphing only means anything with two voices to morph between.
    const bool bothLoaded = processor.getNeural().hasReferenceVoice (0)
                         && processor.getNeural().hasReferenceVoice (1);

    morphSlider.setEnabled (bothLoaded);
    morphLabel.setColour (juce::Label::textColourId, bothLoaded ? Palette::muted : Palette::rule);
}

void VoiceMorphAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Palette::ink);

    g.setColour (Palette::text);
    g.setFont (monoFont (15.0f, true));
    g.drawText ("VOICEMORPH", 20, 14, 240, 20, juce::Justification::centredLeft, false);

    g.setColour (Palette::muted);
    g.setFont (monoFont (9.5f));
    g.drawText ("source / filter voice transformer", 20, 32, 340, 14,
                juce::Justification::centredLeft, false);

    const auto left  = 20.0f;
    const auto right = static_cast<float> (getWidth() - 20);

    g.setColour (Palette::rule);
    g.drawHorizontalLine (topDividerY,    left, right);
    g.drawHorizontalLine (footerDividerY, left, right);

    g.setColour (Palette::muted);
    g.setFont (monoFont (9.5f, true));
    g.drawText ("NEURAL CONVERSION", 20, neuralHeaderY, 240, 14,
                juce::Justification::centredLeft, false);
}

void VoiceMorphAudioProcessorEditor::resized()
{
    auto placeRotary = [] (juce::Rectangle<int> cell, juce::Slider& s, juce::Label& l)
    {
        l.setBounds (cell.removeFromTop (12));
        s.setBounds (cell);
    };

    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (38);

    // --- Display and the two main axes -------------------------------------
    auto top = area.removeFromTop (178);
    display.setBounds (top.removeFromLeft (top.getWidth() - 220).reduced (0, 2));

    auto mainKnobs = top.reduced (12, 6);
    placeRotary (mainKnobs.removeFromLeft (mainKnobs.getWidth() / 2).reduced (4, 0), pitchSlider, pitchLabel);
    placeRotary (mainKnobs.reduced (4, 0), formantSlider, formantLabel);

    area.removeFromTop (14);

    // --- Gender macro -------------------------------------------------------
    auto macro = area.removeFromTop (62);
    genderLabel.setBounds (macro.removeFromTop (14));
    genderSlider.setBounds (macro.removeFromTop (26).reduced (60, 4));
    genderReadout.setBounds (macro.removeFromTop (16));

    area.removeFromTop (10);

    // --- Utility row --------------------------------------------------------
    auto utility = area.removeFromTop (78);

    auto rightColumn = utility.removeFromRight (196);
    linkButton.setBounds (rightColumn.removeFromTop (22).withTrimmedTop (2));
    rightColumn.removeFromTop (8);
    fftBoxLabel.setBounds (rightColumn.removeFromTop (12));
    fftBox.setBounds (rightColumn.removeFromTop (24));

    const std::array<std::pair<juce::Slider*, juce::Label*>, 4> utilityControls {{
        { &detailSlider, &detailLabel },
        { &gateSlider,   &gateLabel   },
        { &mixSlider,    &mixLabel    },
        { &outputSlider, &outputLabel }
    }};

    const int cellWidth = utility.getWidth() / 4;

    for (const auto& control : utilityControls)
        placeRotary (utility.removeFromLeft (cellWidth).reduced (6, 0), *control.first, *control.second);

    // --- Divider and neural section header ----------------------------------
    area.removeFromTop (16);
    topDividerY = area.getY();
    area.removeFromTop (10);
    neuralHeaderY = area.getY();
    area.removeFromTop (22);

    // --- Neural section -----------------------------------------------------
    auto neural = area.removeFromTop (132);

    auto amountCell = neural.removeFromRight (96);
    placeRotary (amountCell.removeFromTop (74).reduced (8, 0), aiAmountSlider, aiAmountLabel);

    neural.removeFromRight (12);

    auto enableRow = neural.removeFromTop (24);
    auto nnCell    = enableRow.removeFromRight (150);
    enableRow.removeFromRight (10);
    lightButton.setBounds (enableRow.removeFromRight (120).withTrimmedTop (2).withHeight (20));
    nnBoxLabel.setBounds (nnCell.removeFromLeft (86).withTrimmedTop (6));
    nnBox.setBounds (nnCell);
    aiButton.setBounds (enableRow.withTrimmedTop (2).withHeight (20));

    neural.removeFromTop (8);

    modelsButton.setBounds (neural.removeFromTop (26));
    neural.removeFromTop (8);

    auto voices = neural.removeFromTop (26);
    voiceAButton.setBounds (voices.removeFromLeft (voices.getWidth() / 2 - 5));
    voices.removeFromLeft (10);
    voiceBButton.setBounds (voices);

    neural.removeFromTop (10);
    morphLabel.setBounds (neural.removeFromTop (14));
    morphSlider.setBounds (neural.removeFromTop (18).reduced (40, 0));

    // --- Footer -------------------------------------------------------------
    auto footer = getLocalBounds().removeFromBottom (30).reduced (20, 8);
    footerDividerY = footer.getY() - 8;

    latencyLabel.setBounds (footer.removeFromRight (250));
    statusLabel.setBounds (footer);
}
