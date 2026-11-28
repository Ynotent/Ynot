#include "MixerView.h"

#include <cmath>

namespace ynotapp {

namespace {
constexpr int kStripWidth = 92;
constexpr double kMinDb = -60.0, kMaxDb = 12.0;

juce::String dbText(double db) {
    if (db <= kMinDb + 0.05) return "-inf";
    return (db > 0.05 ? "+" : "") + juce::String(db, 1);
}
}  // namespace

float MixerView::meterPosition(float linearPeak) {
    if (linearPeak <= 0.f) return 0.f;
    const float db = 20.f * std::log10(linearPeak);
    return juce::jlimit(0.f, 1.f, (db + 60.f) / 66.f);
}

// ------------------------------------------------------------------ meter

class MixerView::Meter : public juce::Component {
public:
    explicit Meter(const Theme& theme) : theme_(theme) {}

    // Called ~30 times a second with the peaks since the previous call.
    void push(float left, float right) {
        const float in[2] = {left, right};
        for (int c = 0; c < 2; ++c) {
            // Instant attack, ~20 dB/s fall: readable, and short peaks still register.
            level_[c] = std::max(in[c], level_[c] * 0.86f);
            if (in[c] >= hold_[c]) {
                hold_[c] = in[c];
                holdFrames_[c] = 45;  // ~1.5 s
            } else if (--holdFrames_[c] <= 0) {
                hold_[c] = std::max(level_[c], hold_[c] * 0.9f);
            }
            clipped_ = clipped_ || in[c] >= 1.f;
        }
        repaint();
    }
    void mouseDown(const juce::MouseEvent&) override {  // click to reset the clip light
        clipped_ = false;
        repaint();
    }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        auto clip = r.removeFromTop(6.f);
        g.setColour(clipped_ ? theme_.record : theme_.border);
        g.fillRoundedRectangle(clip, 2.f);
        r.removeFromTop(3.f);
        const float w = (r.getWidth() - 2.f) / 2.f;
        for (int c = 0; c < 2; ++c) {
            auto bar = juce::Rectangle<float>(r.getX() + static_cast<float>(c) * (w + 2.f), r.getY(), w, r.getHeight());
            g.setColour(theme_.background);
            g.fillRect(bar);
            const float pos = meterPosition(level_[c]);
            auto lit = bar.withTop(bar.getBottom() - bar.getHeight() * pos);
            juce::ColourGradient grad(theme_.secondary, bar.getBottomLeft(), theme_.record, bar.getTopLeft(), false);
            grad.addColour(48.0 / 66.0, theme_.accent);  // -12 dB
            g.setGradientFill(grad);
            g.fillRect(lit);
            const float holdY = bar.getBottom() - bar.getHeight() * meterPosition(hold_[c]);
            if (hold_[c] > 0.f) {
                g.setColour(theme_.text);
                g.fillRect(juce::Rectangle<float>(bar.getX(), holdY - 1.f, w, 2.f));
            }
        }
        // 0 dB line
        g.setColour(theme_.textMuted.withAlpha(0.6f));
        const float zero = r.getBottom() - r.getHeight() * (60.f / 66.f);
        g.drawHorizontalLine(static_cast<int>(zero), r.getX(), r.getRight());
    }

private:
    const Theme& theme_;
    float level_[2] = {0, 0}, hold_[2] = {0, 0};
    int holdFrames_[2] = {0, 0};
    bool clipped_ = false;
};

// ------------------------------------------------------------------ strip

