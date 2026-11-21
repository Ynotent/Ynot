#include "ArrangeView.h"

#include <cmath>

#include "ynot/edit.hpp"

namespace ynotapp {

namespace {
constexpr int kRulerHeight = 30;
constexpr int kStackBelowWidth = 560;
constexpr double kMinPixelsPerBeat = 8, kMaxPixelsPerBeat = 1200;
double ticksToBeats(int64_t t) { return static_cast<double>(t) / ynot::kTicksPerQuarter; }
}  // namespace

// ============================================================== track header

class ArrangeView::TrackHeader : public juce::Component {
public:
    explicit TrackHeader(ArrangeView& owner) : owner_(owner) {
        for (auto* b : {&input_, &arm_, &mute_, &solo_, &freeze_}) addAndMakeVisible(*b);
        arm_.setColour(juce::TextButton::buttonOnColourId, owner_.theme_.record);
        auto& session = owner_.session_;
        arm_.onClick = [this, &session] {
            if (session.isRecording()) return;  // arming changes are blocked mid-take
            session.edit([id = id_](ynot::Project& p) { if (auto* t = p.findTrack(id)) t->armed = !t->armed; });
        };
        input_.onClick = [this, &session] { session.cycleInput(id_); };
        mute_.onClick = [this, &session] {
            session.edit([id = id_](ynot::Project& p) { if (auto* t = p.findTrack(id)) t->mute = !t->mute; });
        };
        solo_.onClick = [this, &session] {
            session.edit([id = id_](ynot::Project& p) { if (auto* t = p.findTrack(id)) t->solo = !t->solo; });
        };
        freeze_.onClick = [this, &session] {
            const auto* t = session.project().findTrack(id_);
            if (!t) return;
            try {
                if (t->frozen) session.unfreeze(id_);
                else session.freeze(id_);
            } catch (const std::exception& e) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Can't freeze", e.what());
            }
        };
    }

    void update(const ynot::Track& t, bool stacked) {
        id_ = t.id;
        track_ = t;
        stacked_ = stacked;
        const bool audio = t.kind == ynot::TrackKind::Audio;
        arm_.setVisible(true);   // audio: records input; instrument: plays live MIDI and records it
        input_.setVisible(audio);
        arm_.setToggleState(t.armed, juce::dontSendNotification);
        input_.setButtonText(owner_.session_.inputLabel(t.id));
        mute_.setToggleState(t.mute, juce::dontSendNotification);
        solo_.setToggleState(t.solo, juce::dontSendNotification);
        freeze_.setButtonText(t.frozen ? "Unfreeze" : "Freeze");
        bool canFreeze = true;
        auto check = [&](const ynot::PluginInstance& p) { canFreeze &= p.bypassed || owner_.host_.isAvailable(p); };
        if (t.instrument) check(*t.instrument);
        for (auto& p : t.inserts) check(p);
        freeze_.setEnabled(t.frozen || canFreeze);
        canFreeze_ = canFreeze;
        freeze_.setVisible(!stacked);  // phones: Freeze lives in the track menu (tap the name)
        resized();
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto& th = owner_.theme_;
        auto r = getLocalBounds().reduced(4, 2);
        g.setColour(th.surfaceRaised);
        g.fillRoundedRectangle(r.toFloat(), 6.f);
        g.setColour(Theme::hex(track_.color));
        g.fillRoundedRectangle(r.removeFromLeft(6).toFloat(), 3.f);
        r.removeFromLeft(10);
        auto text = stacked_ ? r.removeFromLeft(r.getWidth() - controlsWidth()) : r.removeFromTop(r.getHeight() / 2);
        g.setColour(th.text);
        g.setFont(juce::FontOptions(th.bodySize + 1.f, juce::Font::bold));
        juce::String title = track_.name;
        if (track_.frozen) title << "  (frozen)";
        if (stacked_) title << "  \xe2\x96\xbe";  // ▾: tap for the track menu
        g.drawText(title, stacked_ ? text : text.withTrimmedTop(4), juce::Justification::centredLeft, true);
    }

