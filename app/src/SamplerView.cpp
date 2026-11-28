#include "SamplerView.h"

#include "AudioImporter.h"

namespace ynotapp {

namespace {
juce::String noteName(int n) { return juce::MidiMessage::getMidiNoteName(n, true, true, 3); }
}  // namespace

SamplerView::SamplerView(Session& session, const Theme& theme) : session_(session), theme_(theme) {
    title_.setFont(juce::FontOptions(theme.titleSize, juce::Font::bold));
    addAndMakeVisible(title_);
    hint_.setColour(juce::Label::textColourId, theme.textMuted);
    hint_.setFont(juce::FontOptions(theme.bodySize - 1.f));
    addAndMakeVisible(hint_);
    for (auto* b : {&load_, &autoChop_, &newTrack_}) addAndMakeVisible(*b);
    load_.onClick = [this] { loadSample(); };
    newTrack_.onClick = [this] { if (onRequestNewSampler) onRequestNewSampler(); };
    autoChop_.setTooltip("Find each hit or note and put it on its own pad");
    autoChop_.onClick = [this] {
        const auto* a = sample();
        if (!a) return;
        const auto s = ynot::detectSlices(*a, kPads);
        session_.edit([&](ynot::Project& p) { ynot::setSlices(p, trackId_, s); });
    };
    mode_.addItem("Chop: slices on pads", 1);
    mode_.addItem("Keys: play it across the keyboard", 2);
    mode_.onChange = [this] {
        if (!updating_) setParam("mode", mode_.getSelectedId() == 2 ? "keys" : "chop");
    };
    equal_.setTextWhenNothingSelected("Equal slices");
    for (int n : {2, 4, 8, 16, 32}) equal_.addItem(juce::String(n) + " equal slices", n);
    equal_.onChange = [this] {
        if (updating_ || equal_.getSelectedId() == 0) return;
        const auto* a = sample();
        if (a) {
            const auto s = ynot::equalSlices(a->frames(), equal_.getSelectedId());
            session_.edit([&](ynot::Project& p) { ynot::setSlices(p, trackId_, s); });
        }
        equal_.setSelectedId(0, juce::dontSendNotification);
    };
    for (int n = 36; n <= 84; ++n) root_.addItem("Root " + noteName(n), n);
    root_.onChange = [this] {
        if (!updating_) setParam("rootNote", static_cast<int64_t>(root_.getSelectedId()));
    };
    for (auto* c : {&mode_, &equal_, &root_}) addAndMakeVisible(*c);
    auto knob = [this](juce::Slider& s, juce::Label& l, double lo, double hi, double step, const char* key,
                       const juce::String& suffix) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setRange(lo, hi, step);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 20);
        s.setTextValueSuffix(suffix);
        s.setColour(juce::Slider::trackColourId, theme_.primary);
        s.onDragStart = [this] { session_.recordUndoStep(); };
        s.onValueChange = [this, &s, key] {
            if (updating_) return;
            const double v = s.getValue();
            session_.edit([&](ynot::Project& p) {
                if (auto* pl = ynot::samplerOf(p, trackId_)) pl->params[key] = v;
            }, !s.isMouseButtonDown());
        };
        l.setFont(juce::FontOptions(theme_.bodySize - 1.f));
        l.setColour(juce::Label::textColourId, theme_.textMuted);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
    };
    knob(attack_, attackLabel_, 0, 500, 1, "attackMs", " ms");
    knob(release_, releaseLabel_, 1, 3000, 1, "releaseMs", " ms");
    knob(tune_, tuneLabel_, -24, 24, 0.1, "tune", " st");
    attack_.setSkewFactorFromMidPoint(40);
    release_.setSkewFactorFromMidPoint(300);
    setWantsKeyboardFocus(false);
}

SamplerView::~SamplerView() = default;

const ynot::PluginInstance* SamplerView::sampler() const { return ynot::samplerOf(session_.project(), trackId_); }

const ynot::AudioBuffer* SamplerView::sample() const {
    const auto* s = sampler();
    return s ? session_.mediaAudio(s->params.getString("mediaId", "")) : nullptr;
}

std::vector<int64_t> SamplerView::slices() const {
    const auto* s = sampler();
    return s ? ynot::samplerSlices(*s) : std::vector<int64_t>{};
}

bool SamplerView::keysMode() const {
    const auto* s = sampler();
    return s && s->params.getString("mode", "chop") == "keys";
}

