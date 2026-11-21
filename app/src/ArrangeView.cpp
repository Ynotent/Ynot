#include "ArrangeView.h"

#include <cmath>

#include "AudioImporter.h"
#include "QuantizeMenus.h"
#include "ynot/edit.hpp"
#include "ynot/stretch.hpp"

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
        for (auto* b : {&input_, &arm_, &mute_, &solo_, &freeze_}) {
            addAndMakeVisible(*b);
            b->setMouseClickGrabsKeyboardFocus(false);  // keep musical typing working after clicking R/M/S
        }
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
        const bool recordingHere = track_.armed && owner_.session_.isRecording();
        g.setColour(th.surfaceRaised);
        g.fillRoundedRectangle(r.toFloat(), 6.f);
        if (recordingHere) {  // this track is capturing right now
            g.setColour(th.record.withAlpha(0.22f));
            g.fillRoundedRectangle(r.toFloat(), 6.f);
        }
        if (track_.armed) {   // armed: red outline so it's obvious which tracks will record
            g.setColour(th.record);
            g.drawRoundedRectangle(r.toFloat().reduced(1.f), 6.f, recordingHere ? 2.5f : 1.5f);
        }
        g.setColour(Theme::hex(track_.color));
        g.fillRoundedRectangle(r.removeFromLeft(6).toFloat(), 3.f);
        r.removeFromLeft(10);
        auto text = stacked_ ? r.removeFromLeft(r.getWidth() - controlsWidth()) : r.removeFromTop(r.getHeight() / 2);
        g.setColour(th.text);
        g.setFont(juce::FontOptions(th.bodySize + 1.f, juce::Font::bold));
        juce::String title = track_.name;
        if (track_.frozen) title << "  (frozen)";
        title << "  \xe2\x96\xbe";  // ▾: click/tap for the track menu (instrument, effects...)
        const auto titleArea = stacked_ ? text : text.withTrimmedTop(4);
        g.drawText(title, titleArea, juce::Justification::centredLeft, true);
        // Plugin chain after the name, e.g. "Ynot Synth > Gain > ProVerb (off)".
        juce::String chain;
        auto add = [&](const ynot::PluginInstance& pl) {
            if (chain.isNotEmpty()) chain << " > ";
            chain << juce::String(pl.name) << (pl.bypassed ? " (off)" : "");
        };
        if (track_.instrument) add(*track_.instrument);
        for (auto& pl : track_.inserts) add(pl);
        if (chain.isNotEmpty()) {
            const juce::Font titleFont(juce::FontOptions(th.bodySize + 1.f, juce::Font::bold));
            const int used = static_cast<int>(juce::GlyphArrangement::getStringWidth(titleFont, title)) + 12;
            g.setColour(th.textMuted);
            g.setFont(juce::FontOptions(th.bodySize - 1.f));
            g.drawText(chain, titleArea.withTrimmedLeft(used), juce::Justification::centredLeft, true);
        }
        if (recordingHere) {
            g.setColour(th.record);
            g.setFont(juce::FontOptions(th.bodySize, juce::Font::bold));
            g.drawText(juce::CharPointer_UTF8("\xe2\x97\x8f REC"), text.withTrimmedTop(stacked_ ? 0 : 4),
                       juce::Justification::centredRight, false);
        }
    }

    // Click the track name: instrument, effects, freeze, rename, delete.
    void mouseUp(const juce::MouseEvent& e) override {
        if (e.mouseWasDraggedSinceMouseDown()) return;
        owner_.showTrackMenu(id_, this);
    }
    void triggerFreeze() { freeze_.triggerClick(); }
    bool canFreeze() const { return canFreeze_; }

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
        auto& session = owner_.session_;
        // Punch range: red band along the bottom of the ruler (dim when punch is off).
        {
            const float x0 = static_cast<float>(owner_.tickToX(session.punchIn()));
            const float x1 = static_cast<float>(owner_.tickToX(session.punchOut()));
            if (x1 > owner_.laneX0()) {
                const float l = std::max(x0, static_cast<float>(owner_.laneX0()));
                g.setColour(th.record.withAlpha(session.punchEnabled() ? 0.85f : 0.25f));
                g.fillRect(juce::Rectangle<float>(l, static_cast<float>(getHeight() - 7), x1 - l, 7.f));
            }
        }
        const int beatsPerBar = std::max(1, p.timeSigNumerator);
        const int64_t beatTicks = p.beatTicks();
        const double beatPx = owner_.pixelsPerBeat_ * static_cast<double>(beatTicks) / ynot::kTicksPerQuarter;
        const int barStep = beatPx * beatsPerBar < 40 ? 4 : 1;  // thin out labels when zoomed out
        const int64_t firstBeat = std::max<int64_t>(0, owner_.xToTick(owner_.laneX0()) / beatTicks);
        const int64_t lastBeat = owner_.xToTick(getWidth()) / beatTicks + 1;
        g.setFont(juce::FontOptions(th.bodySize - 1.f));
        for (int64_t b = firstBeat; b <= lastBeat; ++b) {
            const float x = static_cast<float>(owner_.tickToX(b * beatTicks));
            if (x < owner_.laneX0()) continue;
            const bool bar = b % beatsPerBar == 0;
            if (!bar && beatPx < 10) continue;
            g.setColour(bar ? th.textMuted : th.border);
            g.drawVerticalLine(static_cast<int>(x), bar ? 4.f : 16.f, static_cast<float>(getHeight() - 8));
            if (bar && (b / beatsPerBar) % barStep == 0)
                g.drawText(juce::String(b / beatsPerBar + 1), static_cast<int>(x) + 4, 2, 40, 14,
                           juce::Justification::left);
        }
        // Time signature at the left edge.
        g.setColour(th.textMuted);
        g.drawText(juce::String(p.timeSigNumerator) + "/" + juce::String(p.timeSigDenominator),
                   owner_.laneX0() - 44, 2, 40, 14, juce::Justification::right);
        const float px = static_cast<float>(owner_.tickToX(session.positionTick()));
        if (px >= owner_.laneX0()) {
            g.setColour(th.accent);
            juce::Path tri;
            tri.addTriangle(px - 6, 0, px + 6, 0, px, 10);
            g.fillPath(tri);
        }
    }

    // Click/drag: move the playhead. Shift+drag: set the punch range (and turn punch on).
    void mouseDown(const juce::MouseEvent& e) override {
        punchDrag_ = e.mods.isShiftDown();
        if (punchDrag_) punchAnchor_ = snapped(e.x);
        else seek(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!punchDrag_) { seek(e); return; }
        owner_.session_.setPunchRange(punchAnchor_, snapped(e.x));
        owner_.session_.setPunchEnabled(true);
        owner_.repaint();
    }