    void mouseUp(const juce::MouseEvent& e) override {
        if (!stacked_ || e.mouseWasDraggedSinceMouseDown()) return;
        juce::PopupMenu m;
        m.addItem(track_.frozen ? "Unfreeze" : "Freeze", track_.frozen || canFreeze_, false,
                  [this] { freeze_.triggerClick(); });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8, 5);
        if (!stacked_) r = r.removeFromBottom(r.getHeight() / 2 + 4);  // controls under the name
        const int unit = r.getHeight();
        if (freeze_.isVisible()) freeze_.setBounds(r.removeFromRight(unit * 2));
        for (auto* b : {&solo_, &mute_, &arm_}) {
            if (!b->isVisible()) continue;
            r.removeFromRight(4);
            b->setBounds(r.removeFromRight(unit));
        }
        if (input_.isVisible()) {
            r.removeFromRight(4);
            input_.setBounds(r.removeFromRight(unit * 3 / 2));
        }
    }

private:
    int controlsWidth() const {
        const int unit = getHeight() - 10;
        int w = (freeze_.isVisible() ? unit * 4 : unit * 2) + 16;
        if (arm_.isVisible()) w += unit + 4;
        if (input_.isVisible()) w += unit * 3 / 2 + 4;
        return w;
    }

    ArrangeView& owner_;
    std::string id_;
    ynot::Track track_;
    bool stacked_ = false;
    bool canFreeze_ = true;
    juce::TextButton input_{"In 1"}, arm_{"R"}, mute_{"M"}, solo_{"S"}, freeze_{"Freeze"};
};

// ============================================================== ruler

class ArrangeView::Ruler : public juce::Component {
public:
    explicit Ruler(ArrangeView& o) : owner_(o) {}

