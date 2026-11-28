// Audio interface controls: device, sample rate, buffer size (with latency), main output pair,
// and a live level meter for every input.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class AudioIOPanel : public juce::Component, private juce::Timer {
public:
    AudioIOPanel(Session& session, const Theme& theme);
    ~AudioIOPanel() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    void refresh();  // device changed

    static void show(Session& session, const Theme& theme);
    static juce::String bufferLabel(int frames, double sampleRate);  // "256 samples (5.3 ms)"

private:
    void timerCallback() override;
    void showDeviceSelector();

    Session& session_;
    const Theme& theme_;
    juce::Label deviceLabel_, latency_, inputsTitle_{{}, "Inputs"};
    juce::Label rateLabel_{{}, "Sample rate"}, bufferLabel_{{}, "Buffer size"}, outputLabel_{{}, "Main output"};
    juce::ComboBox rate_, buffer_, output_;
    juce::TextButton changeDevice_{"Change device..."};
    std::vector<double> rates_;
    std::vector<int> buffers_;
    std::vector<float> inputLevels_;
    juce::Rectangle<int> meterArea_;
    double lastRate_ = 0;
    int lastBuffer_ = 0;
    juce::String lastDevice_;
};

}  // namespace ynotapp
