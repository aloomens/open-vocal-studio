#include "PluginEditor.h"
#include "OfflineRender.h"

using namespace juce;

namespace ui
{
//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (Slider::textBoxTextColourId, col::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::textBoxHighlightColourId, col::accent.withAlpha (0.4f));
    setColour (Label::textColourId, col::text);
    setColour (ComboBox::backgroundColourId, col::knob);
    setColour (ComboBox::outlineColourId, col::panelEdge);
    setColour (ComboBox::textColourId, col::text);
    setColour (ComboBox::arrowColourId, col::accent);
    setColour (PopupMenu::backgroundColourId, col::panel);
    setColour (PopupMenu::textColourId, col::text);
    setColour (PopupMenu::highlightedBackgroundColourId, col::accent.withAlpha (0.85f));
    setColour (PopupMenu::highlightedTextColourId, Colours::black);
    setColour (ToggleButton::textColourId, col::text);
    setColour (TextButton::buttonColourId, col::knob);
    setColour (TextButton::buttonOnColourId, col::accent);
    setColour (TextButton::textColourOffId, col::text);
    setColour (TextButton::textColourOnId, Colours::black);
    setColour (TooltipWindow::backgroundColourId, col::panel);
    setColour (TooltipWindow::textColourId, col::text);
}

Font LookAndFeel::getLabelFont (Label& l) { return Font (FontOptions (l.getFont().getHeight())); }
Font LookAndFeel::getTextButtonFont (TextButton& b, int h)
{
    // small buttons (the note row) get a smaller font so "C#" etc. fit
    return Font (FontOptions (b.getWidth() < 40 ? 11.0f : jmin (13.5f, (float) h * 0.55f), Font::bold));
}

void LookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                                    float startAngle, float endAngle, Slider& s)
{
    const auto bounds = Rectangle<int> (x, y, w, h).toFloat();
    const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 4.0f;
    if (radius < 6.0f) return;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const auto accent = s.findColour (Slider::rotarySliderFillColourId);
    const bool enabled = s.isEnabled();

    Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (col::panelEdge.brighter (0.1f));
    g.strokePath (track, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));

    float fromAngle = startAngle;
    if (s.getMinimum() < 0.0 && s.getMaximum() > 0.0)
        fromAngle = startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle);
    Path value;
    value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, jmin (fromAngle, angle), jmax (fromAngle, angle), true);
    g.setColour (enabled ? accent : col::dim);
    g.strokePath (value, PathStrokeType (3.5f, PathStrokeType::curved, PathStrokeType::rounded));

    const float r2 = radius * 0.74f;
    ColourGradient grad (col::knob.brighter (0.25f), centre.x, centre.y - r2, col::knob.darker (0.4f), centre.x, centre.y + r2, false);
    g.setGradientFill (grad);
    g.fillEllipse (centre.x - r2, centre.y - r2, r2 * 2.0f, r2 * 2.0f);
    g.setColour (Colours::black.withAlpha (0.5f));
    g.drawEllipse (centre.x - r2, centre.y - r2, r2 * 2.0f, r2 * 2.0f, 1.0f);

    Path p;
    p.addRoundedRectangle (-1.5f, -r2 + 3.0f, 3.0f, r2 * 0.55f, 1.5f);
    p.applyTransform (AffineTransform::rotation (angle).translated (centre));
    g.setColour (enabled ? col::text : col::dim);
    g.fillPath (p);
}

void LookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                    Slider::SliderStyle style, Slider& s)
{
    if (style == Slider::LinearBar || style == Slider::LinearHorizontal)
    {
        // horizontal progress bar (the standalone file position)
        const auto r = Rectangle<int> (x, y, w, h).toFloat();
        g.setColour (Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (s.findColour (Slider::trackColourId));
        g.fillRoundedRectangle (r.withRight (jlimit (r.getX(), r.getRight(), pos)), 3.0f);
        return;
    }
    // vertical fader for the EQ bands, filled from the 0 dB line
    const auto r = Rectangle<int> (x, y, w, h).toFloat();
    const float cx = r.getCentreX();
    g.setColour (Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (cx - 2.5f, r.getY(), 5.0f, r.getHeight(), 2.5f);
    const float zero = r.getY() + r.getHeight() * (1.0f - (float) s.valueToProportionOfLength (0.0));
    g.setColour (s.findColour (Slider::trackColourId));
    g.fillRoundedRectangle (cx - 2.5f, jmin (zero, pos), 5.0f, std::abs (zero - pos), 2.5f);
    g.setColour (col::text);
    g.fillRoundedRectangle (cx - 8.0f, pos - 3.0f, 16.0f, 6.0f, 2.0f);
}

void LookAndFeel::drawToggleButton (Graphics& g, ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const auto accent = b.findColour (ToggleButton::tickColourId);
    g.setColour (on ? accent.withAlpha (0.9f) : col::knob.brighter (highlighted ? 0.15f : 0.0f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (on ? accent.brighter (0.3f) : col::panelEdge);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
    g.setColour (on ? Colours::black : col::text);
    g.setFont (FontOptions (12.5f, Font::bold));
    g.drawText (b.getButtonText(), r, Justification::centred);
}

void LookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour& bgc, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    auto c = b.getToggleState() ? b.findColour (TextButton::buttonOnColourId) : bgc;
    if (highlighted) c = c.brighter (0.12f);
    if (down) c = c.darker (0.1f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (col::panelEdge.brighter (0.2f));
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

//==============================================================================
Knob::Knob (AudioProcessorValueTreeState& s, const String& id, const String& name, Colour c)
{
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 76, 16);
    slider.setColour (Slider::rotarySliderFillColourId, c);
    slider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);    // plain value text, no box
    slider.setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.2f, MathConstants<float>::pi * 2.8f, true);
    addAndMakeVisible (slider);
    label.setText (name, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (13.0f, Font::bold));
    addAndMakeVisible (label);
    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);
    if (auto* p = s.getParameter (id))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
}

void Knob::resized()
{
    auto r = getLocalBounds();
    const int sliderH = jmin (r.getHeight() - 18, r.getWidth() + 16);
    auto block = r.withSizeKeepingCentre (r.getWidth(), sliderH + 18);
    label.setBounds (block.removeFromTop (18));
    slider.setBounds (block);
}

Choice::Choice (AudioProcessorValueTreeState& s, const String& id, const String& name)
{
    if (auto* p = dynamic_cast<AudioParameterChoice*> (s.getParameter (id))) box.addItemList (p->choices, 1);
    box.setJustificationType (Justification::centred);
    addAndMakeVisible (box);
    label.setText (name, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (13.0f, Font::bold));
    addAndMakeVisible (label);
    attachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (s, id, box);
}

void Choice::resized()
{
    auto r = getLocalBounds();
    if (label.getText().isEmpty()) { label.setVisible (false); box.setBounds (r); return; }
    label.setBounds (r.removeFromTop (18));
    box.setBounds (r.removeFromTop (26));
}

Switch::Switch (AudioProcessorValueTreeState& s, const String& id, const String& name, Colour c)
{
    label.setText (name, dontSendNotification);
    label.setJustificationType (Justification::centred);
    label.setFont (FontOptions (13.0f, Font::bold));
    addAndMakeVisible (label);
    auto* p = dynamic_cast<AudioParameterChoice*> (s.getParameter (id));
    jassert (p != nullptr);
    if (p == nullptr) return;
    for (int i = 0; i < p->choices.size(); ++i)
    {
        auto b = std::make_unique<TextButton> (p->choices[i].toUpperCase());
        b->setColour (TextButton::buttonOnColourId, p->choices[i] == "Off" ? col::dim : c);
        addAndMakeVisible (*b);
        buttons.push_back (std::move (b));
    }
    attach (*p);
}

Switch::Switch (AudioProcessorValueTreeState& s, const String& id, const StringArray& labels, const Array<Colour>& onColours)
{
    row = true;
    auto* p = s.getParameter (id);
    jassert (p != nullptr);
    if (p == nullptr) return;
    for (int i = 0; i < labels.size(); ++i)
    {
        auto b = std::make_unique<TextButton> (labels[i]);
        b->setColour (TextButton::buttonOnColourId, onColours[i]);
        addAndMakeVisible (*b);
        buttons.push_back (std::move (b));
    }
    attach (*p);
}

void Switch::attach (RangedAudioParameter& p)
{
    for (size_t i = 0; i < buttons.size(); ++i)
        buttons[i]->onClick = [this, i] { if (attachment != nullptr) attachment->setValueAsCompleteGesture ((float) i); };
    attachment = std::make_unique<ParameterAttachment> (p, [this] (float v)
    {
        current = roundToInt (v);
        for (size_t i = 0; i < buttons.size(); ++i) buttons[i]->setToggleState ((int) i == current, dontSendNotification);
    }, nullptr);
    attachment->sendInitialUpdate();
}

void Switch::resized()
{
    auto r = getLocalBounds();
    if (row)
    {
        const int w = (r.getWidth() - 4 * ((int) buttons.size() - 1)) / jmax (1, (int) buttons.size());
        for (auto& b : buttons) { b->setBounds (r.removeFromLeft (w)); r.removeFromLeft (4); }
        return;
    }
    label.setBounds (r.removeFromTop (20));
    for (auto& b : buttons) { b->setBounds (r.removeFromTop (26)); r.removeFromTop (4); }
}

Toggle::Toggle (AudioProcessorValueTreeState& s, const String& id, const String& name, Colour c)
{
    button.setButtonText (name);
    button.setClickingTogglesState (true);
    button.setColour (ToggleButton::tickColourId, c);
    addAndMakeVisible (button);
    attachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, id, button);
}

void Meter::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto titleArea = r.removeFromBottom (16.0f);
    g.setColour (col::dim);
    g.setFont (FontOptions (10.0f, Font::bold));
    g.drawText (title, titleArea, Justification::centred, false);
    auto bar = r.reduced (jmax (2.0f, (r.getWidth() - 12.0f) * 0.5f), 2.0f);
    g.setColour (Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (bar, 3.0f);
    if (kind == reduction)
    {
        const float frac = jlimit (0.0f, 1.0f, -value / 18.0f);
        g.setColour (col::dyn);
        g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * frac), 3.0f);
    }
    else if (kind == bipolar)
    {
        const float mid = bar.getCentreY();
        const float frac = jlimit (-1.0f, 1.0f, value / 12.0f);
        g.setColour (col::dyn);
        if (frac > 0) g.fillRect (bar.getX(), mid - frac * bar.getHeight() * 0.5f, bar.getWidth(), frac * bar.getHeight() * 0.5f);
        else g.fillRect (bar.getX(), mid, bar.getWidth(), -frac * bar.getHeight() * 0.5f);
        g.setColour (col::dim);
        g.drawHorizontalLine ((int) mid, bar.getX() - 2, bar.getRight() + 2);
    }
    else
    {
        const float frac = jlimit (0.0f, 1.0f, (value + 60.0f) / 60.0f);
        g.setColour (value > -1.0f ? col::hot : value > -12.0f ? col::accent : col::good);
        g.fillRoundedRectangle (bar.withTop (bar.getBottom() - bar.getHeight() * frac), 3.0f);
    }
}

