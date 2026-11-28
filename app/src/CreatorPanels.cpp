#include "CreatorPanels.h"

#include "AudioImporter.h"

namespace ynotapp {

namespace {

void launch(juce::Component* c, const juce::String& title, const Theme& theme) {
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(c);
    o.dialogTitle = title;
    o.dialogBackgroundColour = theme.surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void closeDialog(juce::Component& c) {
    if (auto* w = c.findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
}

juce::Colour statusColour(ynot::ClearanceStatus s, const Theme& t) {
    switch (s) {
        case ynot::ClearanceStatus::Original:
        case ynot::ClearanceStatus::RoyaltyFree:
        case ynot::ClearanceStatus::Cleared: return t.secondary;
        case ynot::ClearanceStatus::Requested: return t.accent;
        case ynot::ClearanceStatus::Unknown:
        case ynot::ClearanceStatus::Denied: return t.record;
    }
    return t.textMuted;
}

juce::String timeText(double seconds) {
    const int s = juce::roundToInt(seconds);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

}  // namespace

// ================================================================ clearance

ClearancePanel::ClearancePanel(Session& session, const Theme& theme) : session_(session), theme_(theme) {
    list_.setModel(this);
    list_.setRowHeight(46);
    list_.setColour(juce::ListBox::backgroundColourId, theme.background);
    addAndMakeVisible(list_);
    summary_.setFont(juce::FontOptions(theme.titleSize, juce::Font::bold));
    addAndMakeVisible(summary_);
    hint_.setColour(juce::Label::textColourId, theme.textMuted);
    hint_.setFont(juce::FontOptions(theme.bodySize - 1.f));
    hint_.setText("Every outside sample needs permission from the owner of the recording (master, usually the label) "
                  "AND the owner of the song (publisher) before release. Your own recordings, Ynot ideas and Ynot "
                  "pack sounds don't.",
                  juce::dontSendNotification);
    hint_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(hint_);
    for (auto st : {ynot::ClearanceStatus::Unknown, ynot::ClearanceStatus::Original, ynot::ClearanceStatus::RoyaltyFree,
                    ynot::ClearanceStatus::Requested, ynot::ClearanceStatus::Cleared, ynot::ClearanceStatus::Denied})
        status_.addItem(ynot::clearanceStatusName(st), static_cast<int>(st) + 1);
    status_.onChange = [this] { saveDetails(); };
    addAndMakeVisible(status_);
    const char* names[7] = {"Original title", "Original artist", "Master owner (label)", "Publisher",
                            "Taken from (time in original)", "Licence / agreement", "Notes"};
    for (size_t i = 0; i < fields_.size(); ++i) {
        fields_[i].label.setText(names[i], juce::dontSendNotification);
        fields_[i].label.setColour(juce::Label::textColourId, theme.textMuted);
        fields_[i].label.setFont(juce::FontOptions(theme.bodySize - 1.f));
        addAndMakeVisible(fields_[i].label);
        addAndMakeVisible(fields_[i].edit);
    }
    fields_[6].edit.setMultiLine(true);
    save_.onClick = [this] { saveDetails(); };
    export_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save the clearance sheet",
            session_.packageDir().getParentDirectory().getChildFile(juce::String(session_.project().name) + " - sample clearance.csv"),
            "*.csv");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  auto f = fc.getResult();
                                  if (f == juce::File()) return;
                                  if (f.getFileExtension() != ".csv") f = f.withFileExtension(".csv");
                                  const auto err = session_.exportClearanceSheet(f);
                                  juce::AlertWindow::showMessageBoxAsync(
                                      err.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                      "Clearance sheet", err.isEmpty() ? "Saved " + f.getFileName() +
                                                                             ". It opens in Excel, Numbers or Google Sheets."
                                                                       : err);
                              });
    };
    close_.onClick = [this] { closeDialog(*this); };
    for (auto* b : {&save_, &export_, &close_}) addAndMakeVisible(*b);
    reload();
    if (!uses_.empty()) list_.selectRow(0);
    else selectedRowsChanged(-1);
    setSize(860, 560);
}

