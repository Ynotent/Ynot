// Step sequencer for drum patterns: a row per drum sound, a column per step. Click a step to
// turn it on or off, drag to paint, Shift-click for an accent, right-click for velocity. The
// pattern is an ordinary MIDI clip on a drum track, so it loops, swings and exports like
// everything else. Original Ynot design.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class StepSequencerView : public juce::Component, private juce::Timer {
public:
    StepSequencerView(Session& session, const Theme& theme);
    ~StepSequencerView() override;

    void open(const std::string& trackId, const std::string& clipId);
    void openOrCreate();  // the selected/first pattern, or a new drum track
    void refresh();       // project changed
    bool hasPattern() const;
    const std::string& trackId() const { return trackId_; }
    const std::string& clipId() const { return clipId_; }

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void visibilityChanged() override;

private:
    struct Cell {
        int row = -1, step = -1;
    };
    const ynot::MidiClip* clip() const;
    std::vector<int> rows() const;
    int steps() const;
    Cell cellAt(juce::Point<int> p) const;
    juce::Rectangle<int> cellBounds(int row, int step) const;
    juce::Rectangle<int> nameBounds(int row) const;
    void setStep(int note, int step, int velocity, bool undoStep);
    void timerCallback() override;

    Session& session_;
    const Theme& theme_;
    std::string trackId_, clipId_;
    juce::Label title_;
    juce::ComboBox bars_, grid_;
    juce::Slider swing_;
    juce::Label swingLabel_{{}, "Swing"};
    juce::TextButton play_{"Play pattern"}, clear_{"Clear"}, newDrums_{"+ Drum track"};
    juce::Rectangle<int> gridArea_;
    int paintVelocity_ = -1;  // while dragging: velocity being painted (0 = erasing)
    int lastPaintedRow_ = -1, lastPaintedStep_ = -1;
    int playStep_ = -1;
};

}  // namespace ynotapp