private:
    int64_t snapped(int x) const { return ynot::snapTick(owner_.xToTick(x), owner_.gridTicks()); }
    void seek(const juce::MouseEvent& e) {
        if (e.x < owner_.laneX0()) return;
        owner_.session_.seekToTick(snapped(e.x));
        owner_.repaint();
    }
    ArrangeView& owner_;
    bool punchDrag_ = false;
    int64_t punchAnchor_ = 0;
};

// ============================================================== lanes

class ArrangeView::Lanes : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer {
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
            // Grid: beats and bars of the project's time signature.
            const int64_t beatTicks = p.beatTicks();
            const double beatPx = owner_.pixelsPerBeat_ * static_cast<double>(beatTicks) / ynot::kTicksPerQuarter;
            const int64_t firstBeat = std::max<int64_t>(0, owner_.xToTick(lane.getX()) / beatTicks);
            const int64_t lastBeat = owner_.xToTick(lane.getRight()) / beatTicks + 1;
            for (int64_t b = firstBeat; b <= lastBeat; ++b) {
                if (b % beatsPerBar != 0 && beatPx < 10) continue;
                g.setColour(b % beatsPerBar == 0 ? th.border : th.border.withAlpha(0.4f));
                g.drawVerticalLine(static_cast<int>(owner_.tickToX(b * beatTicks)),
                                   static_cast<float>(lane.getY()), static_cast<float>(lane.getBottom()));
            }
            // Punch range: faint red wash on armed tracks so you can see what will be replaced.
            if (owner_.session_.punchEnabled() && row.track->armed) {
                const float px0 = static_cast<float>(owner_.tickToX(owner_.session_.punchIn()));
                const float px1 = static_cast<float>(owner_.tickToX(owner_.session_.punchOut()));
                g.setColour(th.record.withAlpha(0.08f));
                g.fillRect(juce::Rectangle<float>(px0, static_cast<float>(lane.getY()), px1 - px0,
                                                  static_cast<float>(lane.getHeight())));
            }
            const auto color = Theme::hex(row.track->color);
            for (auto& c : row.track->audioClips)
                drawClip(g, *row.track, c.id, c.startTick, c.lengthTick, lane, color, &c, nullptr);
            for (auto& c : row.track->midiClips)
                drawClip(g, *row.track, c.id, c.startTick, c.lengthTick, lane, color, nullptr, &c);
            // Recording in progress: a red region grows from the record point to the playhead.
            if (row.track->armed && owner_.session_.isRecording()) {
                const float x0 = static_cast<float>(owner_.tickToX(owner_.session_.recordStartTick()));
                const float x1 = static_cast<float>(owner_.tickToX(
                    std::min(owner_.session_.positionTick(), owner_.session_.recordEndTick())));
                if (x1 > x0) {
                    juce::Rectangle<float> rr(x0, static_cast<float>(lane.getY() + 3), x1 - x0,
                                              static_cast<float>(lane.getHeight() - 6));
                    g.setColour(th.record.withAlpha(0.28f));
                    g.fillRoundedRectangle(rr, 4.f);
                    g.setColour(th.record);
                    g.drawRoundedRectangle(rr.reduced(0.5f), 4.f, 1.5f);
                }
            }
            g.restoreState();
        }
        // Drag ghost.
        if (drag_.mode != DragMode::None && drag_.mode != DragMode::Pinch) {
            if (auto g2 = ghostRect()) {
                g.setColour(th.text.withAlpha(0.9f));
                g.drawRoundedRectangle(g2->toFloat().reduced(1.f), 4.f, 2.f);
            }
        }
        if (dropHint_) {  // where a dropped file will start
            const float dx = static_cast<float>(owner_.tickToX(
                ynot::snapTick(owner_.xToTick(dropHint_->x), owner_.gridTicks())));
            g.setColour(th.secondary);
            g.drawLine(dx, 0.f, dx, static_cast<float>(getHeight()), 2.f);
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
        auto hit = clipAt(e.position);
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
        // Option/Alt + drag the right edge = time stretch (audio clips).
        if (hit->zone == DragMode::TrimEnd && hit->audio && e.mods.isAltDown()) drag_.mode = DragMode::Stretch;
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
        if (drag_.mode == DragMode::Stretch && drag_.moved) commitStretchDrag();
        else if (drag_.mode != DragMode::None && drag_.moved) commitDrag();
        drag_ = {};
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (auto hit = clipAt(e.position)) {
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

    // Drop audio files from Finder/Explorer: onto an audio track = that track at the drop
    // point; anywhere else = a new track per file.
    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (auto& f : files)
            if (looksLikeImportableFile(juce::File(f))) return true;
        return false;
    }
    void fileDragMove(const juce::StringArray&, int x, int y) override {
        dropHint_ = {static_cast<float>(x), static_cast<float>(y)};
        repaint();
    }
    void fileDragExit(const juce::StringArray&) override {
        dropHint_.reset();
        repaint();
    }
    void filesDropped(const juce::StringArray& paths, int x, int y) override {
        dropHint_.reset();
        repaint();
        juce::Array<juce::File> files;
        for (auto& p : paths)
            if (looksLikeImportableFile(juce::File(p))) files.add(juce::File(p));
        std::string trackId;
        if (auto row = laneAt({static_cast<float>(x), static_cast<float>(y)}))
            if (row->track->kind == ynot::TrackKind::Audio && files.size() == 1) trackId = row->track->id;
        const int64_t tick = ynot::snapTick(owner_.xToTick(std::max(x, owner_.laneX0())), owner_.gridTicks());
        if (owner_.onImportFiles) owner_.onImportFiles(files, trackId, tick);
    }

    void mouseMove(const juce::MouseEvent& e) override {
        auto hit = clipAt(e.position);
        setMouseCursor(!hit ? juce::MouseCursor::NormalCursor
                       : hit->zone == DragMode::Move ? juce::MouseCursor::DraggingHandCursor
                                                     : juce::MouseCursor::LeftRightResizeCursor);
    }

private:
    enum class DragMode { None, Move, TrimStart, TrimEnd, Stretch, Pinch };
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
            if (const auto mode = ynot::clipStretchMode(*audio)) {  // e.g. "150% Poly"
                const auto label = juce::String(static_cast<int>(std::lround(ynot::clipStretchRatio(*audio) * 100))) +
                                   "% " + ynot::stretchModeName(*mode);
                g.setFont(juce::FontOptions(th.bodySize - 2.f));
                auto tag = inner.withHeight(th.bodySize + 2.f).withTrimmedLeft(2.f);
                g.setColour(th.text.withAlpha(0.85f));
                g.drawText(label, tag, juce::Justification::topLeft, true);
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

    std::optional<Hit> clipAt(juce::Point<float> pos) const {
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
            case DragMode::Stretch: e = s + stretchedLength(); break;
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
                case DragMode::Stretch:
                case DragMode::None:
                case DragMode::Pinch: break;
            }
        });
        if (drag_.mode == DragMode::Move) owner_.selected_.trackId = target;
    }

    // ---- time stretch
    const ynot::AudioClip* audioClip(const std::string& trackId, const std::string& clipId) const {
        if (const auto* t = owner_.session_.project().findTrack(trackId))
            for (auto& a : t->audioClips)
                if (a.id == clipId) return &a;
        return nullptr;
    }
    // New length while Option-dragging, kept inside the 25%..400% range of the original.
    int64_t stretchedLength() const {
        const auto c = drag_.clip;
        const auto* a = audioClip(c.trackId, c.clipId);
        const double cur = a ? ynot::clipStretchRatio(*a) : 1.0;
        const double original = static_cast<double>(c.lengthTick) / cur;
        const int64_t want = ynot::snapTick(c.startTick + c.lengthTick + dragDeltaTicks(), owner_.gridTicks()) - c.startTick;
        return std::clamp<int64_t>(want, static_cast<int64_t>(std::ceil(original * ynot::kMinStretchRatio)),
                                   static_cast<int64_t>(std::floor(original * ynot::kMaxStretchRatio)));
    }
    void commitStretchDrag() {
        const auto c = drag_.clip;
        const auto* a = audioClip(c.trackId, c.clipId);
        if (!a || c.lengthTick <= 0) return;
        const double ratio = ynot::clipStretchRatio(*a) * static_cast<double>(stretchedLength()) /
                             static_cast<double>(c.lengthTick);
        owner_.stretchClip(c.trackId, c.clipId, ratio, ynot::clipStretchMode(*a).value_or(owner_.session_.defaultStretchMode()));
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
        if (!c.audio)
            m.addSubMenu("Quantize", quantizePresetMenu({}, [this, c](const std::string& id) {
                owner_.session_.quantizeClip(c.trackId, c.clipId, id);
            }, false));
        if (const auto* a = c.audio ? audioClip(c.trackId, c.clipId) : nullptr)
            m.addSubMenu("Time stretch", stretchMenu(c, *a));
        m.addItem("Set punch range to this clip", [this, c] {
            owner_.session_.setPunchRange(c.startTick, c.startTick + c.lengthTick);
            owner_.session_.setPunchEnabled(true);
            owner_.repaint();
        });
        m.addSeparator();
        m.addItem("Delete", [this] { owner_.deleteSelected(); });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
            juce::Rectangle<int>(1, 1).withPosition(localPointToGlobal(at.toInt()))));
    }

    juce::PopupMenu stretchMenu(const Hit& c, const ynot::AudioClip& a) {
        auto& session = owner_.session_;
        const double cur = ynot::clipStretchRatio(a);
        const auto clipMode = ynot::clipStretchMode(a);
        const auto mode = clipMode.value_or(session.defaultStretchMode());
        juce::PopupMenu m;
        m.addSectionHeader("Mode");
        struct M { ynot::StretchMode mode; const char* label; };
        for (auto [md, label] : {M{ynot::StretchMode::Mono, "Mono: vocals, bass, solo instruments"},
                                 M{ynot::StretchMode::Poly, "Poly: chords, pads, full mixes, loops"},
                                 M{ynot::StretchMode::Varispeed, "Varispeed: tape speed (pitch follows)"}})
            m.addItem(label, true, md == mode, [this, &session, c, cur, clipMode, md = md] {
                session.setDefaultStretchMode(md);
                if (clipMode && *clipMode != md) owner_.stretchClip(c.trackId, c.clipId, cur, md);  // re-render
            });
        m.addSectionHeader("Length (of the original)");
        for (double r : {0.5, 2.0 / 3.0, 0.75, 1.25, 1.5, 2.0}) {
            const auto pct = juce::String(static_cast<int>(std::lround(r * 100))) + "%";
            const char* hint = r < 0.51 ? "  (double speed)" : r > 1.99 ? "  (half speed)" : "";
            m.addItem(pct + hint, true, std::abs(r - cur) < 1e-3,
                      [this, c, r, mode] { owner_.stretchClip(c.trackId, c.clipId, r, mode); });
        }
        m.addItem("Fit to whole bars", [this, c, mode] {
            if (const auto* x = audioClip(c.trackId, c.clipId))
                owner_.stretchClip(c.trackId, c.clipId, ynot::ratioToFitBars(owner_.session_.project(), *x), mode);
        });
        m.addItem("Match tempo...", [this, c, mode] { owner_.matchTempoDialog(c.trackId, c.clipId, mode); });
        m.addItem("Custom...", [this, c, mode, cur] { owner_.customStretchDialog(c.trackId, c.clipId, mode, cur); });
        m.addSeparator();
        m.addItem("Reset to original length", clipMode.has_value(), false,
                  [this, c, mode] { owner_.stretchClip(c.trackId, c.clipId, 1.0, mode); });
        m.addItem("Tip: Option-drag (Alt on Windows) the clip's right edge", false, false, [] {});
        return m;
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
    std::optional<juce::Point<float>> dropHint_;
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
    return session_.project().barTicks();  // bars
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

namespace {
// Installed plugins as a menu, grouped by manufacturer.
juce::PopupMenu pluginBrowserMenu(const std::vector<juce::PluginDescription>& plugins,
                                  std::function<void(const juce::PluginDescription&)> onPick) {
    juce::PopupMenu m;
    if (plugins.empty()) {
        m.addItem("No plugins found yet: use Plugins > Scan for plugins", false, false, [] {});
        return m;
    }
    juce::String maker;
    juce::PopupMenu group;
    auto flush = [&] {
        if (maker.isNotEmpty()) m.addSubMenu(maker, group);
        group = juce::PopupMenu();
    };
    for (const auto& d : plugins) {
        const auto mk = d.manufacturerName.isEmpty() ? juce::String("Other") : d.manufacturerName;
        if (mk != maker) {
            flush();
            maker = mk;
        }
        group.addItem(d.name + "  (" + d.pluginFormatName + ")", [onPick, d] { onPick(d); });
    }
    flush();
    return m;
}
}  // namespace

void ArrangeView::showTrackMenu(const std::string& trackId, juce::Component* target) {
    const auto* t = session_.project().findTrack(trackId);
    if (!t) return;
    const ynot::Track track = *t;  // snapshot: the menu outlives this call
    auto& session = session_;
    auto& host = host_;
    juce::PopupMenu m;
    m.addSectionHeader(track.name);

    auto pluginItems = [&](const ynot::PluginInstance& pl, bool isInstrument) {
        juce::PopupMenu sub;
        const bool builtin = pl.format == ynot::PluginFormat::Builtin;
        sub.addItem(builtin ? "Open window (built-ins have no window yet)" : "Open window", !builtin, false,
                    [&host, id = pl.id] { host.openEditor(id); });
        sub.addItem("Bypass", true, pl.bypassed, [&session, tid = track.id, pid = pl.id, b = pl.bypassed] {
            session.edit([&](ynot::Project& p) { ynot::setPluginBypassed(p, tid, pid, !b); });
        });
        if (!isInstrument) {
            int index = 0;
            for (size_t i = 0; i < track.inserts.size(); ++i)
                if (track.inserts[i].id == pl.id) index = static_cast<int>(i);
            sub.addItem("Move up", index > 0, false, [&session, tid = track.id, pid = pl.id, index] {
                session.edit([&](ynot::Project& p) { ynot::moveInsert(p, tid, pid, index - 1); });
            });
            sub.addItem("Move down", index + 1 < static_cast<int>(track.inserts.size()), false,
                        [&session, tid = track.id, pid = pl.id, index] {
                            session.edit([&](ynot::Project& p) { ynot::moveInsert(p, tid, pid, index + 1); });
                        });
        }
        sub.addSeparator();
        sub.addItem("Remove", [&session, tid = track.id, pid = pl.id] {
            session.edit([&](ynot::Project& p) { ynot::removePlugin(p, tid, pid); });
        });
        return sub;
    };

    if (track.kind == ynot::TrackKind::Instrument) {
        if (track.instrument)
            m.addSubMenu("Instrument: " + juce::String(track.instrument->name), pluginItems(*track.instrument, true));
        juce::PopupMenu choose;
        choose.addItem("Ynot Synth (built-in)", [&session, tid = track.id] {
            session.edit([&](ynot::Project& p) { ynot::setInstrument(p, tid, ynot::makeBuiltin(ynot::kBuiltinSynth, "Ynot Synth")); });
        });
        choose.addSeparator();
        auto installed = pluginBrowserMenu(host.availablePlugins(true), [&session, &host, tid = track.id](const juce::PluginDescription& d) {
            if (auto inst = host.makeInstance(d))
                session.edit([&](ynot::Project& p) { ynot::setInstrument(p, tid, *inst); });
        });
        choose.addSubMenu("Installed instruments", installed);
        m.addSubMenu(track.instrument ? "Change instrument" : "Choose instrument", choose);
        m.addSeparator();
    }

    if (!track.inserts.empty()) {
        m.addSectionHeader("Effects");
        for (const auto& pl : track.inserts)
            m.addSubMenu(juce::String(pl.name) + (pl.bypassed ? " (off)" : ""), pluginItems(pl, false));
    }
    if (track.kind == ynot::TrackKind::Audio)
        m.addItem("Import audio here (at the playhead)...", [this, tid = track.id] {
            if (onImportRequest) onImportRequest(tid);
        });
    juce::PopupMenu addFx;
    addFx.addItem("Gain (built-in)", [&session, tid = track.id] {
        session.edit([&](ynot::Project& p) { ynot::addInsert(p, tid, ynot::makeBuiltin(ynot::kBuiltinGain, "Gain")); });
    });
    addFx.addItem("Lowpass filter (built-in)", [&session, tid = track.id] {
        session.edit([&](ynot::Project& p) { ynot::addInsert(p, tid, ynot::makeBuiltin(ynot::kBuiltinFilter, "Lowpass")); });
    });
    addFx.addSeparator();
    addFx.addSubMenu("Installed effects",
                     pluginBrowserMenu(host.availablePlugins(false), [&session, &host, tid = track.id](const juce::PluginDescription& d) {
                         if (auto inst = host.makeInstance(d))
                             session.edit([&](ynot::Project& p) { ynot::addInsert(p, tid, *inst); });
                     }));
    m.addSubMenu("Add effect", addFx);
    m.addSeparator();

    if (auto* header = dynamic_cast<TrackHeader*>(target))
        m.addItem(track.frozen ? "Unfreeze" : "Freeze", track.frozen || header->canFreeze(), false,
                  [safe = juce::Component::SafePointer<TrackHeader>(header)] {
                      if (safe) safe->triggerFreeze();
                  });
    m.addItem("Rename...", [this, tid = track.id, name = track.name] { renameTrackDialog(tid, name); });
    m.addItem("Delete track", [&session, tid = track.id] {
        session.edit([&](ynot::Project& p) { ynot::removeTrack(p, tid); });
    });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target));
}