void ClearancePanel::reload() {
    uses_ = ynot::clearanceReport(session_.project());
    int open = 0;
    for (const auto& u : uses_) open += ynot::needsClearance(u.status) ? 1 : 0;
    summary_.setText(uses_.empty() ? juce::String("No outside samples in this song")
                     : juce::String(static_cast<int>(uses_.size())) + " source" + (uses_.size() == 1 ? "" : "s") + ", " +
                           (open == 0 ? juce::String("all clear") : juce::String(open) + " not cleared yet"),
                     juce::dontSendNotification);
    summary_.setColour(juce::Label::textColourId, open == 0 ? theme_.secondary : theme_.record);
    list_.updateContent();
    list_.repaint();
}

int ClearancePanel::getNumRows() { return static_cast<int>(uses_.size()); }

void ClearancePanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row < 0 || row >= static_cast<int>(uses_.size())) return;
    const auto& u = uses_[static_cast<size_t>(row)];
    if (selected) g.fillAll(theme_.surfaceRaised);
    g.setColour(statusColour(u.status, theme_));
    g.fillRoundedRectangle(6.f, 8.f, 6.f, static_cast<float>(h - 16), 3.f);
    g.setColour(theme_.text);
    g.setFont(juce::FontOptions(theme_.bodySize, juce::Font::bold));
    g.drawText(juce::String(u.name), 20, 4, w - 24, 20, juce::Justification::left, true);
    juce::String tracks;
    for (const auto& t : u.trackNames) tracks << (tracks.isEmpty() ? "" : ", ") << t;
    g.setColour(theme_.textMuted);
    g.setFont(juce::FontOptions(theme_.bodySize - 2.f));
    g.drawText(juce::String(ynot::clearanceStatusName(u.status)) + "  |  " + tracks + "  |  " + timeText(u.secondsUsed) +
                   " used" + (u.firstTick >= 0 ? ", from " + timeText(session_.project().tickToSeconds(static_cast<double>(u.firstTick))) : juce::String()),
               20, 24, w - 24, 18, juce::Justification::left, true);
}

void ClearancePanel::selectedRowsChanged(int row) {
    selected_ = row;
    const bool on = row >= 0 && row < static_cast<int>(uses_.size());
    for (auto& f : fields_) f.edit.setEnabled(on);
    status_.setEnabled(on);
    save_.setEnabled(on);
    if (!on) return;
    const auto& u = uses_[static_cast<size_t>(row)];
    ynot::SampleCredit c;
    if (const auto* x = ynot::findCredit(session_.project(), u.mediaId)) c = *x;
    const std::string* vals[7] = {&c.title, &c.artist, &c.masterOwner, &c.publisher, &c.sourceTime, &c.license, &c.notes};
    for (size_t i = 0; i < fields_.size(); ++i) fields_[i].edit.setText(juce::String(*vals[i]), false);
    status_.setSelectedId(static_cast<int>(u.status) + 1, juce::dontSendNotification);
}

void ClearancePanel::saveDetails() {
    if (selected_ < 0 || selected_ >= static_cast<int>(uses_.size())) return;
    const auto& u = uses_[static_cast<size_t>(selected_)];
    ynot::SampleCredit c;
    if (const auto* x = ynot::findCredit(session_.project(), u.mediaId)) c = *x;
    c.mediaId = u.mediaId;
    std::string* vals[7] = {&c.title, &c.artist, &c.masterOwner, &c.publisher, &c.sourceTime, &c.license, &c.notes};
    for (size_t i = 0; i < fields_.size(); ++i) *vals[i] = fields_[i].edit.getText().toStdString();
    c.status = static_cast<ynot::ClearanceStatus>(std::max(0, status_.getSelectedId() - 1));
    session_.setCredit(c);
    const int keep = selected_;
    reload();
    list_.selectRow(keep, true, true);
}

