// Arrangement view: ruler, track headers and clip lanes, shared by all platforms.
//
// Input is unified across mouse, keyboard and touch:
//   drag clip body ............ move (snaps to grid; can move to another track of the same kind)
//   drag clip edge ............ trim (touch gets wider edge zones)
//   double-click / double-tap . split at that point
//   right-click / long-press .. clip menu (split, delete)
//   click ruler / empty lane .. move the playhead
//   Ctrl/Cmd+wheel, trackpad pinch, two-finger pinch .. zoom; Shift+wheel / two-finger drag .. scroll
// Every edit goes through ynot_core's edit functions via Session::edit(), so it
// behaves identically on every platform and is undoable.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>

#include "Session.h"
#include "Theme.h"
#include "ynot/ui_layout.hpp"

namespace ynotapp {

class ArrangeView : public juce::Component, private juce::Timer {
public:
    ArrangeView(Session& session, JucePluginHost& host, const Theme& theme);
    ~ArrangeView() override;

    void setLayout(const ynot::LayoutDecision& layout);
    void refresh();  // project changed

    // Commands also reachable from keyboard / menus / toolbar.
    bool splitSelectedAtPlayhead();
    bool deleteSelected();
    // Selected clip (empty ids if none) and whether it's MIDI, for toolbar/menu commands.
    std::string selectedTrackId() const { return selected_.trackId; }
    std::string selectedClipId() const { return selected_.clipId; }
    bool selectedIsMidi() const;
    bool hasSelection() const { return !selected_.clipId.empty(); }
    // While the on-screen keyboard is open, letter keys play notes instead of editing.
    void setMusicalTyping(bool on) { musicalTyping_ = on; }

    bool keyPressed(const juce::KeyPress& key) override;
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    class Ruler;
    class Lanes;
    class TrackHeader;
    struct ClipKey {
        std::string trackId, clipId;
    };

    void timerCallback() override;

    // Geometry shared by ruler and lanes.
    int laneX0() const { return stacked_ ? 0 : headerWidth_; }
    double tickToX(int64_t tick) const;
    int64_t xToTick(double x) const;
    int64_t gridTicks() const;
    void zoomAround(double x, double factor);
    void scrollByPixels(double dx);

    Session& session_;
    JucePluginHost& host_;
    const Theme& theme_;
    ynot::LayoutDecision layout_{};
    bool stacked_ = false;
    int headerWidth_ = 420;
    int rowHeight_ = 64;
    int laneHeight_ = 64;
    double pixelsPerBeat_ = 60;
    double scrollBeat_ = 0;
    ClipKey selected_;
    bool musicalTyping_ = false;

    std::unique_ptr<Ruler> ruler_;
    std::unique_ptr<Lanes> lanes_;
    juce::Viewport viewport_;

    // Waveform overview per media id: min/max per 256 source frames.
    std::map<std::string, std::vector<std::pair<float, float>>> peaks_;
    const std::vector<std::pair<float, float>>* peaksFor(const std::string& mediaId);

    friend class Ruler;
    friend class Lanes;
};

}  // namespace ynotapp