class MixerView::Strip : public juce::Component {
public:
    // trackId empty = master.
    Strip(Session& session, const Theme& theme, std::string trackId)
        : session_(session), theme_(theme), id_(std::move(trackId)), meter_(theme) {
        name_.setJustificationType(juce::Justification::centred);
        name_.setFont(juce::FontOptions(theme.bodySize, juce::Font::bold));
        addAndMakeVisible(name_);
        fader_.setSliderStyle(juce::Slider::LinearVertical);
        fader_.setRange(kMinDb, kMaxDb, 0.1);
        fader_.setSkewFactorFromMidPoint(-12.0);  // more travel around 0 dB, like a console
        fader_.setDoubleClickReturnValue(true, 0.0);
        fader_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 20);
        fader_.textFromValueFunction = [](double v) { return dbText(v) + " dB"; };
        fader_.updateText();
        fader_.valueFromTextFunction = [](const juce::String& t) { return t.retainCharacters("-+0123456789.").getDoubleValue(); };
        fader_.setColour(juce::Slider::thumbColourId, theme.text);
        fader_.setColour(juce::Slider::trackColourId, theme.primary);
        fader_.setColour(juce::Slider::backgroundColourId, theme.background);
        fader_.setTooltip("Volume. Double-click for 0 dB.");
        fader_.onDragStart = [this] { gesture_ = true; session_.beginMixChange(); };
        fader_.onDragEnd = [this] { gesture_ = false; };
        fader_.onValueChange = [this] {
            if (updating_) return;
            if (!gesture_) session_.beginMixChange();  // typed value or double-click: one undo step
            if (id_.empty()) session_.setMasterGain(fader_.getValue());
            else session_.setTrackGain(id_, fader_.getValue() <= kMinDb + 0.05 ? -96.0 : fader_.getValue());
        };
        addAndMakeVisible(fader_);
        addAndMakeVisible(meter_);
        if (!id_.empty()) {
            pan_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            pan_.setRange(-1.0, 1.0, 0.01);
            pan_.setDoubleClickReturnValue(true, 0.0);
            pan_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            pan_.setColour(juce::Slider::rotarySliderFillColourId, theme.primary);
            pan_.setColour(juce::Slider::rotarySliderOutlineColourId, theme.background);
            pan_.setTooltip("Pan. Double-click for centre.");
            pan_.onDragStart = [this] { panGesture_ = true; session_.beginMixChange(); };
            pan_.onDragEnd = [this] { panGesture_ = false; };
            pan_.onValueChange = [this] {
                if (updating_) return;
                if (!panGesture_) session_.beginMixChange();
                session_.setTrackPan(id_, pan_.getValue());
                panText_.setText(panLabel(pan_.getValue()), juce::dontSendNotification);
            };
            addAndMakeVisible(pan_);
            panText_.setJustificationType(juce::Justification::centred);
            panText_.setFont(juce::FontOptions(theme.bodySize - 2.f));
            panText_.setColour(juce::Label::textColourId, theme.textMuted);
            addAndMakeVisible(panText_);
            for (auto* b : {&mute_, &solo_}) {
                b->setClickingTogglesState(false);
                b->setMouseClickGrabsKeyboardFocus(false);
                addAndMakeVisible(*b);
            }
            mute_.setColour(juce::TextButton::buttonOnColourId, theme.accent);
            solo_.setColour(juce::TextButton::buttonOnColourId, theme.secondary);
            mute_.onClick = [this] {
                if (auto* t = session_.project().findTrack(id_)) session_.setTrackMute(id_, !t->mute);
            };
            solo_.onClick = [this] {
                if (auto* t = session_.project().findTrack(id_)) session_.setTrackSolo(id_, !t->solo);
            };
        }
        fader_.setMouseClickGrabsKeyboardFocus(false);
        pan_.setMouseClickGrabsKeyboardFocus(false);
        update();
    }

    const std::string& trackId() const { return id_; }

    void update() {
        updating_ = true;
        if (id_.empty()) {
            name_.setText("Master", juce::dontSendNotification);
            color_ = theme_.text;
            fader_.setValue(std::max(kMinDb, session_.project().masterGainDb), juce::dontSendNotification);
        } else if (const auto* t = session_.project().findTrack(id_)) {
            name_.setText(t->name, juce::dontSendNotification);
            color_ = Theme::hex(t->color);
            if (!fader_.isMouseButtonDown()) fader_.setValue(std::max(kMinDb, t->gainDb), juce::dontSendNotification);
            if (!pan_.isMouseButtonDown()) pan_.setValue(t->pan, juce::dontSendNotification);
            panText_.setText(panLabel(t->pan), juce::dontSendNotification);
            // Automation, when present, drives the fader (it reads the automation, like a console).
            const bool gainAuto = t->lane("track.gainDb") != nullptr, panAuto = t->lane("track.pan") != nullptr;
            fader_.setEnabled(!gainAuto);
            fader_.setTooltip(gainAuto ? "Volume follows this track's automation." : "Volume. Double-click for 0 dB.");
            pan_.setEnabled(!panAuto);
            if (panAuto) panText_.setText("auto", juce::dontSendNotification);
            autoLabel_ = gainAuto ? "AUTO" : "";
            mute_.setToggleState(t->mute, juce::dontSendNotification);
            solo_.setToggleState(t->solo, juce::dontSendNotification);
        }
        fader_.updateText();
        updating_ = false;
        repaint();
    }

    void pollMeter() {
        const auto m = session_.takeMeter(id_);
        meter_.push(m.left, m.right);
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat().reduced(2.f);
        g.setColour(theme_.surface);
        g.fillRoundedRectangle(r, 6.f);
        g.setColour(color_);
        g.fillRoundedRectangle(r.removeFromTop(4.f), 2.f);
        if (autoLabel_.isNotEmpty()) {
            g.setColour(theme_.accent);
            g.setFont(juce::FontOptions(theme_.bodySize - 3.f, juce::Font::bold));
            // Under the fader's value, clear of the pan readout.
            g.drawText(autoLabel_, fader_.getBounds().removeFromBottom(36).withHeight(12).translated(0, -2),
                       juce::Justification::centred);
        }
    }

    void resized() override {
        auto r = getLocalBounds().reduced(6);
        r.removeFromTop(4);
        name_.setBounds(r.removeFromTop(20));
        if (!id_.empty()) {
            auto panRow = r.removeFromTop(52);
            pan_.setBounds(panRow.withSizeKeepingCentre(52, 52));
            panText_.setBounds(r.removeFromTop(14));
            auto buttons = r.removeFromBottom(26);
            mute_.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2).reduced(1, 0));
            solo_.setBounds(buttons.reduced(1, 0));
            r.removeFromBottom(4);
        }
        r.removeFromTop(4);
        meter_.setBounds(r.removeFromRight(16).withTrimmedBottom(24));
        r.removeFromRight(2);
        fader_.setBounds(r);
    }