void ClearancePanel::resized() {
    auto r = getLocalBounds().reduced(14);
    summary_.setBounds(r.removeFromTop(26));
    hint_.setBounds(r.removeFromTop(40));
    r.removeFromTop(6);
    auto buttons = r.removeFromBottom(30);
    close_.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(8);
    export_.setBounds(buttons.removeFromRight(240));
    r.removeFromBottom(10);
    auto right = r.removeFromRight(360);
    r.removeFromRight(10);
    list_.setBounds(r);
    auto statusRow = right.removeFromTop(28);
    status_.setBounds(statusRow);
    right.removeFromTop(6);
    for (size_t i = 0; i < fields_.size(); ++i) {
        fields_[i].label.setBounds(right.removeFromTop(16));
        fields_[i].edit.setBounds(right.removeFromTop(i == 6 ? 46 : 24));
        right.removeFromTop(3);
    }
    save_.setBounds(right.removeFromTop(28).removeFromRight(130));
}

void ClearancePanel::paint(juce::Graphics& g) { g.fillAll(theme_.surface); }

void ClearancePanel::show(Session& session, const Theme& theme) {
    if (!session.hasProject()) return;
    launch(new ClearancePanel(session, theme), "Sample Clearance", theme);
}

// ================================================================ inspired by

InspirePanel::InspirePanel(Session& session, const Theme& theme, ynot::ReferenceProfile ref, std::string referenceName)
    : session_(session), theme_(theme), ref_(std::move(ref)), refName_(std::move(referenceName)) {
    for (int b : {2, 4, 8}) bars_.addItem(juce::String(b) + " bars", b);
    bars_.setSelectedId(4, juce::dontSendNotification);
    bars_.onChange = [this] { generate(idea_.seed); };
    useTempo_.setToggleState(true, juce::dontSendNotification);
    preview_.onClick = [this] {
        if (session_.isPreviewing()) {
            session_.stopPreview();
            preview_.setButtonText("Preview");
            return;
        }
        session_.previewAudio(ynot::renderIdea(idea_, session_.sampleRate()));
        preview_.setButtonText("Stop preview");
    };
    another_.onClick = [this] { generate(idea_.seed + 101); };
    addTracks_.onClick = [this] {
        session_.stopPreview();
        const auto ids = session_.addIdea(idea_, useTempo_.getToggleState(), refName_);
        status_.setText("Added " + juce::String(static_cast<int>(ids.size())) + " tracks at the playhead. Edit them like any MIDI.",
                        juce::dontSendNotification);
    };
    toSampler_.onClick = [this] {
        session_.stopPreview();
        session_.ideaToSampler(idea_, refName_);
        status_.setText("Added a Ynot Sampler track with the idea chopped onto 16 pads.", juce::dontSendNotification);
    };
    close_.onClick = [this] { closeDialog(*this); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&bars_, &useTempo_, &preview_, &another_, &addTracks_,
                                                                        &toSampler_, &close_, &status_})
        addAndMakeVisible(c);
    status_.setColour(juce::Label::textColourId, theme.secondary);
    generate(static_cast<uint32_t>(juce::Time::currentTimeMillis() & 0xffff) + 1);
    setSize(640, 400);
}

InspirePanel::~InspirePanel() { session_.stopPreview(); }

void InspirePanel::generate(uint32_t seed) {
    session_.stopPreview();
    preview_.setButtonText("Preview");
    idea_ = ynot::generateOriginalIdea(ref_, seed, bars_.getSelectedId() > 0 ? bars_.getSelectedId() : 4);
    status_.setText({}, juce::dontSendNotification);
    repaint();
}

