// Main window content, shared by every platform. Arrangement and control sizes
// come from ynot::decideLayout(), so phones, tablets and desktops adapt the same
// way; the platform only changes chrome (menus, file pickers, store UI).
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "ArrangeView.h"
#include "Session.h"
#include "Theme.h"
#include "ynot/ui_layout.hpp"

namespace ynotapp {

class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent(Session& session, JucePluginHost& host);
    ~MainComponent() override;

    // Lets the dev host preview another platform's layout (screenshots, design review).
    static std::optional<ynot::Platform> layoutPlatformOverride;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void showKeyboard(bool show) {
        keysButton_.setToggleState(show, juce::dontSendNotification);
        keyboard_.setVisible(show);
        arrange_.setMusicalTyping(show);
        resized();
        if (show && isShowing()) keyboard_.grabKeyboardFocus();  // type notes right away
    }

private:
    void timerCallback() override;

    void refresh();
    void refreshCompatibility();
    void openProject();
    void newDemo();
    void showAudioSettings();
    void toggleRecording();
    void togglePlay();
    void scanPlugins();
    void prepareForMobile();
    void showMoreMenu();
    void showCompatibilityDialog();
    ynot::Platform selectedTarget() const;
    bool compact() const { return layout_.layout == ynot::LayoutClass::Compact; }

    Session& session_;
    JucePluginHost& host_;
    Theme theme_;
    juce::LookAndFeel_V4 lnf_;
    ynot::LayoutDecision layout_{};

    // Transport and editing (always visible).
    juce::TextButton playButton_{"Play"}, recordButton_{"Record"}, clickButton_{"Click"};
    juce::TextButton countInButton_{"Count-in"}, monitorButton_{"Monitor"};
    juce::TextButton undoButton_{"Undo"}, redoButton_{"Redo"}, keysButton_{"Keys"};
    // File/device actions: toolbar on larger screens, "More" menu on phones.
    juce::TextButton newButton_{"New Demo"}, openButton_{"Open"}, saveButton_{"Save"};
    juce::TextButton audioButton_{"Audio"}, scanButton_{"Scan Plugins"}, moreButton_{"More"};
    juce::Label status_;
    ArrangeView arrange_;
    // On-screen keyboard: plays armed instrument tracks exactly like a MIDI controller.
    // While it's open, the computer keyboard plays notes too ("musical typing").
    class TypingKeyboard : public juce::MidiKeyboardComponent {
    public:
        using juce::MidiKeyboardComponent::MidiKeyboardComponent;
        // Cmd/Ctrl shortcuts (undo, save...) and unmapped keys (Space, Return) go to the window.
        bool keyPressed(const juce::KeyPress& k) override {
            if (k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown()) return false;
            return juce::MidiKeyboardComponent::keyPressed(k);
        }
        bool keyStateChanged(bool isKeyDown) override {
            if (juce::ModifierKeys::currentModifiers.isCommandDown() || juce::ModifierKeys::currentModifiers.isCtrlDown())
                return false;
            return juce::MidiKeyboardComponent::keyStateChanged(isKeyDown);
        }
    };
    TypingKeyboard keyboard_;

    // Compatibility: side panel on larger screens, summary button + dialog on phones.
    juce::Label compatTitle_;
    juce::ComboBox target_;
    juce::TextButton prepareButton_{"Prepare for iPhone & Android"};
    juce::TextButton compatSummary_;
    juce::TextEditor compat_;
    juce::String compatText_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace ynotapp
