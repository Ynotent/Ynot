#include "StepSequencerView.h"

#include "ynot/drums.hpp"
#include "ynot/edit.hpp"

namespace ynotapp {

namespace {
constexpr int kHeaderH = 40, kNameW = 112, kRowH = 26;
constexpr int kDefaultVelocity = 100, kAccent = 127, kSoft = 60;

struct GridChoice {
    const char* name;
    int64_t ticks;
};
const GridChoice kGrids[] = {{"1/8", ynot::kTicksPerQuarter / 2},
                             {"1/16", ynot::kTicksPerQuarter / 4},
                             {"1/32", ynot::kTicksPerQuarter / 8},
                             {"1/8 triplet", ynot::kTicksPerQuarter / 3},
                             {"1/16 triplet", ynot::kTicksPerQuarter / 6}};
const int kBarChoices[] = {1, 2, 4, 8};

juce::String noteName(int note) {
    if (const char* d = ynot::drumName(note)) return d;
    return juce::MidiMessage::getMidiNoteName(note, true, true, 4);
}
}  // namespace

StepSequencerView::StepSequencerView(Session& session, const Theme& theme) : session_(session), theme_(theme) {
    title_.setFont(juce::FontOptions(theme.bodySize + 1.f, juce::Font::bold));
    addAndMakeVisible(title_);
    for (size_t i = 0; i < std::size(kBarChoices); ++i)
        bars_.addItem(juce::String(kBarChoices[i]) + (kBarChoices[i] == 1 ? " bar" : " bars"), static_cast<int>(i) + 1);
    bars_.onChange = [this] {
        const int i = bars_.getSelectedId() - 1;
        if (i < 0 || !hasPattern()) return;
        session_.edit([this, i](ynot::Project& p) { ynot::setPatternBars(p, trackId_, clipId_, kBarChoices[i]); });
    };
    for (size_t i = 0; i < std::size(kGrids); ++i) grid_.addItem(kGrids[i].name, static_cast<int>(i) + 1);
    grid_.onChange = [this] {
        const int i = grid_.getSelectedId() - 1;
        if (i < 0 || !hasPattern()) return;
        session_.edit([this, i](ynot::Project& p) { ynot::setPatternStepTicks(p, trackId_, clipId_, kGrids[i].ticks); });
    };
    swing_.setSliderStyle(juce::Slider::LinearHorizontal);
    swing_.setRange(50, 75, 1);
    swing_.setTextValueSuffix("%");
    swing_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 22);
    swing_.setColour(juce::Slider::trackColourId, theme.secondary);
    swing_.setTooltip("Swing: delays every second step. 50% straight, 54-62% subtle, 66% shuffle, 75% dotted.");
    swing_.onDragStart = [this] { session_.recordUndoStep(); };
    swing_.onValueChange = [this] {
        if (!hasPattern() || !swing_.isMouseButtonDown()) return;
        const double v = swing_.getValue();
        session_.edit([this, v](ynot::Project& p) { ynot::setPatternSwing(p, trackId_, clipId_, v); }, false);
    };
    swingLabel_.setColour(juce::Label::textColourId, theme.textMuted);
    play_.setClickingTogglesState(false);
    play_.onClick = [this] {
        if (!hasPattern()) return;
        if (session_.isPlaying()) {
            session_.stop();
        } else if (const auto* c = clip()) {
            session_.setLoopRange(c->startTick, c->startTick + c->lengthTick);
            session_.setLoopEnabled(true);
            session_.seekToTick(c->startTick);
            session_.play();
        }
    };
    clear_.onClick = [this] {
        if (!hasPattern()) return;
        session_.edit([this](ynot::Project& p) {
            for (int note : rows()) ynot::clearPatternRow(p, trackId_, clipId_, note);
        });
    };
    newDrums_.onClick = [this] {
        if (!session_.hasProject()) return;
        const int64_t bar = session_.positionTick() / session_.project().barTicks() * session_.project().barTicks();
        std::string id;
        session_.edit([&](ynot::Project& p) { id = ynot::addDrumTrack(p, bar, 1); });
        if (const auto* t = session_.project().findTrack(id); t && !t->midiClips.empty()) open(id, t->midiClips[0].id);
    };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&bars_, &grid_, &swing_, &swingLabel_, &play_,
                                                                       &clear_, &newDrums_}) {
        c->setMouseClickGrabsKeyboardFocus(false);
        addAndMakeVisible(*c);
    }
    startTimerHz(30);
}