void InspirePanel::paint(juce::Graphics& g) {
    g.fillAll(theme_.surface);
    auto r = getLocalBounds().reduced(16);
    auto line = [&](const juce::String& text, float size, juce::Colour c, bool bold = false, int h = 20) {
        g.setColour(c);
        g.setFont(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
        g.drawFittedText(text, r.removeFromTop(h), juce::Justification::topLeft, 2);
    };
    juce::String chords;
    for (const auto& c : ref_.chords) chords << (chords.isEmpty() ? "" : " - ") << c;
    line("Reference: " + juce::String(refName_), theme_.bodySize, theme_.textMuted);
    line(juce::String(juce::roundToInt(ref_.bpm)) + " BPM, " + juce::String(ref_.key.name()) +
             (chords.isEmpty() ? juce::String() : ", chords " + chords) + "  (listened to, not copied)",
         theme_.bodySize, theme_.textMuted);
    r.removeFromTop(10);
    line("Your new idea", theme_.bodySize, theme_.textMuted);
    line(juce::String(idea_.description), 20.f, theme_.text, true, 28);
    // Originality meter.
    const double diff = 1.0 - idea_.similarity;
    const bool ok = idea_.similarity < ynot::kMaxSimilarity;
    auto meter = r.removeFromTop(16).withWidth(300).toFloat();
    g.setColour(theme_.background);
    g.fillRoundedRectangle(meter, 4.f);
    if (ref_.melody.size() >= 4) {
        g.setColour(ok ? theme_.secondary : theme_.accent);
        g.fillRoundedRectangle(meter.withWidth(static_cast<float>(meter.getWidth() * diff)), 4.f);
    }
    r.removeFromTop(4);
    if (ref_.melody.size() < 4)
        line("The reference's melody couldn't be picked out, so it wasn't compared: listen and make sure it's your own",
             theme_.bodySize, theme_.accent, false, 36);
    else
        line(ok ? "Melody " + juce::String(juce::roundToInt(diff * 100)) + "% different from the reference: clearly its own tune"
                : juce::String("Close to the reference's melody: press Try another idea"),
             theme_.bodySize, ok ? theme_.secondary : theme_.accent);
    r.removeFromTop(8);
    line("Written from scratch by Ynot: new chord voicings, bass, melody and drums. Tempo, key and a general chord "
         "movement aren't owned by anyone; the melody is checked against the reference. This isn't legal advice: if "
         "it still sounds like the original to you, change it.",
         theme_.bodySize - 2.f, theme_.textMuted, false, 48);
}

void InspirePanel::resized() {
    auto r = getLocalBounds().reduced(16);
    auto buttons = r.removeFromBottom(30);
    close_.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(8);
    toSampler_.setBounds(buttons.removeFromRight(210));
    buttons.removeFromRight(8);
    addTracks_.setBounds(buttons.removeFromRight(190));
    r.removeFromBottom(8);
    status_.setBounds(r.removeFromBottom(22));
    auto row = r.removeFromBottom(30);
    preview_.setBounds(row.removeFromLeft(120));
    row.removeFromLeft(8);
    another_.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(8);
    bars_.setBounds(row.removeFromLeft(90));
    row.removeFromLeft(8);
    useTempo_.setBounds(row);
}

void InspirePanel::chooseReferenceAndShow(Session& session, const Theme& theme) {
    if (!session.hasProject()) return;
    auto chooser = std::make_shared<juce::FileChooser>("Choose a reference song (it's analysed, never copied into your song)",
                                                       juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                       importFilePatterns());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [chooser, &session, &theme](const juce::FileChooser& fc) {
                             const auto f = fc.getResult();
                             if (f == juce::File()) return;
                             session.analyzeReferenceFile(f, [&session, &theme, name = f.getFileNameWithoutExtension()](
                                                                 const ynot::ReferenceProfile& ref, const juce::String& err) {
                                 if (err.isNotEmpty()) {
                                     juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                            "Can't read the reference", err);
                                     return;
                                 }
                                 launch(new InspirePanel(session, theme, ref, name.toStdString()),
                                        "Ynot AI - Original Idea", theme);
                             });
                         });
}

// ================================================================ sound packs