    void paint(juce::Graphics& g) override {
        auto& th = owner_.theme_;
        g.fillAll(th.surface);
        const auto& p = owner_.session_.project();
        const int beatsPerBar = std::max(1, p.timeSigNumerator);
        const double ppb = owner_.pixelsPerBeat_;
        const int barStep = ppb * beatsPerBar < 40 ? 4 : 1;  // thin out labels when zoomed out
        const int firstBeat = std::max(0, static_cast<int>(owner_.scrollBeat_));
        const int lastBeat = static_cast<int>(owner_.scrollBeat_ + getWidth() / ppb) + 1;
        g.setFont(juce::FontOptions(th.bodySize - 1.f));
        for (int b = firstBeat; b <= lastBeat; ++b) {
            const float x = static_cast<float>(owner_.tickToX(static_cast<int64_t>(b) * ynot::kTicksPerQuarter));
            if (x < owner_.laneX0()) continue;
            const bool bar = b % beatsPerBar == 0;
            if (!bar && ppb < 14) continue;
            g.setColour(bar ? th.textMuted : th.border);
            g.drawVerticalLine(static_cast<int>(x), bar ? 4.f : 18.f, static_cast<float>(getHeight()));
            if (bar && (b / beatsPerBar) % barStep == 0)
                g.drawText(juce::String(b / beatsPerBar + 1), static_cast<int>(x) + 4, 2, 40, 16,
                           juce::Justification::left);
        }
        const float px = static_cast<float>(owner_.tickToX(owner_.session_.positionTick()));
        if (px >= owner_.laneX0()) {
            g.setColour(th.accent);
            juce::Path tri;
            tri.addTriangle(px - 6, 0, px + 6, 0, px, 10);
            g.fillPath(tri);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override { seek(e); }
    void mouseDrag(const juce::MouseEvent& e) override { seek(e); }

private:
    void seek(const juce::MouseEvent& e) {
        if (e.x < owner_.laneX0()) return;
        owner_.session_.seekToTick(ynot::snapTick(owner_.xToTick(e.x), owner_.gridTicks()));
        owner_.repaint();
    }
    ArrangeView& owner_;
};

// ============================================================== lanes

class ArrangeView::Lanes : public juce::Component, private juce::Timer {
public:
    explicit Lanes(ArrangeView& o) : owner_(o) {
        setWantsKeyboardFocus(false);  // keys go to ArrangeView
    }

    struct Row {
        const ynot::Track* track;
        juce::Rectangle<int> header, lane;
    };

    std::vector<Row> rows() const {
        std::vector<Row> out;
        int y = 0;
        for (const auto* t : owner_.session_.project().orderedTracks()) {
            Row r{t, {}, {}};
            if (owner_.stacked_) {
                r.header = {0, y, getWidth(), owner_.rowHeight_};
                r.lane = {0, y + owner_.rowHeight_, getWidth(), owner_.laneHeight_};
                y += owner_.rowHeight_ + owner_.laneHeight_ + 6;
            } else {
                r.header = {0, y, owner_.headerWidth_, owner_.rowHeight_};
                r.lane = {owner_.headerWidth_, y, getWidth() - owner_.headerWidth_, owner_.rowHeight_};
                y += owner_.rowHeight_ + 2;
            }
            out.push_back(r);
        }
        return out;
    }

    int contentHeight() const {
        const auto n = static_cast<int>(owner_.session_.project().tracks.size());
        return owner_.stacked_ ? n * (owner_.rowHeight_ + owner_.laneHeight_ + 6) : n * (owner_.rowHeight_ + 2);
    }

    void syncHeaders() {
        auto rs = rows();
        while (headers_.size() > rs.size()) headers_.pop_back();
        while (headers_.size() < rs.size()) {
            headers_.push_back(std::make_unique<TrackHeader>(owner_));
            addAndMakeVisible(*headers_.back());
        }
        for (size_t i = 0; i < rs.size(); ++i) {
            headers_[i]->setBounds(rs[i].header);
            headers_[i]->update(*rs[i].track, owner_.stacked_);
        }
    }

    void paint(juce::Graphics& g) override {
        auto& th = owner_.theme_;
        g.fillAll(th.background);
        const auto& p = owner_.session_.project();
        const int beatsPerBar = std::max(1, p.timeSigNumerator);
        for (auto& row : rows()) {
            auto lane = row.lane;
            g.setColour(th.surface);
            g.fillRect(lane);
            g.saveState();
            g.reduceClipRegion(lane);
            // Grid.
            const double ppb = owner_.pixelsPerBeat_;
            const int firstBeat = std::max(0, static_cast<int>(owner_.scrollBeat_));
            const int lastBeat = static_cast<int>(owner_.scrollBeat_ + lane.getWidth() / ppb) + 1;
            for (int b = firstBeat; b <= lastBeat; ++b) {
                if (b % beatsPerBar != 0 && ppb < 14) continue;
                g.setColour(b % beatsPerBar == 0 ? th.border : th.border.withAlpha(0.4f));
                g.drawVerticalLine(static_cast<int>(owner_.tickToX(static_cast<int64_t>(b) * ynot::kTicksPerQuarter)),
                                   static_cast<float>(lane.getY()), static_cast<float>(lane.getBottom()));
            }
            const auto color = Theme::hex(row.track->color);
            for (auto& c : row.track->audioClips)
                drawClip(g, *row.track, c.id, c.startTick, c.lengthTick, lane, color, &c, nullptr);
            for (auto& c : row.track->midiClips)
                drawClip(g, *row.track, c.id, c.startTick, c.lengthTick, lane, color, nullptr, &c);
            g.restoreState();
        }
        // Drag ghost.
        if (drag_.mode != DragMode::None && drag_.mode != DragMode::Pinch) {
            if (auto g2 = ghostRect()) {
                g.setColour(th.text.withAlpha(0.9f));
                g.drawRoundedRectangle(g2->toFloat().reduced(1.f), 4.f, 2.f);
            }
        }
        // Playhead.
        const float px = static_cast<float>(owner_.tickToX(owner_.session_.positionTick()));
        if (px >= owner_.laneX0()) {
            g.setColour(th.accent);
            g.drawVerticalLine(static_cast<int>(px), 0.f, static_cast<float>(getHeight()));
        }
    }

    // ---------------------------------------------------------------- input

    void mouseDown(const juce::MouseEvent& e) override {
        touches_[e.source.getIndex()] = e.position;
        if (touches_.size() >= 2) {  // second finger: switch to pinch/pan
            startPinch();
            return;
        }
        auto hit = hitTest(e.position);
        drag_ = {};
        drag_.start = e.position;
        if (!hit) {
            owner_.selected_ = {};
            if (laneAt(e.position)) owner_.session_.seekToTick(ynot::snapTick(owner_.xToTick(e.x), owner_.gridTicks()));
            owner_.repaint();
            return;
        }
        owner_.selected_ = {hit->trackId, hit->clipId};
        drag_.clip = *hit;
        drag_.mode = hit->zone;
        if (e.mods.isPopupMenu()) {
            drag_.mode = DragMode::None;
            showMenu(e.position);
        } else if (e.source.isTouch()) {
            longPressAt_ = e.position;
            startTimer(500);
        }
        owner_.grabKeyboardFocus();
        owner_.repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        touches_[e.source.getIndex()] = e.position;
        if (drag_.mode == DragMode::Pinch) {
            updatePinch();
            return;
        }
        if (e.getDistanceFromDragStart() > 8) stopTimer();  // not a long-press
        if (drag_.mode == DragMode::None) return;
        drag_.current = e.position;
        drag_.moved = drag_.moved || e.getDistanceFromDragStart() > 3;
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override {
        touches_.erase(e.source.getIndex());
        stopTimer();
        if (drag_.mode == DragMode::Pinch) {
            if (touches_.empty()) drag_ = {};
            return;
        }
        if (drag_.mode != DragMode::None && drag_.moved) commitDrag();
        drag_ = {};
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (auto hit = hitTest(e.position)) {
            const int64_t at = ynot::snapTick(owner_.xToTick(e.x), owner_.gridTicks());
            owner_.session_.edit([&](ynot::Project& p) { ynot::splitClip(p, hit->trackId, hit->clipId, at); });
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            owner_.zoomAround(e.x, std::pow(1.15, w.deltaY * 8.0));
        } else if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY)) {
            const float d = std::abs(w.deltaX) > 0 ? w.deltaX : w.deltaY;
            owner_.scrollByPixels(-d * 400.0);
        } else {
            Component::mouseWheelMove(e, w);  // vertical: passes up to the viewport
        }
    }

    void mouseMagnify(const juce::MouseEvent& e, float scale) override { owner_.zoomAround(e.x, scale); }

    void mouseMove(const juce::MouseEvent& e) override {
        auto hit = hitTest(e.position);
        setMouseCursor(!hit ? juce::MouseCursor::NormalCursor
                       : hit->zone == DragMode::Move ? juce::MouseCursor::DraggingHandCursor
                                                     : juce::MouseCursor::LeftRightResizeCursor);
    }

private:
    enum class DragMode { None, Move, TrimStart, TrimEnd, Pinch };
    struct Hit {
        std::string trackId, clipId;
        DragMode zone;
        int64_t startTick, lengthTick;
        bool audio;
    };
    struct Drag {
        DragMode mode = DragMode::None;
        Hit clip;
        juce::Point<float> start, current;
        bool moved = false;
        double pinchStartDist = 0, pinchStartPpb = 0, pinchStartScroll = 0, pinchAnchorX = 0;
    };

    void drawClip(juce::Graphics& g, const ynot::Track& t, const std::string& clipId, int64_t start, int64_t len,
                  juce::Rectangle<int> lane, juce::Colour color, const ynot::AudioClip* audio,
                  const ynot::MidiClip* midi) {
        auto& th = owner_.theme_;
        const float x0 = static_cast<float>(owner_.tickToX(start));
        const float x1 = static_cast<float>(owner_.tickToX(start + len));
        if (x1 < lane.getX() || x0 > lane.getRight()) return;
        juce::Rectangle<float> r(x0, static_cast<float>(lane.getY() + 3), std::max(2.f, x1 - x0),
                                 static_cast<float>(lane.getHeight() - 6));
        const bool selected = owner_.selected_.clipId == clipId && owner_.selected_.trackId == t.id;
        const bool dragging = drag_.mode != DragMode::None && drag_.moved && drag_.clip.clipId == clipId;
        g.setColour(color.withAlpha(dragging ? 0.25f : 0.35f));
        g.fillRoundedRectangle(r, 4.f);
        g.setColour(selected ? th.text : color);
        g.drawRoundedRectangle(r.reduced(0.5f), 4.f, selected ? 2.f : 1.f);
        auto inner = r.reduced(2.f, 4.f);
        g.setColour(color.brighter(0.4f));
        if (audio) {
            const auto* peaks = owner_.peaksFor(audio->mediaId);
            const auto* m = owner_.session_.project().findMedia(audio->mediaId);
            if (peaks && m && !peaks->empty()) {
                const double secondsPerTick = owner_.session_.project().secondsPerTick();
                const double gain = ynot::dbToGain(audio->gainDb);
                const float mid = inner.getCentreY(), half = inner.getHeight() / 2.f;
                const int px0 = std::max(static_cast<int>(inner.getX()), lane.getX());
                const int px1 = std::min(static_cast<int>(inner.getRight()), lane.getRight());
                for (int px = px0; px < px1; ++px) {
                    auto frameAt = [&](double x) {
                        const double tick = static_cast<double>(owner_.xToTick(x) - start);
                        return static_cast<double>(audio->sourceOffsetFrames) + tick * secondsPerTick * m->sampleRate;
                    };
                    const auto b0 = static_cast<size_t>(std::max(0.0, frameAt(px) / 256.0));
                    const auto b1 = std::max(b0 + 1, static_cast<size_t>(std::max(0.0, frameAt(px + 1) / 256.0)));
                    float lo = 0, hi = 0;
                    for (size_t b = b0; b < b1 && b < peaks->size(); ++b) {
                        lo = std::min(lo, (*peaks)[b].first);
                        hi = std::max(hi, (*peaks)[b].second);
                    }
                    g.drawVerticalLine(px, mid - hi * half * static_cast<float>(gain),
                                       mid - lo * half * static_cast<float>(gain) + 1.f);
                }
            } else {
                g.setFont(juce::FontOptions(th.bodySize - 1.f));
                g.drawText("Audio not on this device yet", inner, juce::Justification::centredLeft, true);
            }
        } else if (midi && !midi->notes.empty()) {
            int lo = 127, hi = 0;
            for (auto& n : midi->notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
            const float span = static_cast<float>(std::max(12, hi - lo + 1));
            for (auto& n : midi->notes) {
                if (n.tick < 0 || n.tick >= len) continue;  // hidden by a trim
                const float nx0 = static_cast<float>(owner_.tickToX(start + n.tick));
                const float nx1 = static_cast<float>(owner_.tickToX(start + std::min(n.tick + n.lengthTick, len)));
                const float ny = inner.getBottom() - (static_cast<float>(n.pitch - lo) + 1.f) / span * inner.getHeight();
                g.fillRect(juce::Rectangle<float>(nx0, ny, std::max(2.f, nx1 - nx0 - 1.f),
                                                  std::max(2.f, inner.getHeight() / span - 1.f)));
            }
        }
    }

    std::optional<Row> laneAt(juce::Point<float> pos) const {
        for (auto& r : rows())
            if (r.lane.toFloat().contains(pos)) return r;
        return std::nullopt;
    }

    std::optional<Hit> hitTest(juce::Point<float> pos) const {
        auto row = laneAt(pos);
        if (!row) return std::nullopt;
        const float edge = owner_.layout_.minTouchTargetDp >= 44 ? 16.f : 7.f;
        auto test = [&](const std::string& id, int64_t s, int64_t l, bool audio) -> std::optional<Hit> {
            const float x0 = static_cast<float>(owner_.tickToX(s)), x1 = static_cast<float>(owner_.tickToX(s + l));
            if (pos.x < x0 || pos.x > x1) return std::nullopt;
            const float e = std::min(edge, (x1 - x0) / 3.f);  // short clips stay movable
            DragMode zone = pos.x < x0 + e ? DragMode::TrimStart : pos.x > x1 - e ? DragMode::TrimEnd : DragMode::Move;
            return Hit{row->track->id, id, zone, s, l, audio};
        };
        // Later clips draw on top, so test them first.
        for (auto it = row->track->midiClips.rbegin(); it != row->track->midiClips.rend(); ++it)
            if (auto h = test(it->id, it->startTick, it->lengthTick, false)) return h;
        for (auto it = row->track->audioClips.rbegin(); it != row->track->audioClips.rend(); ++it)
            if (auto h = test(it->id, it->startTick, it->lengthTick, true)) return h;
        return std::nullopt;
    }

    int64_t dragDeltaTicks() const {
        const double beats = (drag_.current.x - drag_.start.x) / owner_.pixelsPerBeat_;
        return static_cast<int64_t>(std::llround(beats * ynot::kTicksPerQuarter));
    }

    // Target track for a vertical move (same kind only), else the original track.
    std::string dropTrack() const {
        for (auto& r : rows())
            if (drag_.current.y >= static_cast<float>(r.lane.getY()) && drag_.current.y < static_cast<float>(r.lane.getBottom())) {
                auto from = owner_.session_.project().findTrack(drag_.clip.trackId);
                if (from && r.track->kind == from->kind) return r.track->id;
            }
        return drag_.clip.trackId;
    }

    std::optional<juce::Rectangle<int>> ghostRect() const {
        if (!drag_.moved) return std::nullopt;
        const int64_t grid = owner_.gridTicks();
        int64_t s = drag_.clip.startTick, e = drag_.clip.startTick + drag_.clip.lengthTick;
        std::string track = drag_.clip.trackId;
        switch (drag_.mode) {
            case DragMode::Move: {
                const int64_t ns = std::max<int64_t>(0, ynot::snapTick(s + dragDeltaTicks(), grid));
                e = ns + (e - s);
                s = ns;
                track = dropTrack();
                break;
            }
            case DragMode::TrimStart: s = std::min(ynot::snapTick(s + dragDeltaTicks(), grid), e - ynot::kMinClipTicks); break;
            case DragMode::TrimEnd: e = std::max(ynot::snapTick(e + dragDeltaTicks(), grid), s + ynot::kMinClipTicks); break;
            case DragMode::None:
            case DragMode::Pinch: return std::nullopt;
        }
        for (auto& r : rows())
            if (r.track->id == track) {
                const int x0 = static_cast<int>(owner_.tickToX(std::max<int64_t>(0, s)));
                const int x1 = static_cast<int>(owner_.tickToX(e));
                return juce::Rectangle<int>(x0, r.lane.getY() + 3, std::max(2, x1 - x0), r.lane.getHeight() - 6);
            }
        return std::nullopt;
    }

    void commitDrag() {
        const auto c = drag_.clip;
        const int64_t grid = owner_.gridTicks();
        const int64_t delta = dragDeltaTicks();
        const std::string target = dropTrack();
        owner_.session_.edit([&](ynot::Project& p) {
            switch (drag_.mode) {
                case DragMode::Move:
                    ynot::moveClip(p, c.trackId, c.clipId, ynot::snapTick(c.startTick + delta, grid), target);
                    break;
                case DragMode::TrimStart:
                    ynot::trimClipStart(p, c.trackId, c.clipId, ynot::snapTick(c.startTick + delta, grid));
                    break;
                case DragMode::TrimEnd:
                    ynot::trimClipEnd(p, c.trackId, c.clipId, ynot::snapTick(c.startTick + c.lengthTick + delta, grid));
                    break;
                case DragMode::None:
                case DragMode::Pinch: break;
            }
        });
        if (drag_.mode == DragMode::Move) owner_.selected_.trackId = target;
    }

    void showMenu(juce::Point<float> at) {
        const auto c = drag_.clip;
        const int64_t tick = ynot::snapTick(owner_.xToTick(at.x), owner_.gridTicks());
        juce::PopupMenu m;
        m.addItem("Split here", [this, c, tick] {
            owner_.session_.edit([&](ynot::Project& p) { ynot::splitClip(p, c.trackId, c.clipId, tick); });
        });
        m.addItem("Split at playhead", [this] { owner_.splitSelectedAtPlayhead(); });
        m.addSeparator();
        m.addItem("Delete", [this] { owner_.deleteSelected(); });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
            juce::Rectangle<int>(1, 1).withPosition(localPointToGlobal(at.toInt()))));
    }