void EqDisplay::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (r, 4.0f);
    const float fLo = 40.0f, fHi = 18000.0f, dbRange = 15.0f;
    auto xOf = [&] (float f) { return r.getX() + r.getWidth() * std::log (f / fLo) / std::log (fHi / fLo); };
    auto yOf = [&] (float db) { return r.getCentreY() - db / dbRange * r.getHeight() * 0.5f; };
    g.setFont (FontOptions (10.0f));
    for (float f : { 100.0f, 1000.0f, 10000.0f })
    {
        g.setColour (col::panelEdge);
        g.drawVerticalLine ((int) xOf (f), r.getY(), r.getBottom());
        g.setColour (col::dim);
        g.drawText (f >= 1000 ? String ((int) (f / 1000)) + "k" : String ((int) f), Rectangle<float> (xOf (f) + 2, r.getBottom() - 13, 30, 12), Justification::left);
    }
    g.setColour (col::panelEdge);
    g.drawHorizontalLine ((int) yOf (0), r.getX(), r.getRight());

    // live reduction from the spectral stage (resonances, smoothing, de-essing)
    {
        Path p;
        const int n = vs::SpectralStage::kDisplayPoints;
        for (int i = 0; i < n; ++i)
        {
            const float f = 50.0f * std::pow (16000.0f / 50.0f, i / (float) (n - 1));
            const float db = jmax (-dbRange, proc.spectral.displayGr[(size_t) i].load());
            const float x = xOf (jmax (f, fLo)), y = yOf (db);
            if (i == 0) p.startNewSubPath (x, yOf (0)), p.lineTo (x, y); else p.lineTo (x, y);
        }
        p.lineTo (xOf (16000.0f), yOf (0));
        p.closeSubPath();
        g.setColour (col::dyn.withAlpha (0.35f));
        g.fillPath (p);
    }
    // EQ curve (tone EQ x amount, plus Air)
    {
        std::array<float, vs::VocalEq::kBands> gains {};
        const float amt = proc.apvts.getRawParameterValue (IDs::eqAmount)->load() * 0.01f;
        for (int b = 0; b < vs::VocalEq::kBands; ++b) gains[(size_t) b] = proc.apvts.getRawParameterValue (IDs::eq (b))->load() * amt;
        const double air = proc.apvts.getRawParameterValue (IDs::air)->load();
        Path p;
        for (int i = 0; i <= 120; ++i)
        {
            const float f = fLo * std::pow (fHi / fLo, i / 120.0f);
            const float db = (float) vs::VocalEq::responseDb (48000.0, gains, air, f);
            const float x = xOf (f), y = yOf (jlimit (-dbRange, dbRange, db));
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (col::accent);
        g.strokePath (p, PathStrokeType (2.0f));
    }
}
} // namespace ui

