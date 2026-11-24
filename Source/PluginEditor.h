#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class HardwareLookAndFeel;

// The embedded fonts, loaded once and shared by every open AutoClip Pro window.
struct FontHolder
{
    FontHolder();
    juce::Typeface::Ptr bold, semi, med, mono;
};

// Row of hardware lamp keys bound to a choice parameter.
class SegmentControl : public juce::Component
{
public:
    SegmentControl (juce::RangedAudioParameter& param, juce::StringArray labels);
    void resized() override;
    void setLocked (int value, bool locked);          // padlock on that key
    void showValue (int value);                       // which key is lit (the value that actually runs)
    std::function<bool (int value)> allowValue;       // return false to refuse a click (locked Pro value)
    int gap = 10;
private:
    juce::OwnedArray<juce::TextButton> buttons;
    juce::ParameterAttachment attachment;
    int shown = -1;
};

// One lamp key bound to an on/off parameter. A locked key opens the Pro panel instead.
class ToggleKey : public juce::TextButton
{
public:
    ToggleKey (juce::RangedAudioParameter& param, const juce::String& text);
    void setLocked (bool l);
    void showValue (bool on);
    bool locked = false;
    std::function<void()> onLockedClick;
private:
    juce::ParameterAttachment attachment;
    bool value = false;
};

// Dial that can be padlocked: a click on a locked dial opens the Pro panel instead of turning it.
class Dial : public juce::Slider
{
public:
    bool locked = false;
    std::function<void()> onLockedClick;
    void mouseDown (const juce::MouseEvent& e) override        { if (locked) { if (onLockedClick) onLockedClick(); return; } Slider::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override        { if (! locked) Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override          { if (! locked) Slider::mouseUp (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (! locked) Slider::mouseDoubleClick (e); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { if (! locked) Slider::mouseWheelMove (e, w); }
};

// Upgrade panel: what Pro adds, start the 7-day trial, paste a key, or buy.
class ProPanel : public juce::Component
{
public:
    explicit ProPanel (AutoClipProAudioProcessor& p);
    void show (const juce::String& reason);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;   // click outside the card closes it
    std::function<void()> onLicenceChanged;
private:
    void refresh();
    juce::Rectangle<int> card() const;
    AutoClipProAudioProcessor& proc;
    juce::String reason, message;
    bool messageIsError = false, busy = false;
    juce::TextEditor emailBox, keyBox;
    juce::ToggleButton subscribe { "Send me tips and offers" };
    juce::TextButton trialButton { "START 7-DAY FREE TRIAL" }, unlockButton { "UNLOCK" },
                     buyButton { "BUY PRO" }, closeButton { "CLOSE" };
};

// The hero display: the last 2 seconds of waveform going in (red where it's over the ceiling = what
// gets clipped) and coming out, with the ceiling line.
class ClipScope : public juce::Component
{
public:
    explicit ClipScope (AutoClipProAudioProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    float ceilingDb = -0.3f;
private:
    AutoClipProAudioProcessor& proc;
};

class AutoClipProAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AutoClipProAudioProcessorEditor (AutoClipProAudioProcessor&);
    ~AutoClipProAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    void openPro (const juce::String& reason) { proPanel.show (reason); }

private:
    struct Knob
    {
        Dial slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
        juce::String label, paramId;
        juce::StringArray stepLabels;   // empty = continuous dial
        juce::String value;             // text under the dial
    };
    Knob& addKnob (const juce::String& id, const juce::String& label, juce::StringArray steps = {});
    Knob* knob (const juce::String& id);
    void renderBackground();
    void updateLocks();

    juce::SharedResourcePointer<FontHolder> fontHolder;   // first: outlives everything that draws text
    bool fontsInUse;
    AutoClipProAudioProcessor& proc;
    std::unique_ptr<HardwareLookAndFeel> laf;

    SegmentControl typeKeys;
    ToggleKey matchKey, tpKey;
    juce::TextButton learnButton { "LEARN" };
    ClipScope scopeView;
    juce::TextButton licenceButton;
    ProPanel proPanel;
    juce::OwnedArray<Knob> knobs;

    juce::Image background;
    juce::Rectangle<int> ledArea, meterArea, secType, secClip, secPro, secOut;
    juce::String ledLabel, ledText, ledRightLabel, ledRightText;
    float mIn = 0.0f, mOut = 0.0f, mClip = 0.0f, mLouder = 0.0f, mDist = -120.0f;
    int clipHeat = 0;                             // 0 clean, 1 audible, 2 hot (from the clipper's distortion)
    juce::Colour heatColour() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoClipProAudioProcessorEditor)
};