StepSequencerView::~StepSequencerView() {
    stopTimer();
    session_.setAuditionTrack({});
}

const ynot::MidiClip* StepSequencerView::clip() const {
    if (!session_.hasProject()) return nullptr;
    if (const auto* t = session_.project().findTrack(trackId_))
        for (const auto& c : t->midiClips)
            if (c.id == clipId_) return &c;
    return nullptr;
}

bool StepSequencerView::hasPattern() const { return clip() != nullptr; }

std::vector<int> StepSequencerView::rows() const {
    std::vector<int> r;
    const auto* c = clip();
    if (!c) return r;
    if (c->steps) r = c->steps->rows;
    if (r.empty())
        for (const auto& d : ynot::drumKitSounds()) r.push_back(d.note);
    for (const auto& n : c->notes)  // notes played in from a keyboard get a row too
        if (std::find(r.begin(), r.end(), n.pitch) == r.end()) r.push_back(n.pitch);
    return r;
}

int StepSequencerView::steps() const {
    const auto* c = clip();
    return c ? ynot::stepCount(c->steps.value_or(ynot::StepPattern{}), c->lengthTick) : 0;
}

void StepSequencerView::open(const std::string& trackId, const std::string& clipId) {
    trackId_ = trackId;
    clipId_ = clipId;
    session_.setAuditionTrack(trackId);
    refresh();
}

void StepSequencerView::openOrCreate() {
    if (hasPattern() || !session_.hasProject()) {
        refresh();
        return;
    }
    for (const auto* t : session_.project().orderedTracks())  // first existing pattern
        for (const auto& c : t->midiClips)
            if (c.steps) {
                open(t->id, c.id);
                return;
            }
    refresh();
}

void StepSequencerView::refresh() {
    const auto* c = clip();
    if (c) {
        const auto* t = session_.project().findTrack(trackId_);
        const int64_t bar = c->startTick / session_.project().barTicks() + 1;
        title_.setText(juce::String(t->name) + "  |  pattern at bar " + juce::String(bar), juce::dontSendNotification);
        const auto pat = c->steps.value_or(ynot::StepPattern{});
        const int64_t b = c->lengthTick / std::max<int64_t>(1, session_.project().barTicks());
        bars_.setSelectedId(0, juce::dontSendNotification);
        for (size_t i = 0; i < std::size(kBarChoices); ++i)
            if (kBarChoices[i] == b) bars_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
        grid_.setSelectedId(0, juce::dontSendNotification);
        for (size_t i = 0; i < std::size(kGrids); ++i)
            if (kGrids[i].ticks == pat.stepTicks) grid_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
        if (!swing_.isMouseButtonDown()) swing_.setValue(pat.swingPercent, juce::dontSendNotification);
    } else {
        title_.setText("No pattern open. Add a drum track, or double-click a pattern in the arrangement.",
                       juce::dontSendNotification);
    }
    for (juce::Component* x : std::initializer_list<juce::Component*>{&bars_, &grid_, &swing_, &play_, &clear_})
        x->setEnabled(c != nullptr);
    resized();
    repaint();
}

juce::Rectangle<int> StepSequencerView::nameBounds(int row) const {
    return {gridArea_.getX(), gridArea_.getY() + row * kRowH, kNameW - 8, kRowH - 3};
}

juce::Rectangle<int> StepSequencerView::cellBounds(int row, int step) const {
    const int n = std::max(1, steps());
    const float w = static_cast<float>(gridArea_.getWidth() - kNameW) / static_cast<float>(n);
    const int x0 = gridArea_.getX() + kNameW + static_cast<int>(std::floor(w * static_cast<float>(step)));
    const int x1 = gridArea_.getX() + kNameW + static_cast<int>(std::floor(w * static_cast<float>(step + 1)));
    return {x0, gridArea_.getY() + row * kRowH, std::max(2, x1 - x0 - 3), kRowH - 3};
}