//==============================================================================
VocalStudioAudioProcessorEditor::VocalStudioAudioProcessorEditor (VocalStudioAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      mode (p.apvts, IDs::mode, ""),
      compare (p.apvts, IDs::compare, StringArray { "PROCESSED", "ORIGINAL" }, Array<Colour> { ui::col::good, ui::col::dyn }),
      denoise (p.apvts, IDs::denoise, "Denoise"),
      deroom (p.apvts, IDs::deroom, "De-room"),
      deplosive (p.apvts, IDs::deplosive, "De-plosive"),
      rumble (p.apvts, IDs::rumble, "Rumble Cut"),
      inGain (p.apvts, IDs::inGain, "Input"),
      pitchOn (p.apvts, IDs::pitchOn, "ON", ui::col::pitch),
      strength (p.apvts, IDs::pitchStrength, "Strength", ui::col::pitch),
      speed (p.apvts, IDs::pitchSpeed, "Speed", ui::col::pitch),
      tolerance (p.apvts, IDs::pitchTolerance, "Tolerance", ui::col::pitch),
      key (p.apvts, IDs::key, "Key"),
      scale (p.apvts, IDs::scale, "Scale"),
      resonance (p.apvts, IDs::resonance, "Resonance"),
      smooth (p.apvts, IDs::smooth, "Smooth"),
      deess (p.apvts, IDs::deess, "De-ess"),
      breath (p.apvts, IDs::breath, "Breath"),
      consonant (p.apvts, IDs::consonant, "Consonant"),
      eqDisplay (p),
      eqAmount (p.apvts, IDs::eqAmount, "EQ Amount"),
      air (p.apvts, IDs::air, "Air"),
      rider (p.apvts, IDs::rider, "Rider", ui::col::dyn),
      compAmt (p.apvts, IDs::comp, "Compress", ui::col::dyn),
      outGain (p.apvts, IDs::outGain, "Output"),
      revType (p.apvts, IDs::revType, "Reverb", ui::col::space),
      revMix (p.apvts, IDs::revMix, "Mix", ui::col::space),
      revDecay (p.apvts, IDs::revDecay, "Decay", ui::col::space),
      revPreDelay (p.apvts, IDs::revPreDelay, "Pre-delay", ui::col::space),
      revDuck (p.apvts, IDs::revDuck, "Duck", ui::col::space),
      dlyOn (p.apvts, IDs::dlyOn, "DELAY", ui::col::space),
      dlySync (p.apvts, IDs::dlySync, "SYNC", ui::col::space),
      dlyTime (p.apvts, IDs::dlyTime, "Time", ui::col::space),
      dlyNote (p.apvts, IDs::dlyNote, "Note"),
      dlyFeedback (p.apvts, IDs::dlyFeedback, "Feedback", ui::col::space),
      dlyMix (p.apvts, IDs::dlyMix, "Mix", ui::col::space),
      dlyDuck (p.apvts, IDs::dlyDuck, "Duck", ui::col::space),
      dlyWidth (p.apvts, IDs::dlyWidth, "Width", ui::col::space),
      doubler (p.apvts, IDs::doubler, "Doubler", ui::col::space)
{
    setLookAndFeel (&lnf);
    fileUi = proc.isStandaloneApp();
    baseH = kBaseH + (fileUi ? kFileStripH : 0);
    for (Component* c : std::initializer_list<Component*> {
             &mode, &compare, &denoise, &deroom, &deplosive, &rumble, &inGain, &pitchOn, &strength, &speed, &tolerance,
             &key, &scale, &resonance, &smooth, &deess, &breath, &consonant, &eqDisplay, &eqAmount, &air, &rider, &compAmt,
             &riderMeter, &compMeter, &deessMeter, &plosiveMeter, &consonantMeter, &outGain, &inMeter, &outMeter, &limMeter,
             &revType, &revMix, &revDecay, &revPreDelay, &revDuck, &dlyOn, &dlySync, &dlyTime, &dlyNote,
             &dlyFeedback, &dlyMix, &dlyDuck, &dlyWidth, &doubler, &presetBox, &analyzeButton, &statusLabel,
             &infoLabel, &useKeyButton, &detectedLabel, &pitchReadout, &eqFlatButton })
        body.addAndMakeVisible (c);

    // tooltips: what each control does, in a sentence
    auto tip = [] (ui::Knob& k, const char* t) { k.slider.setTooltip (t); };
    tip (denoise, "Removes noise, hum, hiss and room/headphone bleed around the voice (neural in Mix mode). Double-click any knob to reset it.");
    tip (deroom, "Pulls back the sound of the room (reflections and reverb tail) recorded with the voice.");
    tip (deplosive, "Turns down the low thump of P and B sounds, only while it happens.");
    tip (rumble, "High-pass filter for rumble, handling noise and proximity boom (24 dB/oct).");
    tip (strength, "How far off-pitch notes are pulled toward the nearest allowed note. Vibrato and slides stay as sung.");
    tip (speed, "How quickly correction follows the note. Low = only the long-term intonation (most natural).");
    tip (tolerance, "Deviations smaller than this are left alone.");
    tip (resonance, "Finds narrow ringing or harsh resonances and turns them down while they ring.");
    tip (smooth, "Softens a harsh 2-5 kHz edge when it pokes out.");
    tip (deess, "Turns down sibilance (S, SH, T) without dulling the rest of the voice.");
    tip (breath, "Turns down breaths between phrases.");
    tip (consonant, "Softens the click of hard consonants (K, T, hard C) for just that moment. Vowels and sibilance are left to the other controls.");
    tip (eqAmount, "How much of the tone EQ is applied. ANALYZE sets the EQ bands to match a polished vocal curve.");
    tip (air, "High shelf above 12 kHz for openness.");
    tip (rider, "Evens out the level between phrases (the range it may ride, in dB) before the compressor.");
    tip (compAmt, "Program-dependent vocal compression; the threshold follows the voice's level automatically.");
    tip (doubler, "Adds a subtle double-tracked width on the sides.");
    {
        const char* revTips[] = { "No reverb.", "Plate: bright and dense, no early reflections.", "Room: shorter and darker, with early reflections." };
        for (size_t i = 0; i < revType.buttons.size() && i < 3; ++i) revType.buttons[i]->setTooltip (revTips[i]);
    }
    analyzeButton.setTooltip ("Play 10-30 seconds of the vocal, then click again: sets the tone EQ and detects the key.");
    compare.buttons[0]->setTooltip ("You hear Open Vocal Studio's processing.");
    compare.buttons[1]->setTooltip ("You hear the untouched input (the original), level-matched to the processed sound, to judge the processing honestly.");
    mode.box.setTooltip ("Mix: highest quality (about 170 ms latency, compensated by the DAW). Track: low latency for recording.");

    // presets
    for (int i = 0; i < proc.getNumPrograms(); ++i) presetBox.addItem (proc.getProgramName (i), i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };

    analyzeButton.setClickingTogglesState (true);
    analyzeButton.onClick = [this]
    {
        if (analyzeButton.getToggleState()) { proc.startAnalyze(); setStatus ("Analyzing... play the vocal, then click ANALYZE again.", ui::col::accent, 600); }
        else setStatus (proc.stopAnalyzeAndApply(), ui::col::good, 12);
    };
    useKeyButton.onClick = [this]
    {
        if (proc.applyDetectedKey())
        {
            int k; bool minor; float conf;
            proc.detectedKey (k, minor, conf);
            setStatus (String ("Key set to ") + vs::Scales::noteName (k) + (minor ? " minor" : " major") + ".", ui::col::good, 6);
        }
        else setStatus ("No key detected yet - sing a little longer.", ui::col::accent, 6);
    };
    useKeyButton.setTooltip ("Copies the detected key into Key and Scale (once). Available when a key has been detected.");
    if (fileUi) setUpFileStrip();
    eqFlatButton.onClick = [this]
    {
        for (int b = 0; b < vs::VocalEq::kBands; ++b)
            if (auto* prm = proc.apvts.getParameter (IDs::eq (b))) prm->setValueNotifyingHost (prm->convertTo0to1 (0.0f));
    };

    // pitch note buttons (piano layout colors)
    for (int pc = 0; pc < 12; ++pc)
    {
        auto b = std::make_unique<TextButton> (vs::Scales::noteName (pc));
        b->setClickingTogglesState (true);
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        b->setColour (TextButton::buttonColourId, black ? Colour (0xff1b1c20) : ui::col::knob.brighter (0.1f));
        b->setColour (TextButton::buttonOnColourId, ui::col::pitch.withAlpha (black ? 0.7f : 0.9f));
        b->setTooltip ("Allow this note (turn off to keep correction away from it)");
        body.addAndMakeVisible (*b);
        noteAttach[(size_t) pc] = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, IDs::note (pc), *b);
        noteButtons[(size_t) pc] = std::move (b);
    }

    for (int b = 0; b < vs::VocalEq::kBands; ++b)
    {
        auto& s = eqSliders[(size_t) b];
        s.setSliderStyle (Slider::LinearVertical);
        s.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        s.setColour (Slider::trackColourId, ui::col::accent);
        body.addAndMakeVisible (s);
        eqAttach[(size_t) b] = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, IDs::eq (b), s);
        s.setDoubleClickReturnValue (true, 0.0);
        auto& l = eqLabels[(size_t) b];
        const float f = vs::VocalEq::kFreqs[b];
        l.setText (f >= 1000 ? String (f / 1000.0f, f >= 10000 ? 1 : 0) + "k" : String ((int) f), dontSendNotification);
        l.setJustificationType (Justification::centred);
        l.setFont (FontOptions (10.5f));
        l.setColour (Label::textColourId, ui::col::dim);
        body.addAndMakeVisible (l);
    }

    for (auto* l : { &statusLabel, &infoLabel, &detectedLabel })
    {
        l->setFont (FontOptions (12.5f));
        l->setColour (Label::textColourId, ui::col::dim);
    }
    statusLabel.setJustificationType (Justification::centredRight);
    infoLabel.setJustificationType (Justification::centredRight);
    pitchReadout.setFont (FontOptions (22.0f, Font::bold));
    pitchReadout.setJustificationType (Justification::centred);
    pitchReadout.setColour (Label::textColourId, ui::col::pitch);

    body.onPaint = [this] (Graphics& g) { paintBody (g); };
    body.onMouseUp = [this] (const MouseEvent& e)
    {
        if (titleArea().contains (e.getPosition()))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, String ("About ") + VS_PRODUCT_NAME, aboutText(), "OK", this);
    };
    body.onMouseMove = [this] (const MouseEvent& e)
    { body.setMouseCursor (titleArea().contains (e.getPosition()) ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor); };
    body.onResize = [this] { layoutBody(); };
    addAndMakeVisible (body);
    body.setBounds (0, 0, kBaseW, baseH);

    setResizable (true, true);
    setResizeLimits (kBaseW / 2, baseH / 2, kBaseW * 4, baseH * 4);
    if (! proc.isStandaloneApp())
        if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) kBaseW / baseH);
    setSize (proc.editorWidth > 0 ? proc.editorWidth : kBaseW, proc.editorHeight > 0 ? proc.editorHeight : baseH);
    timerCallback();
    sizeReady = true;
    startTimerHz (30);
}