int SamplerView::baseNote() const { return static_cast<int>(param("baseNote", 36)); }

double SamplerView::param(const char* key, double def) const {
    const auto* s = sampler();
    return s ? s->params.getNumber(key, def) : def;
}

void SamplerView::setParam(const char* key, ynot::Json value) {
    session_.edit([&](ynot::Project& p) {
        if (auto* pl = ynot::samplerOf(p, trackId_)) pl->params[key] = value;
    });
}

void SamplerView::open(const std::string& trackId) {
    trackId_ = trackId;
    session_.setAuditionTrack(trackId_);
    refresh();
}

void SamplerView::openOrCreate() {
    if (sampler()) {
        session_.setAuditionTrack(trackId_);
        refresh();
        return;
    }
    for (const auto* t : session_.project().orderedTracks())
        if (ynot::samplerOf(session_.project(), t->id)) {
            open(t->id);
            return;
        }
    trackId_.clear();
    refresh();
}

void SamplerView::visibilityChanged() {
    if (isVisible()) openOrCreate();
    else session_.setAuditionTrack({});
}

void SamplerView::refresh() {
    updating_ = true;
    const auto* s = sampler();
    const auto* t = session_.project().findTrack(trackId_);
    title_.setText(s && t ? "Ynot Sampler - " + juce::String(t->name) : juce::String("Ynot Sampler"),
                   juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*>{&load_, &autoChop_, &mode_, &equal_, &root_, &attack_,
                                                           &release_, &tune_})
        c->setEnabled(s != nullptr);
    if (s) {
        const bool keys = keysMode();
        mode_.setSelectedId(keys ? 2 : 1, juce::dontSendNotification);
        root_.setSelectedId(static_cast<int>(param("rootNote", 60)), juce::dontSendNotification);
        root_.setVisible(keys);
        autoChop_.setVisible(!keys);
        equal_.setVisible(!keys);
        attack_.setValue(param("attackMs", 1), juce::dontSendNotification);
        release_.setValue(param("releaseMs", keys ? 120 : 15), juce::dontSendNotification);
        tune_.setValue(param("tune", 0), juce::dontSendNotification);
        const auto n = static_cast<int>(slices().size());
        hint_.setText(keys ? "Play it on your keyboard. Root " + noteName(static_cast<int>(param("rootNote", 60))) +
                                 " plays the sample as recorded."
                           : juce::String(n) + " slice" + (n == 1 ? "" : "s") + " on pads " + noteName(baseNote()) +
                                 " and up. Drag a line to move it, double-click to add, right-click to remove.",
                      juce::dontSendNotification);
    } else {
        hint_.setText(trackId_.empty() ? "No sampler track yet: press \"+ Sampler track\" and pick a sound."
                                       : "This track has no sampler.",
                      juce::dontSendNotification);
    }
    updating_ = false;
    resized();
    repaint();
}

void SamplerView::resized() {
    auto r = getLocalBounds().reduced(10, 8);
    auto top = r.removeFromTop(28);
    newTrack_.setBounds(top.removeFromRight(130));
    top.removeFromRight(6);
    load_.setBounds(top.removeFromRight(120));
    top.removeFromRight(6);
    title_.setBounds(top);
    r.removeFromTop(6);
    auto row = r.removeFromTop(26);
    mode_.setBounds(row.removeFromLeft(240));
    row.removeFromLeft(8);
    if (keysMode()) {
        root_.setBounds(row.removeFromLeft(110));
    } else {
        autoChop_.setBounds(row.removeFromLeft(90));
        row.removeFromLeft(6);
        equal_.setBounds(row.removeFromLeft(140));
    }
    row.removeFromLeft(12);
    for (auto [s, l] : {std::pair{&attack_, &attackLabel_}, {&release_, &releaseLabel_}, {&tune_, &tuneLabel_}}) {
        if (row.getWidth() < 120) {
            s->setBounds({});
            l->setBounds({});
            continue;
        }
        l->setBounds(row.removeFromLeft(50));
        s->setBounds(row.removeFromLeft(std::min(170, row.getWidth())));
        row.removeFromLeft(6);
    }
    r.removeFromTop(4);
    hint_.setBounds(r.removeFromBottom(18));
    r.removeFromBottom(4);
    const int padSize = std::clamp((r.getHeight() - 3 * 4) / 4, 24, 60);
    padArea_ = r.removeFromRight(padSize * 4 + 3 * 4);
    r.removeFromRight(10);
    waveArea_ = r;
}

