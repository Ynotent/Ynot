// Main window content, shared by every platform. Arrangement and control sizes
// come from ynot::decideLayout(), so phones, tablets and desktops adapt the same
// way; the platform only changes chrome (menus, file pickers, store UI).
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "Session.h"
#include "Theme.h"
#include "ynot/ui_layout.hpp"

namespace ynotapp {

class MainComponent : public juce::Component, private juce::ListBoxModel, private juce::Timer {
public:
    MainComponent(Session& session, JucePluginHost& host);
    ~MainComponent() override;

    // Lets the dev host preview another platform's layout (screenshots, design review).
    static std::optional<ynot::Platform> layoutPlatformOverride;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // ListBoxModel (track list)
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    juce::Component* refreshComponentForRow(int row, bool selected, juce::Component* existing) override;
    void timerCallback() override;

    void refresh();
    void refreshCompatibility();
    void openProject();
    void showAudioSettings();
    void toggleRecording();
    void scanPlugins();
    void prepareForMobile();
    ynot::Platform selectedTarget() const;

    class TrackRow;

    Session& session_;
    JucePluginHost& host_;
    Theme theme_;
    juce::LookAndFeel_V4 lnf_;
    ynot::LayoutDecision layout_{};

    juce::TextButton newButton_{"New Demo"}, openButton_{"Open"}, saveButton_{"Save"};
    juce::TextButton playButton_{"Play"}, recordButton_{"Record"}, clickButton_{"Click"};
    juce::TextButton audioButton_{"Audio"}, scanButton_{"Scan Plugins"};
    juce::Label status_;
    juce::ListBox tracks_;
    juce::Label compatTitle_;
    juce::ComboBox target_;
    juce::TextButton prepareButton_{"Prepare for iPhone & Android"};
    juce::TextEditor compat_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace ynotapp
