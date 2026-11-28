#include "InspectorView.h"

namespace ynotapp {

InspectorView::InspectorView(Session& session, ArrangeView& arrange, const Theme& theme)
    : session_(session), arrange_(arrange), theme_(theme) {
    name_.setFont(juce::FontOptions(theme.titleSize, juce::Font::bold));
    name_.setEditable(false, true);  // double-click to rename
    name_.onTextChange = [this] {
        const auto text = name_.getText().trim().toStdString();
        if (!text.empty()) session_.edit([&](ynot::Project& p) { ynot::renameTrack(p, trackId_, text); });
    };
    kind_.setColour(juce::Label::textColourId, theme.textMuted);
    for (auto* l : {&chainTitle_, &volLabel_, &panLabel_, &inputLabel_}) {
        l->setColour(juce::Label::textColourId, theme.textMuted);
        l->setFont(juce::FontOptions(theme.bodySize - 1.f));
    }
    chain_.setJustificationType(juce::Justification::topLeft);
    chain_.setFont(juce::FontOptions(theme.bodySize));
    input_.onClick = [this] {
        juce::PopupMenu m;
        const auto current = session_.inputFor(trackId_);
        for (const auto& c : session_.inputChoices())
            m.addItem(c.label, !session_.isRecording(), c.channels == current,
                      [this, ch = c.channels] { session_.setInput(trackId_, ch); });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(input_));
    };
    editChain_.onClick = [this] { arrange_.showTrackMenu(trackId_, &editChain_); };
    volume_.setSliderStyle(juce::Slider::LinearHorizontal);
    volume_.setRange(-60.0, 12.0, 0.1);
    volume_.setSkewFactorFromMidPoint(-12.0);
    volume_.setDoubleClickReturnValue(true, 0.0);
    volume_.setTextValueSuffix(" dB");
    volume_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    volume_.onDragStart = [this] { session_.beginMixChange(); };
    volume_.onValueChange = [this] {
        if (updating_) return;
        if (!volume_.isMouseButtonDown()) session_.beginMixChange();
        session_.setTrackGain(trackId_, volume_.getValue() <= -59.95 ? -96.0 : volume_.getValue());
    };
    pan_.setSliderStyle(juce::Slider::LinearHorizontal);
    pan_.setRange(-1.0, 1.0, 0.01);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    pan_.textFromValueFunction = [](double v) {
        if (std::abs(v) < 0.005) return juce::String("C");
        return juce::String(juce::roundToInt(std::abs(v) * 100)) + (v < 0 ? " L" : " R");
    };
    pan_.onDragStart = [this] { session_.beginMixChange(); };
    pan_.onValueChange = [this] {
        if (updating_) return;
        if (!pan_.isMouseButtonDown()) session_.beginMixChange();
        session_.setTrackPan(trackId_, pan_.getValue());
    };
    for (auto* s : {&volume_, &pan_}) {
        s->setColour(juce::Slider::trackColourId, theme.primary);
        s->setMouseClickGrabsKeyboardFocus(false);
    }
    mute_.setColour(juce::TextButton::buttonOnColourId, theme.accent);
    solo_.setColour(juce::TextButton::buttonOnColourId, theme.secondary);
    arm_.setColour(juce::TextButton::buttonOnColourId, theme.record);
    mute_.onClick = [this] {
        if (const auto* t = track()) session_.setTrackMute(trackId_, !t->mute);
    };
    solo_.onClick = [this] {
        if (const auto* t = track()) session_.setTrackSolo(trackId_, !t->solo);
    };
    arm_.onClick = [this] {
        if (session_.isRecording()) return;
        session_.edit([this](ynot::Project& p) {
            if (auto* t = p.findTrack(trackId_)) t->armed = !t->armed;
        });
    };
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &name_, &kind_, &chainTitle_, &chain_, &volLabel_, &panLabel_, &inputLabel_, &input_, &editChain_,
             &mute_, &solo_, &arm_, &volume_, &pan_}) {
        c->setMouseClickGrabsKeyboardFocus(false);
        addAndMakeVisible(*c);
    }
}

const ynot::Track* InspectorView::track() const {
    return session_.hasProject() ? session_.project().findTrack(trackId_) : nullptr;
}

void InspectorView::refresh() {
    trackId_ = arrange_.focusedTrackId();
    const auto* t = track();
    updating_ = true;
    for (int i = 0; i < getNumChildComponents(); ++i) getChildComponent(i)->setVisible(t != nullptr);
    if (t) {
        name_.setText(t->name, juce::dontSendNotification);
        const bool audio = t->kind == ynot::TrackKind::Audio;
        kind_.setText(audio ? "Audio track" : "Instrument track", juce::dontSendNotification);
        inputLabel_.setVisible(audio);
        input_.setVisible(audio);
        input_.setButtonText(juce::String(session_.inputLabel(t->id)) + "  \xe2\x96\xbe");
        juce::String chain;
        if (t->instrument) chain << juce::String(t->instrument->name) << (t->instrument->bypassed ? " (off)" : "") << "\n";
        for (const auto& p : t->inserts) chain << juce::String(p.name) << (p.bypassed ? " (off)" : "") << "\n";
        chain_.setText(chain.isEmpty() ? juce::String("No effects yet") : chain.trimEnd(), juce::dontSendNotification);
        if (!volume_.isMouseButtonDown()) volume_.setValue(std::max(-60.0, t->gainDb), juce::dontSendNotification);
        if (!pan_.isMouseButtonDown()) pan_.setValue(t->pan, juce::dontSendNotification);
        pan_.updateText();
        volume_.updateText();
        volume_.setEnabled(t->lane("track.gainDb") == nullptr);  // automation drives it
        mute_.setToggleState(t->mute, juce::dontSendNotification);
        solo_.setToggleState(t->solo, juce::dontSendNotification);
        arm_.setToggleState(t->armed, juce::dontSendNotification);
    }
    updating_ = false;
    resized();
    repaint();
}

void InspectorView::paint(juce::Graphics& g) {
    g.setColour(theme_.surface);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.f);
    if (const auto* t = track()) {
        g.setColour(Theme::hex(t->color));
        g.fillRoundedRectangle(getLocalBounds().toFloat().removeFromTop(5.f), 2.f);
    } else {
        g.setColour(theme_.textMuted);
        g.drawText("Add a track to see it here.", getLocalBounds().reduced(12), juce::Justification::centredTop);
    }
}

void InspectorView::resized() {
    auto r = getLocalBounds().reduced(12);
    r.removeFromTop(6);
    name_.setBounds(r.removeFromTop(28));
    kind_.setBounds(r.removeFromTop(18));
    r.removeFromTop(10);
    if (input_.isVisible()) {
        inputLabel_.setBounds(r.removeFromTop(18));
        input_.setBounds(r.removeFromTop(28));
        r.removeFromTop(10);
    }
    auto buttons = r.removeFromTop(30);
    const int w = (buttons.getWidth() - 12) / 3;
    arm_.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(6);
    mute_.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(6);
    solo_.setBounds(buttons);
    r.removeFromTop(12);
    volLabel_.setBounds(r.removeFromTop(18));
    volume_.setBounds(r.removeFromTop(26));
    r.removeFromTop(6);
    panLabel_.setBounds(r.removeFromTop(18));
    pan_.setBounds(r.removeFromTop(26));
    r.removeFromTop(12);
    chainTitle_.setBounds(r.removeFromTop(18));
    editChain_.setBounds(r.removeFromBottom(30));
    r.removeFromBottom(6);
    chain_.setBounds(r);
}

}  // namespace ynotapp