float SamplerView::frameToX(int64_t frame) const {
    const auto* a = sample();
    const double n = a ? static_cast<double>(std::max<int64_t>(1, a->frames())) : 1.0;
    return static_cast<float>(waveArea_.getX() + waveArea_.getWidth() * static_cast<double>(frame) / n);
}

int64_t SamplerView::xToFrame(float x) const {
    const auto* a = sample();
    if (!a || waveArea_.getWidth() <= 0) return 0;
    const double t = std::clamp((x - static_cast<float>(waveArea_.getX())) / static_cast<float>(waveArea_.getWidth()), 0.f, 1.f);
    return static_cast<int64_t>(t * static_cast<double>(a->frames()));
}

int SamplerView::markerAt(float x) const {
    if (keysMode()) return -1;
    const auto s = slices();
    for (int i = 1; i < static_cast<int>(s.size()); ++i)
        if (std::abs(frameToX(s[static_cast<size_t>(i)]) - x) <= 5.f) return i;
    return -1;
}

int SamplerView::sliceAt(float x) const {
    const auto s = slices();
    const int64_t f = xToFrame(x);
    int idx = 0;
    for (int i = 0; i < static_cast<int>(s.size()); ++i)
        if (s[static_cast<size_t>(i)] <= f) idx = i;
    return idx;
}

juce::Rectangle<int> SamplerView::padBounds(int pad) const {
    const int size = (padArea_.getWidth() - 3 * 4) / 4;
    const int col = pad % 4, rowFromBottom = pad / 4;  // pad 1 bottom-left, like the hardware
    return {padArea_.getX() + col * (size + 4), padArea_.getBottom() - (rowFromBottom + 1) * (size + 4) + 4, size, size};
}

void SamplerView::paint(juce::Graphics& g) {
    g.fillAll(theme_.surface);
    g.setColour(theme_.background);
    g.fillRoundedRectangle(waveArea_.toFloat(), 6.f);
    const auto* a = sample();
    const auto sl = slices();
    const bool keys = keysMode();
    if (a && a->frames() > 0 && waveArea_.getWidth() > 0) {
        const auto* s = sampler();
        const std::string id = s ? s->params.getString("mediaId", "") : "";
        if (id != peaksFor_ || peaksWidth_ != waveArea_.getWidth()) {
            peaksFor_ = id;
            peaksWidth_ = waveArea_.getWidth();
            peaks_.assign(static_cast<size_t>(peaksWidth_), {0.f, 0.f});
            const int64_t n = a->frames();
            for (int x = 0; x < peaksWidth_; ++x) {
                const int64_t from = n * x / peaksWidth_, to = std::max(from + 1, n * (x + 1) / peaksWidth_);
                float lo = 0, hi = 0;
                for (int64_t f = from; f < to && f < n; ++f) {
                    const float v = a->samples[static_cast<size_t>(f * a->channels)];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                peaks_[static_cast<size_t>(x)] = {lo, hi};
            }
        }
        // Alternate shading per slice so the chops are easy to see.
        if (!keys)
            for (size_t i = 0; i < sl.size(); ++i) {
                const float x0 = frameToX(sl[i]), x1 = i + 1 < sl.size() ? frameToX(sl[i + 1]) : static_cast<float>(waveArea_.getRight());
                g.setColour((i % 2 ? theme_.primary : theme_.secondary).withAlpha(0.08f));
                g.fillRect(juce::Rectangle<float>(x0, static_cast<float>(waveArea_.getY()), x1 - x0, static_cast<float>(waveArea_.getHeight())));
            }
        const float mid = static_cast<float>(waveArea_.getCentreY()), half = waveArea_.getHeight() * 0.45f;
        g.setColour(theme_.secondary);
        for (int x = 0; x < peaksWidth_; ++x) {
            const auto [lo, hi] = peaks_[static_cast<size_t>(x)];
            g.drawVerticalLine(waveArea_.getX() + x, mid - hi * half, mid - lo * half + 1.f);
        }
        if (!keys) {
            g.setFont(juce::FontOptions(theme_.bodySize - 2.f, juce::Font::bold));
            for (size_t i = 0; i < sl.size(); ++i) {
                const float x = frameToX(sl[i]);
                g.setColour(static_cast<int>(i) == dragMarker_ ? theme_.accent : theme_.text.withAlpha(0.8f));
                g.drawVerticalLine(static_cast<int>(x), static_cast<float>(waveArea_.getY()), static_cast<float>(waveArea_.getBottom()));
                g.drawText(juce::String(static_cast<int>(i) + 1), juce::Rectangle<float>(x + 3, static_cast<float>(waveArea_.getY() + 2), 24, 14),
                           juce::Justification::topLeft);
            }
        }
    } else {
        g.setColour(theme_.textMuted);
        g.drawText(sampler() ? "Load a sample (any audio file), or right-click an audio clip > Send to Sampler"
                             : "Ynot Sampler: chop a sample onto pads, or play it across the keyboard",
                   waveArea_, juce::Justification::centred);
    }
    // Pads.
    for (int p = 0; p < kPads; ++p) {
        const auto b = padBounds(p).toFloat();
        const bool used = keys || p < static_cast<int>(sl.size());
        g.setColour(p == flashPad_ ? theme_.accent : used ? theme_.surfaceRaised : theme_.background);
        g.fillRoundedRectangle(b, 5.f);
        g.setColour(used ? theme_.text : theme_.textMuted.withAlpha(0.5f));
        g.setFont(juce::FontOptions(theme_.bodySize - 1.f, juce::Font::bold));
        g.drawText(keys ? noteName(baseNote() + p) : juce::String(p + 1), b.reduced(4), juce::Justification::topLeft);
        g.setFont(juce::FontOptions(theme_.bodySize - 3.f));
        if (!keys) g.drawText(noteName(baseNote() + p), b.reduced(4), juce::Justification::bottomRight);
    }
}

void SamplerView::play(int note) {
    session_.auditionNote(note, 110);
    flashPad_ = note - baseNote();
    repaint();
    juce::Component::SafePointer<SamplerView> safe(this);
    juce::Timer::callAfterDelay(150, [safe] {
        if (safe) {
            safe->flashPad_ = -1;
            safe->repaint();
        }
    });
}

void SamplerView::loadSample() {
    if (!sampler()) return;
    chooser_ = std::make_unique<juce::FileChooser>("Load a sample", juce::File(), importFilePatterns());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f == juce::File()) return;
                              session_.loadSampleFile(trackId_, f, [](const juce::String& err) {
                                  if (err.isNotEmpty())
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                             "Can't load the sample", err);
                              });
                          });
}