VocalStudioAudioProcessorEditor::~VocalStudioAudioProcessorEditor()
{
    if (proc.isAnalyzing()) proc.stopAnalyzeAndApply();
    setLookAndFeel (nullptr);
}

// The title, drawn the same way in Open Vocal Studio, Clean Tone Studio and Open Mastering Studio:
// the name in white with "STUDIO" in blue, 22 px bold, then the version, dim, beside it.
namespace
{
    const float kTitlePx = 22.0f, kVersionPx = 12.0f;
    Font titleFont()   { return Font (FontOptions (kTitlePx, Font::bold)); }
    Font versionFont() { return Font (FontOptions (kVersionPx)); }
    String versionText() { return String ("v") + VS_VERSION; }
}

int VocalStudioAudioProcessorEditor::titleWidth()
{
    return GlyphArrangement::getStringWidthInt (titleFont(), titleText()) + 8
         + GlyphArrangement::getStringWidthInt (versionFont(), versionText());
}

String VocalStudioAudioProcessorEditor::aboutText()
{
    return String (VS_PRODUCT_NAME) + " " + VS_VERSION + "\n"
           "Copyright (C) 2026 Al Oomens\n\n"
           "This program is free software: you can redistribute it and/or modify it under the terms of the "
           "GNU Affero General Public License as published by the Free Software Foundation, either version 3 "
           "of the License, or (at your option) any later version.\n\n"
           "This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without "
           "even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU "
           "Affero General Public License (LICENSE.txt, included with the program) for more details.\n\n"
           "The source code is distributed alongside the program (" + String (VS_PRODUCT_NAME).removeCharacters (" ")
           + "-" VS_VERSION "-source.zip).\n\n"
           "Noise removal uses the DeepFilterNet3 model (Hendrik Schroeter et al., MIT / Apache-2.0). "
           "Built with JUCE (AGPLv3) and the Steinberg VST3 and ASIO SDKs (GPLv3). "
           "VST and ASIO are trademarks of Steinberg Media Technologies GmbH. See THIRD-PARTY-NOTICES.txt.";
}

void VocalStudioAudioProcessorEditor::resized()
{
    const float s = jmin ((float) getWidth() / kBaseW, (float) getHeight() / baseH);
    const float x = 0.5f * ((float) getWidth() - kBaseW * s);
    const float y = 0.5f * ((float) getHeight() - baseH * s);
    body.setTransform (AffineTransform::scale (s).translated (x, y));
    const auto* peer = getPeer();
    if (sizeReady && (peer == nullptr || ! (peer->isFullScreen() || peer->isMinimised())))
    {
        proc.editorWidth = getWidth();
        proc.editorHeight = getHeight();
    }
}

void VocalStudioAudioProcessorEditor::paint (Graphics& g) { g.fillAll (ui::col::bg); }

void VocalStudioAudioProcessorEditor::parentHierarchyChanged()
{
    AudioProcessorEditor::parentHierarchyChanged();
    if (! proc.isStandaloneApp()) return;
    auto* window = findParentComponentOfClass<DocumentWindow>();
    if (window == nullptr || window == setUpWindow.getComponent()) return;
    setUpWindow = window;
    MessageManager::callAsync ([w = Component::SafePointer<DocumentWindow> (window)]
    {
        if (w == nullptr) return;
        w->setTitleBarButtonsRequired (DocumentWindow::allButtons, false);
        w->setName (String (VS_PRODUCT_NAME) + " " + VS_VERSION);     // the version in the app's title bar too
    });
}

void VocalStudioAudioProcessorEditor::setStatus (const String& t, Colour c, int seconds)
{
    statusLabel.setText (t, dontSendNotification);
    statusLabel.setColour (Label::textColourId, c);
    statusTicks = seconds * 30;
}