StepSequencerView::Cell StepSequencerView::cellAt(juce::Point<int> p) const {
    if (!gridArea_.contains(p) || p.x < gridArea_.getX() + kNameW) return {};
    const int row = (p.y - gridArea_.getY()) / kRowH;
    if (row < 0 || row >= static_cast<int>(rows().size())) return {};
    const int n = std::max(1, steps());
    const float w = static_cast<float>(gridArea_.getWidth() - kNameW) / static_cast<float>(n);
    const int step = static_cast<int>(static_cast<float>(p.x - gridArea_.getX() - kNameW) / w);
    if (step < 0 || step >= n) return {};
    return {row, step};
}

void StepSequencerView::setStep(int note, int step, int velocity, bool undoStep) {
    session_.edit([&](ynot::Project& p) { ynot::setStep(p, trackId_, clipId_, note, step, velocity); }, undoStep);
}

void StepSequencerView::mouseDown(const juce::MouseEvent& e) {
    const auto r = rows();
    for (int i = 0; i < static_cast<int>(r.size()); ++i)  // click a name: hear the sound
        if (nameBounds(i).contains(e.getPosition())) {
            session_.auditionNote(r[static_cast<size_t>(i)], kDefaultVelocity);
            return;
        }
    const auto cell = cellAt(e.getPosition());
    const auto* c = clip();
    if (cell.row < 0 || !c) return;
    const int note = r[static_cast<size_t>(cell.row)];
    const int current = ynot::stepVelocity(*c, note, cell.step);
    if (e.mods.isPopupMenu()) {
        juce::PopupMenu m;
        m.addItem("Accent (127)", true, current == kAccent, [this, note, s = cell.step] { setStep(note, s, kAccent, true); });
        m.addItem("Normal (100)", true, current == kDefaultVelocity,
                  [this, note, s = cell.step] { setStep(note, s, kDefaultVelocity, true); });
        m.addItem("Soft (60)", true, current == kSoft, [this, note, s = cell.step] { setStep(note, s, kSoft, true); });
        m.addItem("Off", current > 0, false, [this, note, s = cell.step] { setStep(note, s, 0, true); });
        m.addSeparator();
        m.addItem("Clear this row", [this, note] {
            session_.edit([this, note](ynot::Project& p) { ynot::clearPatternRow(p, trackId_, clipId_, note); });
        });
        m.showMenuAsync(juce::PopupMenu::Options());
        return;
    }
    // Drag paints: starting on an empty step turns steps on, starting on a lit one turns them off.
    paintVelocity_ = current > 0 ? 0 : e.mods.isShiftDown() ? kAccent : e.mods.isAltDown() ? kSoft : kDefaultVelocity;
    lastPaintedRow_ = cell.row;
    lastPaintedStep_ = cell.step;
    setStep(note, cell.step, paintVelocity_, true);  // one undo step for the whole drag
    if (paintVelocity_ > 0 && !session_.isPlaying()) session_.auditionNote(note, paintVelocity_);
}

void StepSequencerView::mouseDrag(const juce::MouseEvent& e) {
    if (paintVelocity_ < 0) return;
    const auto cell = cellAt(e.getPosition());
    if (cell.row < 0 || (cell.row == lastPaintedRow_ && cell.step == lastPaintedStep_)) return;
    lastPaintedRow_ = cell.row;
    lastPaintedStep_ = cell.step;
    const auto r = rows();
    setStep(r[static_cast<size_t>(cell.row)], cell.step, paintVelocity_, false);
}

void StepSequencerView::mouseUp(const juce::MouseEvent&) { paintVelocity_ = -1; }

void StepSequencerView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    // Wheel over a lit step: velocity up/down.
    const auto cell = cellAt(e.getPosition());
    const auto* c = clip();
    if (cell.row < 0 || !c) return;
    const int note = rows()[static_cast<size_t>(cell.row)];
    const int v = ynot::stepVelocity(*c, note, cell.step);
    if (v <= 0) return;
    setStep(note, cell.step, std::clamp(v + (w.deltaY > 0 ? 8 : -8), 1, 127), true);
}