void ArrangeView::renameTrackDialog(const std::string& trackId, const std::string& current) {
    auto* w = new juce::AlertWindow("Rename track", {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", current);
    w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ArrangeView> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, trackId](int result) {
        if (result == 1 && safe) {
            const auto name = w->getTextEditorContents("name").trim().toStdString();
            safe->session_.edit([&](ynot::Project& p) { ynot::renameTrack(p, trackId, name); });
        }
    }), true);
}

void ArrangeView::stretchClip(const std::string& trackId, const std::string& clipId, double ratio,
                              ynot::StretchMode mode) {
    session_.setDefaultStretchMode(mode);
    session_.stretchClip(trackId, clipId, ratio, mode, [](const juce::String& err) {
        if (err.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Time stretch", err);
    });
}

void ArrangeView::matchTempoDialog(const std::string& trackId, const std::string& clipId, ynot::StretchMode mode) {
    auto* w = new juce::AlertWindow("Match tempo",
                                    "What tempo was this audio recorded at? It will be stretched to play at " +
                                        juce::String(session_.project().tempoBpm, 1) + " BPM.",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("bpm", "", "Original BPM");
    w->addButton("Stretch", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ArrangeView> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, trackId, clipId, mode](int result) {
        if (result != 1 || !safe) return;
        const double bpm = w->getTextEditorContents("bpm").trim().getDoubleValue();
        if (bpm < 20 || bpm > 400) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Match tempo",
                                                   "Enter a tempo between 20 and 400 BPM.");
            return;
        }
        safe->stretchClip(trackId, clipId, ynot::ratioForTempo(safe->session_.project(), bpm), mode);
    }), true);
}