//==============================================================================
void VocalStudioAudioProcessorEditor::layoutBody()
{
    sections.clear();
    auto sec = [this] (const char* t, Rectangle<int> r, Colour c) { sections.push_back ({ t, r, c }); return r; };

    // header
    const int tx = 20 + titleWidth() + 16;
    presetBox.setBounds (tx, 14, 160, 28);
    mode.setBounds (tx + 166, 14, 156, 28);
    analyzeButton.setBounds (tx + 328, 14, 90, 28);
    compare.setBounds (tx + 424, 14, 184, 28);       // PROCESSED | ORIGINAL
    statusLabel.setBounds (tx + 616, 6, kBaseW - 12 - (tx + 616), 22);
    infoLabel.setBounds (tx + 616, 28, kBaseW - 12 - (tx + 616), 20);

    auto knobRow = [] (Rectangle<int> r, std::initializer_list<Component*> cs, int w)
    {
        int x = r.getX();
        for (auto* c : cs) { c->setBounds (x, r.getY(), w, r.getHeight()); x += w; }
    };

    // CLEAN
    auto c = sec ("CLEAN", { 12, 64, 290, 250 }, ui::col::accent);
    knobRow ({ c.getX() + 10, c.getY() + 30, 270, 108 }, { &denoise, &deroom, &deplosive }, 90);
    knobRow ({ c.getX() + 55, c.getY() + 138, 180, 108 }, { &rumble, &inGain }, 90);

    // PITCH
    auto pt = sec ("PITCH", { 310, 64, 500, 250 }, ui::col::pitch);
    pitchOn.setBounds (pt.getX() + 12, pt.getY() + 34, 56, 26);
    knobRow ({ pt.getX() + 76, pt.getY() + 28, 270, 108 }, { &strength, &speed, &tolerance }, 90);
    key.setBounds (pt.getX() + 362, pt.getY() + 28, 126, 46);
    scale.setBounds (pt.getX() + 362, pt.getY() + 80, 126, 46);
    pitchReadout.setBounds (pt.getX() + 362, pt.getY() + 136, 126, 34);
    {
        const int bw = 28, by = pt.getY() + 148;
        for (int i = 0; i < 12; ++i) noteButtons[(size_t) i]->setBounds (pt.getX() + 12 + i * (bw + 1), by, bw, 26);
    }
    detectedLabel.setBounds (pt.getX() + 12, pt.getY() + 186, 230, 24);
    useKeyButton.setBounds (pt.getX() + 246, pt.getY() + 186, 104, 24);

    // TONE
    auto tn = sec ("TONE", { 818, 64, 370, 250 }, ui::col::accent);
    knobRow ({ tn.getX() + 20, tn.getY() + 30, 330, 108 }, { &resonance, &smooth, &deess }, 110);
    knobRow ({ tn.getX() + 20, tn.getY() + 138, 330, 108 }, { &consonant, &breath, &air }, 110);

    // EQ
    auto e = sec ("TONE EQ", { 12, 322, 600, 220 }, ui::col::accent);
    eqDisplay.setBounds (e.getX() + 12, e.getY() + 30, 444, 108);
    for (int b = 0; b < vs::VocalEq::kBands; ++b)
    {
        const int x = e.getX() + 12 + b * 49;
        eqSliders[(size_t) b].setBounds (x + 4, e.getY() + 142, 40, 54);
        eqLabels[(size_t) b].setBounds (x, e.getY() + 196, 48, 16);
    }
    eqAmount.setBounds (e.getX() + 466, e.getY() + 36, 122, 124);
    eqFlatButton.setBounds (e.getX() + 497, e.getY() + 178, 60, 24);

    // DYNAMICS
    auto d = sec ("DYNAMICS", { 620, 322, 300, 220 }, ui::col::dyn);
    knobRow ({ d.getX() + 2, d.getY() + 40, 148, 120 }, { &rider, &compAmt }, 74);
    {
        int x = d.getX() + 150;
        for (auto* m : { &riderMeter, &compMeter, &deessMeter, &plosiveMeter, &consonantMeter }) { m->setBounds (x, d.getY() + 30, 30, 176); x += 29; }
    }

    // OUTPUT
    auto o = sec ("OUTPUT", { 928, 322, 260, 220 }, ui::col::good);
    outGain.setBounds (o.getX() + 8, o.getY() + 40, 96, 120);
    {
        int x = o.getX() + 112;
        for (auto* m : { &inMeter, &outMeter, &limMeter }) { m->setBounds (x, o.getY() + 30, 44, 176); x += 46; }
    }

    // SPACE
    auto s = sec ("SPACE", { 12, 550, 1176, 160 }, ui::col::space);
    revType.setBounds (s.getX() + 14, s.getY() + 32, 96, 110);
    knobRow ({ s.getX() + 118, s.getY() + 30, 344, 120 }, { &revMix, &revDecay, &revPreDelay, &revDuck }, 86);
    dlyOn.setBounds (s.getX() + 490, s.getY() + 40, 74, 28);
    dlySync.setBounds (s.getX() + 490, s.getY() + 76, 74, 28);
    dlyTime.setBounds (s.getX() + 570, s.getY() + 30, 86, 120);
    dlyNote.setBounds (s.getX() + 574, s.getY() + 58, 80, 46);
    knobRow ({ s.getX() + 660, s.getY() + 30, 344, 120 }, { &dlyFeedback, &dlyMix, &dlyDuck, &dlyWidth }, 86);
    doubler.setBounds (s.getX() + 1050, s.getY() + 30, 100, 120);

    // FILE (standalone app only)
    if (fileUi)
    {
        auto f = sec ("FILE", { 12, 716, 1176, 38 }, ui::col::good);
        const int y = f.getY() + 6, h = 26;
        srcLive.setBounds (82, y, 56, h);
        srcFile.setBounds (140, y, 56, h);
        openButton.setBounds (206, y, 76, h);
        fileLabel.setBounds (288, y, 230, h);
        playButton.setBounds (524, y, 64, h);
        loopButton.setBounds (592, y, 64, h);
        posSlider.setBounds (664, y + 4, 290, h - 8);
        timeLabel.setBounds (958, y, 88, h);
        saveButton.setBounds (1052, y, 130, h);
    }
}