SoundPacksPanel::SoundPacksPanel(Session& session, const Theme& theme) : session_(session), theme_(theme) {
    list_.setModel(this);
    list_.setRowHeight(30);
    list_.setColour(juce::ListBox::backgroundColourId, theme.background);
    addAndMakeVisible(list_);
    hint_.setText("Made by Ynot (synthesised, no recordings): royalty-free and cleared for any release.",
                  juce::dontSendNotification);
    hint_.setColour(juce::Label::textColourId, theme.secondary);
    addAndMakeVisible(hint_);
    status_.setColour(juce::Label::textColourId, theme.textMuted);
    addAndMakeVisible(status_);
    preview_.onClick = [this] { preview(); };
    stop_.onClick = [this] { session_.stopPreview(); };
    toSampler_.onClick = [this] {
        if (const auto* s = selected()) {
            const auto id = session_.addPackSoundToSampler(s->id);
            status_.setText("Added \"" + juce::String(s->name) + "\" to a new Sampler track" +
                                (s->kind == "loop" ? juce::String(", chopped onto 16 pads.") : juce::String(", playable across the keys.")),
                            juce::dontSendNotification);
            if (onSampler && !id.empty()) onSampler(id);
        }
    };
    asClip_.onClick = [this] {
        if (const auto* s = selected()) {
            session_.addPackSoundAsClip(s->id);
            status_.setText("Added \"" + juce::String(s->name) + "\" as an audio clip at the playhead.", juce::dontSendNotification);
        }
    };
    close_.onClick = [this] { closeDialog(*this); };
    for (auto* b : {&preview_, &stop_, &toSampler_, &asClip_, &close_}) addAndMakeVisible(*b);
    list_.selectRow(0);
    setSize(620, 560);
}

SoundPacksPanel::~SoundPacksPanel() { session_.stopPreview(); }

int SoundPacksPanel::getNumRows() { return static_cast<int>(ynot::packSounds().size()); }

const ynot::PackSound* SoundPacksPanel::selected() const {
    const int row = list_.getSelectedRow();
    return row >= 0 && row < static_cast<int>(ynot::packSounds().size()) ? &ynot::packSounds()[static_cast<size_t>(row)] : nullptr;
}

void SoundPacksPanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row < 0 || row >= getNumRows()) return;
    const auto& s = ynot::packSounds()[static_cast<size_t>(row)];
    if (selected) g.fillAll(theme_.surfaceRaised);
    g.setColour(theme_.text);
    g.setFont(juce::FontOptions(theme_.bodySize, juce::Font::bold));
    g.drawText(juce::String(s.name), 10, 0, w / 2, h, juce::Justification::centredLeft, true);
    g.setColour(theme_.textMuted);
    g.setFont(juce::FontOptions(theme_.bodySize - 1.f));
    juce::String info = juce::String(s.pack);
    if (s.kind == "loop") info << "  |  " << juce::roundToInt(s.bpm) << " BPM" << (s.key.empty() ? "" : ", " + juce::String(s.key));
    else info << "  |  one-shot";
    g.drawText(info, w / 2, 0, w / 2 - 10, h, juce::Justification::centredRight, true);
}

void SoundPacksPanel::listBoxItemDoubleClicked(int, const juce::MouseEvent&) { preview(); }

void SoundPacksPanel::preview() {
    if (const auto* s = selected()) session_.previewAudio(ynot::renderPackSound(s->id, session_.sampleRate()));
}

void SoundPacksPanel::resized() {
    auto r = getLocalBounds().reduced(14);
    hint_.setBounds(r.removeFromTop(22));
    r.removeFromTop(6);
    auto buttons = r.removeFromBottom(30);
    close_.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(8);
    asClip_.setBounds(buttons.removeFromRight(150));
    buttons.removeFromRight(8);
    toSampler_.setBounds(buttons.removeFromRight(130));
    buttons.removeFromRight(8);
    preview_.setBounds(buttons.removeFromLeft(90));
    buttons.removeFromLeft(8);
    stop_.setBounds(buttons.removeFromLeft(70));
    r.removeFromBottom(6);
    status_.setBounds(r.removeFromBottom(22));
    list_.setBounds(r);
}

void SoundPacksPanel::paint(juce::Graphics& g) { g.fillAll(theme_.surface); }

void SoundPacksPanel::show(Session& session, const Theme& theme, std::function<void(const std::string&)> onSampler) {
    if (!session.hasProject()) return;
    auto* p = new SoundPacksPanel(session, theme);
    p->onSampler = std::move(onSampler);
    launch(p, "Ynot Sound Packs", theme);
}

}  // namespace ynotapp