    // Long-press = context menu on touch screens.
    void timerCallback() override {
        stopTimer();
        if (drag_.mode == DragMode::None || drag_.moved) return;
        drag_.mode = DragMode::None;
        showMenu(longPressAt_);
    }

    void startPinch() {
        stopTimer();
        auto it = touches_.begin();
        auto a = it->second, b = (++it)->second;
        drag_ = {};
        drag_.mode = DragMode::Pinch;
        drag_.pinchStartDist = std::max(1.0, static_cast<double>(a.getDistanceFrom(b)));
        drag_.pinchStartPpb = owner_.pixelsPerBeat_;
        drag_.pinchStartScroll = owner_.scrollBeat_;
        drag_.pinchAnchorX = (a.x + b.x) / 2.0;
        repaint();
    }

    void updatePinch() {
        if (touches_.size() < 2) return;
        auto it = touches_.begin();
        auto a = it->second, b = (++it)->second;
        const double dist = std::max(1.0, static_cast<double>(a.getDistanceFrom(b)));
        const double centre = (a.x + b.x) / 2.0;
        // Keep the beat under the fingers' starting midpoint under their current midpoint.
        const double anchorBeat = drag_.pinchStartScroll + (drag_.pinchAnchorX - owner_.laneX0()) / drag_.pinchStartPpb;
        owner_.pixelsPerBeat_ = std::clamp(drag_.pinchStartPpb * dist / drag_.pinchStartDist, kMinPixelsPerBeat, kMaxPixelsPerBeat);
        owner_.scrollBeat_ = std::max(0.0, anchorBeat - (centre - owner_.laneX0()) / owner_.pixelsPerBeat_);
        owner_.repaint();
    }