void StepSequencerView::visibilityChanged() {
    if (isVisible()) openOrCreate();
}

void StepSequencerView::timerCallback() {
    // Playhead: highlight the step that is sounding.
    int step = -1;
    if (const auto* c = clip(); c && session_.isPlaying()) {
        const int64_t t = session_.positionTick() - c->startTick;
        if (t >= 0 && t < c->lengthTick) {
            const auto pat = c->steps.value_or(ynot::StepPattern{});
            step = static_cast<int>(t / std::max<int64_t>(1, pat.stepTicks));
        }
    }
    play_.setButtonText(session_.isPlaying() ? "Stop" : "Play pattern");
    if (step != playStep_) {
        playStep_ = step;
        repaint(gridArea_);
    }
}

void StepSequencerView::paint(juce::Graphics& g) {
    g.setColour(theme_.background.brighter(0.04f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.f);
    const auto* c = clip();
    if (!c) return;
    const auto r = rows();
    const int n = steps();
    const auto pat = c->steps.value_or(ynot::StepPattern{});
    const int64_t beat = session_.project().beatTicks();
    const auto* t = session_.project().findTrack(trackId_);
    const auto color = t ? Theme::hex(t->color) : theme_.primary;
    g.setFont(juce::FontOptions(theme_.bodySize - 1.f));
    for (int row = 0; row < static_cast<int>(r.size()); ++row) {
        auto nb = nameBounds(row);
        if (nb.getBottom() > gridArea_.getBottom()) break;
        g.setColour(theme_.surfaceRaised.withAlpha(0.7f));
        g.fillRoundedRectangle(nb.toFloat(), 4.f);
        g.setColour(theme_.text);
        g.drawText(noteName(r[static_cast<size_t>(row)]), nb.reduced(8, 0), juce::Justification::centredLeft, true);
        for (int s = 0; s < n; ++s) {
            const auto cb = cellBounds(row, s).toFloat();
            // Alternate shading by beat, so the grid reads in beats whatever the step size.
            const bool oddBeat = (static_cast<int64_t>(s) * pat.stepTicks / std::max<int64_t>(1, beat)) % 2 == 1;
            g.setColour(oddBeat ? theme_.surfaceRaised.brighter(0.18f) : theme_.surfaceRaised);
            g.fillRoundedRectangle(cb, 3.f);
            const int v = ynot::stepVelocity(*c, r[static_cast<size_t>(row)], s);
            if (v > 0) {
                g.setColour(color.withAlpha(0.35f + 0.65f * static_cast<float>(v) / 127.f));
                g.fillRoundedRectangle(cb.reduced(1.5f), 3.f);
                if (v >= kAccent) {
                    g.setColour(theme_.text);
                    g.fillEllipse(cb.withSizeKeepingCentre(5.f, 5.f));
                }
            }
            if (s == playStep_) {
                g.setColour(theme_.accent.withAlpha(0.9f));
                g.drawRoundedRectangle(cb.reduced(0.5f), 3.f, 1.5f);
            }
        }
    }
}

void StepSequencerView::resized() {
    auto r = getLocalBounds().reduced(10);
    auto header = r.removeFromTop(kHeaderH - 8);
    newDrums_.setBounds(header.removeFromRight(120));
    header.removeFromRight(6);
    clear_.setBounds(header.removeFromRight(70));
    header.removeFromRight(6);
    play_.setBounds(header.removeFromRight(110));
    header.removeFromRight(10);
    auto swingArea = header.removeFromRight(190);
    swingLabel_.setBounds(swingArea.removeFromLeft(48));
    swing_.setBounds(swingArea);
    header.removeFromRight(8);
    grid_.setBounds(header.removeFromRight(120));
    header.removeFromRight(6);
    bars_.setBounds(header.removeFromRight(90));
    header.removeFromRight(8);
    title_.setBounds(header);
    r.removeFromTop(8);
    gridArea_ = r;
}

}  // namespace ynotapp