void SamplerView::mouseMove(const juce::MouseEvent& e) {
    setMouseCursor(waveArea_.contains(e.getPosition()) && markerAt(e.position.x) > 0
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::NormalCursor);
}

void SamplerView::mouseDown(const juce::MouseEvent& e) {
    if (!sampler()) return;
    for (int p = 0; p < kPads; ++p)
        if (padBounds(p).contains(e.getPosition())) {
            play(baseNote() + p);
            return;
        }
    if (!waveArea_.contains(e.getPosition()) || !sample()) return;
    const int marker = markerAt(e.position.x);
    if (e.mods.isPopupMenu()) {
        if (marker > 0) {
            juce::PopupMenu m;
            m.addItem("Remove slice " + juce::String(marker + 1), [this, marker] {
                session_.edit([&](ynot::Project& p) { ynot::removeSlice(p, trackId_, marker); });
            });
            m.showMenuAsync({});
        }
        return;
    }
    if (marker > 0) {
        dragMarker_ = marker;
        session_.recordUndoStep();
        return;
    }
    play(keysMode() ? static_cast<int>(param("rootNote", 60)) : baseNote() + sliceAt(e.position.x));
}

void SamplerView::mouseDrag(const juce::MouseEvent& e) {
    if (dragMarker_ <= 0) return;
    const int64_t f = xToFrame(e.position.x);
    session_.edit([&](ynot::Project& p) { ynot::moveSlice(p, trackId_, dragMarker_, f); }, false);
}

void SamplerView::mouseUp(const juce::MouseEvent&) {
    if (dragMarker_ > 0) {
        dragMarker_ = -1;
        repaint();
    }
}

void SamplerView::mouseDoubleClick(const juce::MouseEvent& e) {
    if (!sampler() || keysMode() || !waveArea_.contains(e.getPosition()) || markerAt(e.position.x) > 0) return;
    const int64_t f = xToFrame(e.position.x);
    session_.edit([&](ynot::Project& p) { ynot::addSlice(p, trackId_, f); });
}

}  // namespace ynotapp
