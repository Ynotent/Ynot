// The mixer: one channel strip per track plus the master. Faders, pan, mute and solo change
// the sound immediately (Session's live mix path); meters show post-fader peak levels.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class MixerView : public juce::Component, private juce::Timer {
public:
    MixerView(Session& session, const Theme& theme);
    ~MixerView() override;

    void refresh();  // tracks or their settings changed
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Meter scale shared by strips (and tests): -60 dB..+6 dB mapped to 0..1.
    static float meterPosition(float linearPeak);

private:
    class Meter;
    class Strip;
    void timerCallback() override;

    Session& session_;
    const Theme& theme_;
    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<Strip>> strips_;
    std::unique_ptr<Strip> master_;
};

}  // namespace ynotapp