private:
    static juce::String panLabel(double pan) {
        if (std::abs(pan) < 0.005) return "C";
        return juce::String(static_cast<int>(std::lround(std::abs(pan) * 100))) + (pan < 0 ? " L" : " R");
    }

    Session& session_;
    const Theme& theme_;
    std::string id_;
    juce::Label name_, panText_;
    juce::Slider fader_, pan_;
    juce::TextButton mute_{"M"}, solo_{"S"};
    Meter meter_;
    juce::Colour color_;
    juce::String autoLabel_;
    bool updating_ = false, gesture_ = false, panGesture_ = false;
};

// ------------------------------------------------------------------ view

MixerView::MixerView(Session& session, const Theme& theme) : session_(session), theme_(theme) {
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(false, true);
    addAndMakeVisible(viewport_);
    master_ = std::make_unique<Strip>(session_, theme_, std::string());
    addAndMakeVisible(*master_);
    refresh();
    startTimerHz(30);
}

MixerView::~MixerView() { stopTimer(); }

void MixerView::refresh() {
    std::vector<std::string> ids;
    if (session_.hasProject())
        for (const auto* t : session_.project().orderedTracks()) ids.push_back(t->id);
    bool same = ids.size() == strips_.size();
    for (size_t i = 0; same && i < ids.size(); ++i) same = strips_[i]->trackId() == ids[i];
    if (!same) {  // tracks added, removed or reordered: rebuild the strips
        strips_.clear();
        for (const auto& id : ids) {
            strips_.push_back(std::make_unique<Strip>(session_, theme_, id));
            content_.addAndMakeVisible(*strips_.back());
        }
        resized();
    }
    for (auto& s : strips_) s->update();
    master_->update();
}

void MixerView::timerCallback() {
    if (!isShowing()) return;
    for (auto& s : strips_) s->pollMeter();
    master_->pollMeter();
}

void MixerView::paint(juce::Graphics& g) {
    g.setColour(theme_.background.brighter(0.04f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.f);
}

void MixerView::resized() {
    auto r = getLocalBounds().reduced(6);
    master_->setBounds(r.removeFromRight(kStripWidth));
    r.removeFromRight(8);
    viewport_.setBounds(r);
    const int h = r.getHeight() - (static_cast<int>(strips_.size()) * kStripWidth > r.getWidth() ? 10 : 0);
    content_.setSize(static_cast<int>(strips_.size()) * kStripWidth, h);
    for (size_t i = 0; i < strips_.size(); ++i)
        strips_[i]->setBounds(static_cast<int>(i) * kStripWidth, 0, kStripWidth, h);
}

}  // namespace ynotapp
