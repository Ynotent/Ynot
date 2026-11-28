// Ynot Sampler panel: the loaded sample's waveform with its slices, and 16 pads.
//
//   * Chop mode: each slice plays from a pad (note 36 up: the first pad bank on an Akai MPK
//     and most controllers). Drag a slice line to move it, double-click to add one,
//     right-click a line to remove it. Click a slice or a pad to hear it.
//   * Keys mode: the whole sample plays across the keyboard from the root note.
// Original Ynot design.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class SamplerView : public juce::Component {
public:
    SamplerView(Session& session, const Theme& theme);
    ~SamplerView() override;

    void open(const std::string& trackId);
    void openOrCreate();  // the focused/first sampler track
    void refresh();
    const std::string& trackId() const { return trackId_; }
    std::function<void()> onRequestNewSampler;  // "+ Sampler track" (the main window picks a file)

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void visibilityChanged() override;

    static constexpr int kPads = 16;

private:
    const ynot::PluginInstance* sampler() const;
    const ynot::AudioBuffer* sample() const;
    std::vector<int64_t> slices() const;
    bool keysMode() const;
    int baseNote() const;
    double param(const char* key, double def) const;
    void setParam(const char* key, ynot::Json value);
    float frameToX(int64_t frame) const;
    int64_t xToFrame(float x) const;
    int markerAt(float x) const;  // slice index whose line is under x (not 0), or -1
    int sliceAt(float x) const;
    juce::Rectangle<int> padBounds(int pad) const;
    void play(int note);
    void loadSample();

    Session& session_;
    const Theme& theme_;
    std::string trackId_;
    juce::Label title_, hint_;
    juce::TextButton load_{"Load sample..."}, autoChop_{"Auto-chop"}, newTrack_{"+ Sampler track"};
    juce::ComboBox mode_, equal_, root_;
    juce::Slider attack_, release_, tune_;
    juce::Label attackLabel_{{}, "Attack"}, releaseLabel_{{}, "Release"}, tuneLabel_{{}, "Tune"};
    juce::Rectangle<int> waveArea_, padArea_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int dragMarker_ = -1;
    int flashPad_ = -1;
    bool updating_ = false;
    // Waveform peaks cache.
    std::string peaksFor_;
    int peaksWidth_ = 0;
    std::vector<std::pair<float, float>> peaks_;
};

}  // namespace ynotapp
