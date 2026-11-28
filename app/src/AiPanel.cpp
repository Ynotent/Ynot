#include "AiPanel.h"

namespace ynotapp {

AiResultsPanel::AiResultsPanel(Session& session, const Theme& theme, Session::SongAnalysis result)
    : session_(session), theme_(theme), result_(std::move(result)) {
    applyTune_.setButtonText("Set Ynot Tune to " + juce::String(result_.key.name()));
    applyTune_.onClick = [this] {
        const int n = session_.applyKeyToTune(result_.key);
        applyTune_.setButtonText(n == 0 ? juce::String("No Ynot Tune on this song yet")
                                        : "Ynot Tune set (" + juce::String(n) + " track" + (n == 1 ? ")" : "s)"));
        applyTune_.setEnabled(false);
    };
    showChords_.onClick = [this] {
        session_.setShownChords(result_.chords);
        showChords_.setButtonText("Chords shown above the timeline");
        showChords_.setEnabled(false);
    };
    close_.onClick = [this] {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    };
    for (auto* b : {&showChords_, &applyTune_, &close_}) addAndMakeVisible(*b);
    const bool any = std::any_of(result_.chords.begin(), result_.chords.end(), [](const ynot::ChordSpan& c) { return c.name != "N"; });
    showChords_.setEnabled(any);
    setSize(560, 400);
}

void AiResultsPanel::resized() {
    auto r = getLocalBounds().reduced(16);
    auto buttons = r.removeFromBottom(30);
    close_.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(8);
    applyTune_.setBounds(buttons.removeFromRight(230));
    buttons.removeFromRight(8);
    showChords_.setBounds(buttons);
    r.removeFromBottom(10);
    r.removeFromTop(86);
    chordArea_ = r;
}

void AiResultsPanel::paint(juce::Graphics& g) {
    g.fillAll(theme_.surface);
    auto r = getLocalBounds().reduced(16);
    g.setColour(theme_.textMuted);
    g.setFont(juce::FontOptions(theme_.bodySize));
    g.drawText("Key", r.removeFromTop(18), juce::Justification::left);
    g.setColour(theme_.text);
    g.setFont(juce::FontOptions(28.f, juce::Font::bold));
    const bool sure = result_.key.confidence >= 0.15;
    g.drawText(juce::String(result_.key.name()), r.removeFromTop(36), juce::Justification::left);
    g.setColour(sure ? theme_.secondary : theme_.accent);
    g.setFont(juce::FontOptions(theme_.bodySize));
    g.drawText(sure ? "Confident (" + juce::String(juce::roundToInt(result_.key.confidence * 100)) + "%)"
                    : juce::String("Not sure: the song may change key, or there isn't much to go on yet"),
               r.removeFromTop(18), juce::Justification::left);

    // Chords, in bars.
    g.setColour(theme_.textMuted);
    g.drawText("Chords (bar.beat)", chordArea_.withHeight(18), juce::Justification::left);
    auto area = chordArea_.withTrimmedTop(22);
    const auto& p = session_.project();
    const int64_t bar = std::max<int64_t>(1, p.barTicks()), beat = std::max<int64_t>(1, p.beatTicks());
    const int cellW = 84, cellH = 40;
    int x = area.getX(), y = area.getY();
    int shown = 0;
    for (const auto& c : result_.chords) {
        if (c.name == "N") continue;
        if (x + cellW > area.getRight()) {
            x = area.getX();
            y += cellH + 6;
        }
        if (y + cellH > area.getBottom()) {
            g.setColour(theme_.textMuted);
            g.drawText("...", juce::Rectangle<int>(x, y - cellH - 6, cellW, cellH), juce::Justification::centred);
            break;
        }
        const juce::Rectangle<float> cell(static_cast<float>(x), static_cast<float>(y), static_cast<float>(cellW), static_cast<float>(cellH));
        g.setColour(theme_.surfaceRaised);
        g.fillRoundedRectangle(cell, 5.f);
        g.setColour(theme_.text);
        g.setFont(juce::FontOptions(16.f, juce::Font::bold));
        g.drawText(juce::String(c.name), cell.withTrimmedBottom(14), juce::Justification::centred);
        g.setColour(theme_.textMuted);
        g.setFont(juce::FontOptions(theme_.bodySize - 2.f));
        g.drawText(juce::String(c.tick / bar + 1) + "." + juce::String((c.tick % bar) / beat + 1),
                   cell.withTrimmedTop(24), juce::Justification::centred);
        x += cellW + 6;
        ++shown;
    }
    if (shown == 0) {
        g.setColour(theme_.textMuted);
        g.drawText("No clear chords found (only drums, or not enough music yet).", area, juce::Justification::topLeft);
    }
}

void AiResultsPanel::detectKeyAndChords(Session& session, const Theme& theme) {
    if (!session.hasProject()) return;
    session.analyzeSong([&session, &theme](const Session::SongAnalysis& r) {
        if (r.error.isNotEmpty()) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Key & chords", r.error);
            return;
        }
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(new AiResultsPanel(session, theme, r));
        o.dialogTitle = "Ynot AI - Key & Chords";
        o.dialogBackgroundColour = theme.surface;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.launchAsync();
    });
}

void aiAudioToMidi(Session& session, const std::string& trackId, const std::string& clipId) {
    session.convertClipToMidi(trackId, clipId, [](const juce::String& err) {
        if (err.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Audio to MIDI", err);
    });
}

void aiDetectTempo(Session& session, const std::string& trackId, const std::string& clipId) {
    session.detectClipTempo(trackId, clipId, [&session](ynot::TempoEstimate e) {
        if (e.bpm <= 0) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Detect tempo",
                                                   "No steady beat found in this clip (it needs a few seconds of rhythm).");
            return;
        }
        const double bpm = e.bpm;
        const auto text = juce::String(bpm, 1) + " BPM";
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, "Detect tempo",
            "This clip is about " + text + ". Set the song's tempo to " + text +
                "?\n(Audio clips keep their real length; MIDI follows the new tempo.)",
            "Set tempo", "Not now", nullptr, juce::ModalCallbackFunction::create([&session, bpm](int ok) {
                if (ok) session.edit([&](ynot::Project& p) { ynot::setTempo(p, bpm); });
            }));
    });
}

}  // namespace ynotapp