void ArrangeView::customStretchDialog(const std::string& trackId, const std::string& clipId, ynot::StretchMode mode,
                                      double currentRatio) {
    auto* w = new juce::AlertWindow("Time stretch", "New length as a percentage of the original (25 to 400).",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("pct", juce::String(currentRatio * 100.0, 1), "Percent");
    w->addButton("Stretch", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ArrangeView> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, trackId, clipId, mode](int result) {
        if (result != 1 || !safe) return;
        const double pct = w->getTextEditorContents("pct").retainCharacters("0123456789.").getDoubleValue();
        if (pct < 25 || pct > 400) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Time stretch",
                                                   "Enter a length between 25% and 400%.");
            return;
        }
        safe->stretchClip(trackId, clipId, pct / 100.0, mode);
    }), true);
}

bool ArrangeView::selectedIsMidi() const {
    auto span = ynot::findClip(session_.project(), selected_.trackId, selected_.clipId);
    return span && !span->audio;
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
    if (!musicalTyping_ && (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S')) {
        if (!key.getModifiers().isCommandDown()) return splitSelectedAtPlayhead();
    }
    if (key == juce::KeyPress('=', juce::ModifierKeys::commandModifier, 0)) { zoomAround(laneX0(), 1.25); return true; }
    if (key == juce::KeyPress('-', juce::ModifierKeys::commandModifier, 0)) { zoomAround(laneX0(), 0.8); return true; }
    return false;  // undo/redo/transport keys are handled by the main window
}

}  // namespace ynotapp
