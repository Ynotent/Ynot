// Main window content, shared by every platform. Arrangement and control sizes
// come from ynot::decideLayout(), so phones, tablets and desktops adapt the same
// way; the platform only changes chrome (menus, file pickers, store UI).
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "ArrangeView.h"
#include "CounterDisplay.h"
#include "InspectorView.h"
#include "MixerView.h"
#include "StepSequencerView.h"
#include "SamplerView.h"
#include "Session.h"
#include "Theme.h"
#include "ynot/ui_layout.hpp"

namespace ynotapp {

juce::File pluginCacheFile();  // device-local list of scanned plugins (Main.cpp)

// Transport button with a drawn icon (works the same in every font).
class IconButton : public juce::TextButton {
public:
    enum class Icon { ToStart, Play, Record };
    explicit IconButton(Icon icon) : juce::TextButton(juce::String()), icon_(icon) {}
    void paintButton(juce::Graphics& g, bool over, bool down) override {
        auto& lf = getLookAndFeel();
        lf.drawButtonBackground(g, *this, findColour(getToggleState() ? buttonOnColourId : buttonColourId), over, down);
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre(16.f, 16.f);
        g.setColour(findColour(getToggleState() ? textColourOnId : textColourOffId));
        juce::Path p;
        switch (icon_) {
            case Icon::ToStart:
                g.fillRect(r.removeFromLeft(3.f));
                p.addTriangle(r.getRight(), r.getY(), r.getRight(), r.getBottom(), r.getX() + 1.f, r.getCentreY());
                break;
            case Icon::Play:
                if (getToggleState()) p.addRectangle(r.reduced(1.f));  // playing: stop square
                else p.addTriangle(r.getX() + 2.f, r.getY(), r.getX() + 2.f, r.getBottom(), r.getRight(), r.getCentreY());
                break;
            case Icon::Record:
                g.setColour(getToggleState() ? juce::Colours::white : juce::Colour(0xffff4d5e));
                p.addEllipse(r.reduced(1.f));
                break;
        }
        g.fillPath(p);
    }

private:
    Icon icon_;
};

class MainComponent : public juce::Component, public juce::MenuBarModel, private juce::Timer {
public:
    MainComponent(Session& session, JucePluginHost& host);
    ~MainComponent() override;

    // Lets the dev host preview another platform's layout (screenshots, design review).
    static std::optional<ynot::Platform> layoutPlatformOverride;

    void paint(juce::Graphics&) override;
    void resized() override;
    // Menu bar (macOS: the system menu bar; Windows/Linux: across the top of the window).
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int, int) override {}
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    // Take keyboard focus as soon as the window is on screen, so shortcuts (Space, Return,
    // Cmd+S, Cmd+Z...) work without clicking into the arrangement first.
    void parentHierarchyChanged() override { focusSoon(); }
    void visibilityChanged() override { focusSoon(); }
    void focusSoon() {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe && safe->isShowing() && !safe->hasKeyboardFocus(true)) safe->grabKeyboardFocus();
        });
    }
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
    void layoutDesktop();
    void openProject();
    void newDemo();
    void newSong();
    void saveAs();
    void openRecent(const juce::File& dir);
    void showAudioSettings();
    void toggleRecording();
    void togglePlay();
    void handleTransport(TransportCommand c);
    void addMidiControllerItems(juce::PopupMenu& m);
    void scanPlugins();
    void prepareForMobile();
    void showMoreMenu();
    void showCompatibilityDialog();
    void addQuantizeItems(juce::PopupMenu& m);
    void addPluginItems(juce::PopupMenu& m);
    void addTrackItems(juce::PopupMenu& m);
    void importAudio(const std::string& trackId);  // file chooser
    void importFiles(const juce::Array<juce::File>& files, const std::string& trackId, int64_t startTick);
    void addPunchItems(juce::PopupMenu& m);
    void addTimeSignatureItems(juce::PopupMenu& m);
    void addSampleRateItems(juce::PopupMenu& m);
    void addTempoItems(juce::PopupMenu& m);
    void addLoopItems(juce::PopupMenu& m);
    void toggleMixer();
    void toggleSteps();
public:
    void showPanel(const juce::String& which) { if (which == "sampler") toggleSampler(); }
private:
    void toggleSampler();
    void openSampler(const std::string& trackId);
    void newSamplerTrack();
    void addAiItems(juce::PopupMenu& m);
    void openPattern(const std::string& trackId, const std::string& clipId);
    // Small dialog asking for a number; `onOk` gets the value if it's within [lo, hi].
    void askNumber(const juce::String& title, const juce::String& message, double initial, double lo, double hi,
                   std::function<void(double)> onOk);
    ynot::Platform selectedTarget() const;
    bool compact() const { return layout_.layout == ynot::LayoutClass::Compact; }

    Session& session_;
    JucePluginHost& host_;
    Theme theme_;
    juce::LookAndFeel_V4 lnf_;
    ynot::LayoutDecision layout_{};

    // Transport and editing (always visible).
    IconButton startButton_{IconButton::Icon::ToStart}, playButton_{IconButton::Icon::Play},
        recordButton_{IconButton::Icon::Record};
    juce::TextButton clickButton_{"Click"};
    juce::TextButton countInButton_{"Count-in"}, monitorButton_{"Monitor"};
    juce::TextButton quantizeButton_{"Quantize"}, punchButton_{"Punch"}, timeSigButton_{"4/4"}, rateButton_{"48 kHz"}, tempoButton_{"120 BPM"}, loopButton_{"Loop"}, mixerButton_{"Mixer"}, exportButton_{"Export"}, stepsButton_{"Steps"}, samplerButton_{"Sampler"}, aiButton_{"AI"};
    juce::TextButton addTrackButton_{"+ Track"};
    juce::TextButton undoButton_{"Undo"}, redoButton_{"Redo"}, keysButton_{"Keys"};
    // File/device actions: toolbar on larger screens, "More" menu on phones.
    juce::TextButton newButton_{"New Demo"}, openButton_{"Open"}, saveButton_{"Save"};
    juce::TextButton audioButton_{"Audio"}, scanButton_{"Scan Plugins"}, moreButton_{"More"};
    juce::Label status_;
    ArrangeView arrange_;
    CounterDisplay counter_;
    InspectorView inspector_;
    bool inspectorVisible_ = true, compatPanelVisible_ = false;
#if !(JUCE_MAC || JUCE_IOS || JUCE_ANDROID)
    juce::MenuBarComponent menuBar_{this};
#endif
    MixerView mixer_;
    StepSequencerView steps_;
    SamplerView sampler_;
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
    bool scanning_ = false;
};

}  // namespace ynotapp
