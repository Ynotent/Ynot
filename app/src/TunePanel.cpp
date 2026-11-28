#include "TunePanel.h"

#include "ynot/pitch_correct.hpp"

namespace ynotapp {

TunePanel::TunePanel(Session& session, const Theme& theme, std::string trackId, std::string pluginId)
    : session_(session), theme_(theme), trackId_(std::move(trackId)), pluginId_(std::move(pluginId)) {
    for (int k = 0; k < 12; ++k) key_.addItem(ynot::keyName(k), k + 1);
    key_.setSelectedId(static_cast<int>(param("key", 0)) + 1, juce::dontSendNotification);
    key_.onChange = [this] { set("key", key_.getSelectedId() - 1); };
    const auto& scales = ynot::tuneScales();
    for (size_t i = 0; i < scales.size(); ++i) scale_.addItem(scales[i].name, static_cast<int>(i) + 1);
    scale_.setSelectedId(static_cast<int>(param("scale", 1)) + 1, juce::dontSendNotification);
    scale_.onChange = [this] { set("scale", scale_.getSelectedId() - 1); };

    auto setup = [this](juce::Slider& s, double lo, double hi, double step, double value, const char* key,
                        const juce::String& suffix) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setRange(lo, hi, step);
        s.setValue(value, juce::dontSendNotification);
        s.setTextValueSuffix(suffix);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
        s.setColour(juce::Slider::trackColourId, theme_.primary);
        // Apply on release: each change rebuilds the track's effect chain.
        s.onDragEnd = [this, &s, key] { set(key, s.getValue()); };
        s.onValueChange = [this, &s, key] {
            if (!s.isMouseButtonDown()) set(key, s.getValue());
        };
        addAndMakeVisible(s);
    };
    setup(speed_, 0, 400, 1, param("speed", 20), "speed", " ms");
    speed_.setSkewFactorFromMidPoint(60);
    speed_.setTooltip("0 ms = hard, stepped 'robot' tuning. 20-60 ms = natural. 100+ ms = gentle.");
    setup(humanize_, 0, 100, 1, param("humanize", 0) * 100, "humanize", " %");
    humanize_.setTooltip("Held notes are corrected more gently, keeping natural vibrato.");
    setup(flex_, 0, 100, 1, param("flex", 0), "flex", " cents");
    flex_.setTooltip("Notes further than this from the scale are left alone (slides, bends). 0 = always correct.");
    setup(mix_, 0, 100, 1, param("mix", 1) * 100, "mix", " %");
    humanize_.onDragEnd = [this] { set("humanize", humanize_.getValue() / 100.0); };
    humanize_.onValueChange = [this] {
        if (!humanize_.isMouseButtonDown()) set("humanize", humanize_.getValue() / 100.0);
    };
    mix_.onDragEnd = [this] { set("mix", mix_.getValue() / 100.0); };
    mix_.onValueChange = [this] {
        if (!mix_.isMouseButtonDown()) set("mix", mix_.getValue() / 100.0);
    };
    for (auto* l : {&keyLabel_, &scaleLabel_, &speedLabel_, &humanizeLabel_, &flexLabel_, &mixLabel_}) {
        l->setColour(juce::Label::textColourId, theme.textMuted);
        addAndMakeVisible(*l);
    }
    hint_.setText("Set the song's key and scale, then lower the retune speed for a harder effect. "
                  "Ynot Tune adds about 16 ms of look-ahead; playback and export stay in time.",
                  juce::dontSendNotification);
    hint_.setFont(juce::FontOptions(theme.bodySize - 1.f));
    hint_.setColour(juce::Label::textColourId, theme.textMuted);
    hint_.setJustificationType(juce::Justification::topLeft);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&key_, &scale_, &hint_}) addAndMakeVisible(*c);
    setSize(460, 330);
}

double TunePanel::param(const char* key, double def) const {
    if (const auto* t = session_.project().findTrack(trackId_))
        for (const auto& p : t->inserts)
            if (p.id == pluginId_) return p.params.getNumber(key, def);
    return def;
}

void TunePanel::set(const char* key, double value) {
    session_.edit([&](ynot::Project& p) {
        if (auto* t = p.findTrack(trackId_))
            for (auto& pl : t->inserts)
                if (pl.id == pluginId_) pl.params[key] = value;
    });
}

void TunePanel::paint(juce::Graphics& g) { g.fillAll(theme_.surface); }

void TunePanel::resized() {
    auto r = getLocalBounds().reduced(16);
    auto row = [&](juce::Label& l, juce::Component& c) {
        auto line = r.removeFromTop(30);
        l.setBounds(line.removeFromLeft(110));
        c.setBounds(line);
        r.removeFromTop(8);
    };
    row(keyLabel_, key_);
    row(scaleLabel_, scale_);
    row(speedLabel_, speed_);
    row(humanizeLabel_, humanize_);
    row(flexLabel_, flex_);
    row(mixLabel_, mix_);
    hint_.setBounds(r);
}

void TunePanel::show(Session& session, const Theme& theme, const std::string& trackId, const std::string& pluginId) {
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new TunePanel(session, theme, trackId, pluginId));
    const auto* t = session.project().findTrack(trackId);
    o.dialogTitle = "Ynot Tune" + (t ? juce::String(" - ") + t->name : juce::String());
    o.dialogBackgroundColour = theme.surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.launchAsync();
}

}  // namespace ynotapp