    ArrangeView& owner_;
    std::vector<std::unique_ptr<TrackHeader>> headers_;
    Drag drag_;
    std::map<int, juce::Point<float>> touches_;
    juce::Point<float> longPressAt_;
};

// ============================================================== ArrangeView

ArrangeView::ArrangeView(Session& session, JucePluginHost& host, const Theme& theme)
    : session_(session), host_(host), theme_(theme) {
    ruler_ = std::make_unique<Ruler>(*this);
    lanes_ = std::make_unique<Lanes>(*this);
    addAndMakeVisible(*ruler_);
    viewport_.setViewedComponent(lanes_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    setWantsKeyboardFocus(true);
    startTimerHz(30);  // playhead
}

ArrangeView::~ArrangeView() { viewport_.setViewedComponent(nullptr, false); }

void ArrangeView::setLayout(const ynot::LayoutDecision& layout) {
    layout_ = layout;
    resized();
}

double ArrangeView::tickToX(int64_t tick) const {
    return laneX0() + (ticksToBeats(tick) - scrollBeat_) * pixelsPerBeat_;
}

int64_t ArrangeView::xToTick(double x) const {
    const double beat = scrollBeat_ + (x - laneX0()) / pixelsPerBeat_;
    return std::max<int64_t>(0, static_cast<int64_t>(std::llround(beat * ynot::kTicksPerQuarter)));
}

int64_t ArrangeView::gridTicks() const {
    // Finest musical grid whose lines stay at least ~12 px apart.
    for (int64_t div : {16, 8, 4, 2, 1}) {
        const double px = pixelsPerBeat_ / static_cast<double>(div);
        if (px >= 12) return ynot::kTicksPerQuarter / div;
    }
    return ynot::kTicksPerQuarter * std::max(1, session_.project().timeSigNumerator);  // bars
}

void ArrangeView::zoomAround(double x, double factor) {
    const double beatAtX = scrollBeat_ + (x - laneX0()) / pixelsPerBeat_;
    pixelsPerBeat_ = std::clamp(pixelsPerBeat_ * factor, kMinPixelsPerBeat, kMaxPixelsPerBeat);
    scrollBeat_ = std::max(0.0, beatAtX - (x - laneX0()) / pixelsPerBeat_);
    repaint();
}

void ArrangeView::scrollByPixels(double dx) {
    scrollBeat_ = std::max(0.0, scrollBeat_ + dx / pixelsPerBeat_);
    repaint();
}

void ArrangeView::refresh() {
    // Drop waveform caches for media no longer in the project.
    for (auto it = peaks_.begin(); it != peaks_.end();)
        it = session_.project().findMedia(it->first) ? std::next(it) : peaks_.erase(it);
    if (!session_.hasProject()) selected_ = {};
    else if (!ynot::findClip(session_.project(), selected_.trackId, selected_.clipId)) selected_ = {};
    resized();
    repaint();
}

void ArrangeView::resized() {
    const int target = static_cast<int>(std::max(layout_.minTouchTargetDp, 32.0));
    // Phones and narrow windows put the lane under the track header (full-width lanes).
    stacked_ = layout_.layout == ynot::LayoutClass::Compact || getWidth() < kStackBelowWidth;
    headerWidth_ = std::clamp(getWidth() / 3, 300, 440);
    rowHeight_ = stacked_ ? target + 12 : std::max(target * 2 + 12, 72);
    laneHeight_ = stacked_ ? std::max(target + 16, 56) : rowHeight_;
    auto r = getLocalBounds();
    ruler_->setBounds(r.removeFromTop(kRulerHeight));
    viewport_.setBounds(r);
    // Set twice: the first call may toggle the vertical scrollbar, changing the visible width.
    lanes_->setSize(viewport_.getMaximumVisibleWidth(), std::max(lanes_->contentHeight(), r.getHeight()));
    lanes_->setSize(viewport_.getMaximumVisibleWidth(), std::max(lanes_->contentHeight(), r.getHeight()));
    if (session_.hasProject()) lanes_->syncHeaders();
}

void ArrangeView::paint(juce::Graphics& g) { g.fillAll(theme_.background); }

void ArrangeView::timerCallback() {
    if (!session_.hasProject()) return;
    // Follow the playhead while playing.
    if (session_.isPlaying()) {
        const double x = tickToX(session_.positionTick());
        const double right = getWidth() - 40;
        if (x > right) scrollByPixels(x - laneX0() - (getWidth() - laneX0()) * 0.25);
    }
    ruler_->repaint();
    lanes_->repaint();
}

const std::vector<std::pair<float, float>>* ArrangeView::peaksFor(const std::string& mediaId) {
    auto it = peaks_.find(mediaId);
    if (it != peaks_.end()) return &it->second;
    const auto* audio = session_.mediaAudio(mediaId);
    if (!audio) return nullptr;
    std::vector<std::pair<float, float>> v;
    const int64_t frames = audio->frames();
    v.reserve(static_cast<size_t>(frames / 256 + 1));
    for (int64_t f = 0; f < frames; f += 256) {
        float lo = 0, hi = 0;
        for (int64_t i = f; i < std::min(frames, f + 256); ++i)
            for (int c = 0; c < audio->channels; ++c) {
                const float s = audio->samples[static_cast<size_t>(i * audio->channels + c)];
                lo = std::min(lo, s);
                hi = std::max(hi, s);
            }
        v.emplace_back(lo, hi);
    }
    return &(peaks_[mediaId] = std::move(v));
}

bool ArrangeView::splitSelectedAtPlayhead() {
    if (!hasSelection()) return false;
    auto sel = selected_;
    bool ok = false;
    const int64_t at = session_.positionTick();
    session_.edit([&](ynot::Project& p) { ok = ynot::splitClip(p, sel.trackId, sel.clipId, at).has_value(); });
    return ok;
}

bool ArrangeView::deleteSelected() {
    if (!hasSelection()) return false;
    auto sel = selected_;
    selected_ = {};
    session_.edit([&](ynot::Project& p) { ynot::deleteClip(p, sel.trackId, sel.clipId); });
    return true;
}

bool ArrangeView::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) return deleteSelected();
    if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S') {
        if (!key.getModifiers().isCommandDown()) return splitSelectedAtPlayhead();
    }
    if (key == juce::KeyPress('=', juce::ModifierKeys::commandModifier, 0)) { zoomAround(laneX0(), 1.25); return true; }
    if (key == juce::KeyPress('-', juce::ModifierKeys::commandModifier, 0)) { zoomAround(laneX0(), 0.8); return true; }
    return false;  // undo/redo/transport keys are handled by the main window
}

}  // namespace ynotapp