void VocalStudioAudioProcessorEditor::paintBody (Graphics& g)
{
    g.fillAll (ui::col::bg);
    {
        // "OPEN VOCAL " white, "STUDIO" blue, then the version (the same editor serves the VST3 and the app)
        const String name = titleText();
        const String first = name.upToLastOccurrenceOf (" ", true, false), last = name.fromLastOccurrenceOf (" ", false, false);
        const int firstW = GlyphArrangement::getStringWidthInt (titleFont(), first);
        const int lastW = GlyphArrangement::getStringWidthInt (titleFont(), last);
        const int x = 20, y = 8, h = 40;
        g.setFont (titleFont());
        g.setColour (ui::col::titleWhite);
        g.drawText (first, x, y, firstW + 4, h, Justification::centredLeft);
        g.setColour (ui::col::studio);
        g.drawText (last, x + firstW, y, lastW + 4, h, Justification::centredLeft);
        g.setColour (ui::col::dim);
        g.setFont (versionFont());
        g.drawText (versionText(), x + firstW + lastW + 8, y + 3, 80, h, Justification::centredLeft);   // on the title's baseline
    }
    for (auto& s : sections)
    {
        // section title in its colour with a short underline (as in Clean Tone Studio)
        const auto r = s.area.toFloat();
        g.setColour (ui::col::panel);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (ui::col::panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        // (a one-row strip such as FILE gets its title centred on the row)
        const auto band = r.getHeight() < 60.0f ? r.reduced (12, 0).withSizeKeepingCentre (r.getWidth() - 24, 26).translated (0, -2)
                                                : r.reduced (12, 0).removeFromTop (26);
        g.setColour (s.colour);
        g.setFont (FontOptions (13.5f, Font::bold));
        g.drawText (s.title, band, Justification::centredLeft);
        g.setColour (s.colour.withAlpha (0.35f));
        g.fillRect (band.getX(), band.getBottom(), 28.0f, 2.0f);
    }
    // SPACE: divider between reverb and delay
    g.setColour (ui::col::panelEdge);
    g.drawVerticalLine (12 + 476, 580, 700);
    g.drawVerticalLine (12 + 1040, 580, 700);
}

//==============================================================================
// Standalone app: the file strip under SPACE
void VocalStudioAudioProcessorEditor::setUpFileStrip()
{
    for (Component* c : std::initializer_list<Component*> { &srcLive, &srcFile, &openButton, &playButton, &loopButton,
                                                             &saveButton, &fileLabel, &timeLabel, &posSlider })
        body.addAndMakeVisible (c);
    for (auto* b : { &srcLive, &srcFile }) b->setColour (TextButton::buttonOnColourId, ui::col::good);
    for (auto* b : { &playButton, &loopButton }) b->setColour (TextButton::buttonOnColourId, ui::col::good);
    srcLive.setTooltip ("Process the sound card's input (sing through it).");
    srcFile.setTooltip ("Process the loaded file instead of the input.");
    openButton.setTooltip ("Load a recording (WAV, AIFF, FLAC, MP3, Ogg) to play through Open Vocal Studio.");
    loopButton.setTooltip ("Play the file round and round.");
    saveButton.setTooltip ("Save the whole file processed with the current settings, lined up with the original (24-bit WAV).");
    for (auto* l : { &fileLabel, &timeLabel })
    {
        l->setFont (FontOptions (12.5f));
        l->setColour (Label::textColourId, ui::col::dim);
    }
    timeLabel.setJustificationType (Justification::centred);
    posSlider.setSliderStyle (Slider::LinearBar);
    posSlider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    posSlider.setColour (Slider::trackColourId, ui::col::good.withAlpha (0.55f));
    posSlider.setColour (Slider::backgroundColourId, Colours::black.withAlpha (0.35f));
    posSlider.setRange (0.0, 1.0);
    posSlider.onValueChange = [this]
    {
        if (posSlider.isMouseButtonDown() && proc.player.hasTake())
            proc.player.seekTo.store ((int64_t) (posSlider.getValue() * proc.player.getTake()->rate));
    };

    srcLive.onClick = [this] { proc.player.source.store (false); proc.player.playing.store (false); };
    srcFile.onClick = [this] { proc.player.source.store (true); };
    loopButton.onClick = [this] { proc.player.loop.store (! proc.player.loop.load()); };
    playButton.onClick = [this]
    {
        auto& pl = proc.player;
        if (! pl.hasTake()) { setStatus ("Open a file first.", ui::col::accent, 5); return; }
        pl.source.store (true);
        if (pl.playing.load()) { pl.playing.store (false); return; }
        if (pl.posSeconds.load() >= pl.lengthSeconds() - 0.01) pl.seekTo.store (0);
        pl.playing.store (true);
    };
    openButton.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Open a vocal recording", lastDir, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == File()) return;
            lastDir = f.getParentDirectory();
            const auto err = proc.loadFileTake (f);
            if (err.isNotEmpty()) { setStatus (err, ui::col::hot, 8); return; }
            proc.player.source.store (true);
            setStatus ("Loaded " + f.getFileName() + ". Press PLAY.", ui::col::good, 6);
        });
    };
    saveButton.onClick = [this]
    {
        if (! proc.player.hasTake()) { setStatus ("Open a file first.", ui::col::accent, 5); return; }
        if (render != nullptr) return;
        const auto* t = proc.player.getTake();
        const auto suggested = lastDir.getChildFile (File::createLegalFileName (t->name).upToLastOccurrenceOf (".", false, false) + " (Open Vocal Studio).wav");
        chooser = std::make_unique<FileChooser> ("Save the processed file", suggested, "*.wav");
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting, [this] (const FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == File() || ! proc.player.hasTake()) return;
            if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
            render = std::make_unique<OfflineRender> (proc, *proc.player.getTake(), f);
            render->start();
        });
    };
}

