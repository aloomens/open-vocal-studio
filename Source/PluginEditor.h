#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

namespace ui
{
    namespace col
    {
        const juce::Colour bg        { 0xff16171a };
        const juce::Colour panel     { 0xff212328 };
        const juce::Colour panelEdge { 0xff32353c };
        const juce::Colour text      { 0xffdcd9d2 };
        const juce::Colour dim       { 0xff8b8b88 };
        const juce::Colour accent    { 0xffe39a6b };   // warm coral: cleanup and tone
        const juce::Colour pitch     { 0xff8fb8ec };   // soft blue: pitch
        const juce::Colour space     { 0xff6fc2c9 };   // teal: reverb / delay
        const juce::Colour dyn       { 0xffd9c46a };   // gold: dynamics
        const juce::Colour knob      { 0xff2d2f35 };
        const juce::Colour good      { 0xff7cc576 };
        const juce::Colour hot       { 0xffe0533d };
        // the title, shared by Open Vocal Studio, Clean Tone Studio and Open Mastering Studio:
        const juce::Colour titleWhite { 0xffe4e7ee };  // "OPEN VOCAL"
        const juce::Colour studio     { 0xff4fc3f7 };  // "STUDIO"
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;
        void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
        juce::Font getLabelFont (juce::Label&) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    };

    struct Knob : juce::Component
    {
        Knob (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& name, juce::Colour c = col::accent);
        void resized() override;
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    struct Choice : juce::Component
    {
        Choice (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& name);
        void resized() override;
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    };

    // a choice as a row or column of buttons; the active one is lit
    struct Switch : juce::Component
    {
        // a choice parameter, one button per choice, in a column (lit in 'c', or grey for "Off")
        Switch (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& name, juce::Colour c);
        // any parameter with labels.size() states, in a row, each lit in its own colour
        Switch (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::StringArray& labels, const juce::Array<juce::Colour>& onColours);
        void resized() override;
        int index() const { return current; }
        juce::Label label;
        std::vector<std::unique_ptr<juce::TextButton>> buttons;
        std::unique_ptr<juce::ParameterAttachment> attachment;
        int current = 0;
        bool row = false;
    private:
        void attach (juce::RangedAudioParameter& p);
    };

    struct Toggle : juce::Component
    {
        Toggle (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& name, juce::Colour c = col::accent);
        void resized() override { button.setBounds (getLocalBounds()); }
        juce::ToggleButton button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    };

    // vertical bar: level (dBFS) or gain change (dB, negative = reduction, positive = boost for the rider)
    struct Meter : juce::Component
    {
        enum Kind { level, reduction, bipolar };
        Meter (juce::String name, Kind k) : title (std::move (name)), kind (k) {}
        void setDb (float db) { if (std::abs (db - value) > 0.05f) { value = db; repaint(); } }
        void paint (juce::Graphics&) override;
        juce::String title;
        Kind kind;
        float value = -100.0f;
    };

    // EQ curve, live gain reduction of the spectral stage, frequency grid
    struct EqDisplay : juce::Component
    {
        explicit EqDisplay (VocalStudioAudioProcessor& p) : proc (p) {}
        void paint (juce::Graphics&) override;
        VocalStudioAudioProcessor& proc;
    };
}

class OfflineRender;

class VocalStudioAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VocalStudioAudioProcessorEditor (VocalStudioAudioProcessor&);
    ~VocalStudioAudioProcessorEditor() override;

    static constexpr int kBaseW = 1200, kBaseH = 720, kFileStripH = 44;
    int getBaseHeight() const { return baseH; }      // taller in the standalone app (file strip)

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    static juce::String aboutText();

private:
    struct Body : juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        std::function<void()> onResize;
        std::function<void (const juce::MouseEvent&)> onMouseUp, onMouseMove;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
        void resized() override { if (onResize) onResize(); }
        void mouseUp (const juce::MouseEvent& e) override { if (onMouseUp) onMouseUp (e); }
        void mouseMove (const juce::MouseEvent& e) override { if (onMouseMove) onMouseMove (e); }
    };

    void paintBody (juce::Graphics&);
    void layoutBody();
    void timerCallback() override;
    static juce::String titleText() { return juce::String (VS_PRODUCT_NAME).toUpperCase(); }
    static int titleWidth();                         // "OPEN VOCAL STUDIO v1.3.0", as drawn
    juce::Rectangle<int> titleArea() const { return { 12, 8, titleWidth() + 16, 40 }; }
    void setStatus (const juce::String& t, juce::Colour c, int seconds = 8);

    VocalStudioAudioProcessor& proc;
    ui::LookAndFeel lnf;
    Body body;
    juce::Component::SafePointer<juce::DocumentWindow> setUpWindow;
    bool sizeReady = false;

    // header
    juce::ComboBox presetBox;
    ui::Choice mode;
    juce::TextButton analyzeButton { "ANALYZE" };
    ui::Switch compare;                          // PROCESSED / ORIGINAL: the lit one is what you hear
    juce::Label statusLabel, infoLabel;
    int statusTicks = 0;
    // clean
    ui::Knob denoise, deroom, deplosive, rumble, inGain;
    // pitch
    ui::Toggle pitchOn;
    ui::Knob strength, speed, tolerance;
    ui::Choice key, scale;
    juce::TextButton useKeyButton { "Use Detected" };
    juce::Label detectedLabel, pitchReadout;
    std::array<std::unique_ptr<juce::TextButton>, 12> noteButtons;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 12> noteAttach;
    // tone
    ui::Knob resonance, smooth, deess, breath, consonant;
    // eq
    ui::EqDisplay eqDisplay;
    std::array<juce::Slider, vs::VocalEq::kBands> eqSliders;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, vs::VocalEq::kBands> eqAttach;
    std::array<juce::Label, vs::VocalEq::kBands> eqLabels;
    ui::Knob eqAmount, air;
    juce::TextButton eqFlatButton { "Flat" };
    // dynamics
    ui::Knob rider, compAmt;
    ui::Meter riderMeter { "RIDE", ui::Meter::bipolar }, compMeter { "COMP", ui::Meter::reduction },
              deessMeter { "S", ui::Meter::reduction }, plosiveMeter { "P", ui::Meter::reduction }, consonantMeter { "K", ui::Meter::reduction };
    // output
    ui::Knob outGain;
    ui::Meter inMeter { "IN", ui::Meter::level }, outMeter { "OUT", ui::Meter::level }, limMeter { "LIM", ui::Meter::reduction };
    // space
    ui::Switch revType;
    ui::Knob revMix, revDecay, revPreDelay, revDuck;
    ui::Toggle dlyOn, dlySync;
    ui::Knob dlyTime;
    ui::Choice dlyNote;
    ui::Knob dlyFeedback, dlyMix, dlyDuck, dlyWidth, doubler;

    struct Section { juce::String title; juce::Rectangle<int> area; juce::Colour colour; };
    std::vector<Section> sections;

    // standalone app only: play a file through the chain, save it processed
    bool fileUi = false;
    int baseH = kBaseH;
    juce::TextButton srcLive { "LIVE" }, srcFile { "FILE" }, openButton { "Open..." }, playButton { "PLAY" },
                     loopButton { "LOOP" }, saveButton { "Save Processed..." };
    juce::Label fileLabel, timeLabel;
    juce::Slider posSlider;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<OfflineRender> render;
    juce::File lastDir;
    void setUpFileStrip();
    void updateFileStrip();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalStudioAudioProcessorEditor)
};