void VocalStudioAudioProcessorEditor::updateFileStrip()
{
    auto& pl = proc.player;
    const bool file = pl.source.load(), has = pl.hasTake(), playing = pl.playing.load();
    srcLive.setToggleState (! file, dontSendNotification);
    srcFile.setToggleState (file, dontSendNotification);
    playButton.setButtonText (playing ? "STOP" : "PLAY");
    playButton.setToggleState (playing, dontSendNotification);
    loopButton.setToggleState (pl.loop.load(), dontSendNotification);
    for (Component* c : std::initializer_list<Component*> { &playButton, &posSlider, &saveButton })
        c->setAlpha (has ? 1.0f : 0.4f);
    auto mmss = [] (double s) { const int t = (int) s; return String (t / 60) + ":" + String (t % 60).paddedLeft ('0', 2); };
    fileLabel.setText (has ? pl.getTake()->name : String ("no file loaded"), dontSendNotification);
    const double len = pl.lengthSeconds(), pos = jlimit (0.0, jmax (0.0, len), pl.posSeconds.load());
    timeLabel.setText (has ? mmss (pos) + " / " + mmss (len) : String ("-:-- / -:--"), dontSendNotification);
    if (len > 0.0 && std::abs (posSlider.getMaximum() - len) > 1e-6) posSlider.setRange (0.0, len);
    if (! posSlider.isMouseButtonDown()) posSlider.setValue (pos, dontSendNotification);

    if (render != nullptr)
    {
        if (render->done.load())
        {
            if (render->error.isNotEmpty()) setStatus (render->error, ui::col::hot, 10);
            else setStatus ("Saved " + render->destination().getFileName(), ui::col::good, 10);
            render.reset();
        }
        else setStatus ("Saving the processed file... " + String (roundToInt (render->progress.load() * 100.0f)) + " %", ui::col::accent, 2);
    }
}

void VocalStudioAudioProcessorEditor::timerCallback()
{
    const auto& m = proc.meters;
    inMeter.setDb (m.in.load()); outMeter.setDb (m.out.load()); limMeter.setDb (m.limiter.load());
    riderMeter.setDb (m.rider.load()); compMeter.setDb (m.comp.load());
    deessMeter.setDb (m.deess.load()); plosiveMeter.setDb (m.plosive.load()); consonantMeter.setDb (m.consonant.load());
    eqDisplay.repaint();

    // pitch readout
    const bool pOn = proc.apvts.getRawParameterValue (IDs::pitchOn)->load() > 0.5f;
    const int nt = proc.currentNote();
    String r;
    if (nt >= 0)
    {
        const float cents = proc.currentCorrection();
        const int ci = roundToInt (cents);
        r << vs::Scales::noteName (nt) << (nt / 12 - 1) << "  " << (ci >= 0 ? "+" : "") << ci << " c";
    }
    pitchReadout.setText (pOn ? r : String ("off"), dontSendNotification);
    for (int pc = 0; pc < 12; ++pc)
        noteButtons[(size_t) pc]->setColour (TextButton::textColourOnId, (nt >= 0 && nt % 12 == pc) ? Colours::white : Colours::black);
    // a section that's off shows its controls dimmed (they can still be set in advance)
    auto dimUnless = [] (bool active, std::initializer_list<Component*> cs) { for (auto* c : cs) c->setAlpha (active ? 1.0f : 0.35f); };
    dimUnless (pOn, { &strength, &speed, &tolerance, &key, &scale, &pitchReadout });
    for (auto& b : noteButtons) b->setAlpha (pOn ? 1.0f : 0.35f);
    dimUnless (revType.index() != 0, { &revMix, &revDecay, &revPreDelay, &revDuck });
    dimUnless (proc.apvts.getRawParameterValue (IDs::dlyOn)->load() > 0.5f, { &dlySync, &dlyTime, &dlyNote, &dlyFeedback, &dlyMix, &dlyDuck, &dlyWidth });

    int k; bool minor; float conf;
    proc.detectedKey (k, minor, conf);
    useKeyButton.setAlpha (proc.detectedKeyUsable() ? 1.0f : 0.4f);   // usable once a key is detected
    detectedLabel.setText (conf >= 0.05f ? String ("Detected: ") + vs::Scales::noteName (k) + (minor ? " minor" : " major")
                                             + "  (" + String ((int) (conf * 100)) + "% sure)"
                                       : String ("Detected key: listening..."), dontSendNotification);

    // delay: time knob or note value
    const bool sync = proc.apvts.getRawParameterValue (IDs::dlySync)->load() > 0.5f;
    dlyTime.setVisible (! sync); dlyNote.setVisible (sync);

    // status / info line
    const double lat = proc.chainLatency() / proc.getSampleRate() * 1000.0;
    String info;
    // (the mode is in the Mode box; "original" = how far ORIGINAL is turned to match the processed level)
    info << "latency " << (lat < 100 ? String (lat, 1) : String (roundToInt (lat))) << " ms  |  "
         << (proc.isMixMode() ? (proc.neuralActive() ? "neural denoiser" : "spectral denoiser (" + vs::DeepFilter::status().message + ")") : "spectral denoiser")
         << "  |  original " << (m.matchDb.load() > 0.05f ? "+" : "") << String (m.matchDb.load(), 1) << " dB";
    infoLabel.setText (info, dontSendNotification);
    if (proc.isAnalyzing())
        setStatus ("Analyzing... " + String (proc.analyzeSeconds(), 0) + " s of voice so far. Click ANALYZE again to apply.", ui::col::accent, 600);
    else if (statusTicks > 0 && --statusTicks == 0) statusLabel.setText ({}, dontSendNotification);
    analyzeButton.setToggleState (proc.isAnalyzing(), dontSendNotification);
    if (presetBox.getSelectedId() != proc.getCurrentProgram() + 1) presetBox.setSelectedId (proc.getCurrentProgram() + 1, dontSendNotification);
    if (fileUi) updateFileStrip();
}
