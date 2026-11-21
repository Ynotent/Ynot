#include "MainComponent.h"

namespace ynotapp {

std::optional<ynot::Platform> MainComponent::layoutPlatformOverride;

namespace {
const ynot::Platform kTargets[] = {ynot::Platform::MacOS, ynot::Platform::Windows, ynot::Platform::IOS,
                                   ynot::Platform::Android};
const char* kTargetNames[] = {"macOS", "Windows", "iPhone / iPad", "Android"};

bool hasPrecisePointer() {
    if (auto p = MainComponent::layoutPlatformOverride; p && (*p == ynot::Platform::IOS || *p == ynot::Platform::Android))
        return false;
#if JUCE_IOS || JUCE_ANDROID
    return false;  // updated when a pointer device is attached (M4/M5)
#else
    return true;
#endif
}
}  // namespace

constexpr int kStackBelowWidth = 560;

// One track row: name, source, badges and input/arm/mute/solo/freeze controls.
class MainComponent::TrackRow : public juce::Component {
public:
    TrackRow(MainComponent& owner) : owner_(owner) {
        for (auto* b : {&arm_, &input_, &mute_, &solo_, &freeze_}) addAndMakeVisible(*b);
        arm_.setColour(juce::TextButton::buttonOnColourId, owner_.theme_.record);
        arm_.onClick = [this] {
            if (owner_.session_.isRecording()) return;  // arming changes are blocked mid-take
            owner_.session_.edit([id = id_](ynot::Project& p) { if (auto* t = p.findTrack(id)) t->armed = !t->armed; });
        };
        input_.onClick = [this] { owner_.session_.cycleInput(id_); };
        mute_.setClickingTogglesState(false);
        solo_.setClickingTogglesState(false);
        mute_.onClick = [this] { owner_.session_.edit([id = id_](ynot::Project& p) { if (auto* t = p.findTrack(id)) t->mute = !t->mute; }); };
        solo_.onClick = [this] { owner_.session_.edit([id = id_](ynot::Project& p) { if (auto* t = p.findTrack(id)) t->solo = !t->solo; }); };
        freeze_.onClick = [this] {
            const auto* t = owner_.session_.project().findTrack(id_);
            if (!t) return;
            try {
                if (t->frozen) owner_.session_.unfreeze(id_);
                else owner_.session_.freeze(id_);
            } catch (const std::exception& e) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Can't freeze", e.what());
            }
        };
    }

    void update(const ynot::Track& t, bool canFreezeHere) {
        id_ = t.id;
        track_ = t;
        const bool audio = t.kind == ynot::TrackKind::Audio;
        arm_.setVisible(audio);  // MIDI recording for instrument tracks comes later in M1
        input_.setVisible(audio);
        arm_.setToggleState(t.armed, juce::dontSendNotification);
        input_.setButtonText(owner_.session_.inputLabel(t.id));
        mute_.setToggleState(t.mute, juce::dontSendNotification);
        solo_.setToggleState(t.solo, juce::dontSendNotification);
        freeze_.setButtonText(t.frozen ? "Unfreeze" : "Freeze");
        freeze_.setEnabled(t.frozen || canFreezeHere);
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
        auto text = stacked() ? r.removeFromTop(r.getHeight() / 2 - 2) : r.withTrimmedRight(controlsWidth());
        g.setColour(th.text);
        g.setFont(juce::FontOptions(th.bodySize + 1.f, juce::Font::bold));
        g.drawText(track_.name, text.removeFromTop(text.getHeight() / 2), juce::Justification::bottomLeft);
        juce::String detail = track_.kind == ynot::TrackKind::Audio ? "Audio" : "Instrument";
        if (track_.instrument) detail << " - " << track_.instrument->name;
        for (auto& p : track_.inserts) detail << " - " << p.name;
        if (track_.frozen) detail << "   FROZEN";
        g.setColour(track_.frozen ? th.secondary : th.textMuted);
        g.setFont(juce::FontOptions(th.bodySize));
        g.drawText(detail, text, juce::Justification::topLeft, true);
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8, 6);
        if (stacked()) r = r.removeFromBottom(r.getHeight() / 2 + 2).withTrimmedLeft(16);  // controls under the name
        const int unit = r.getHeight();
        freeze_.setBounds(r.removeFromRight(unit * 2));
        r.removeFromRight(6);
        solo_.setBounds(r.removeFromRight(unit));
        r.removeFromRight(4);
        mute_.setBounds(r.removeFromRight(unit));
        r.removeFromRight(4);
        arm_.setBounds(r.removeFromRight(unit));
        r.removeFromRight(4);
        input_.setBounds(r.removeFromRight(unit * 3 / 2));
    }

private:
    // Narrow rows (phones, split-screen) put controls on a second line.
    bool stacked() const { return getWidth() < kStackBelowWidth; }
    int controlsWidth() const {
        const int unit = getHeight() - 12;
        int w = unit * 4 + 24;  // M, S, Freeze (2 units) and gaps
        if (arm_.isVisible()) w += unit + 4;
        if (input_.isVisible()) w += unit * 3 / 2 + 4;
        return w;
    }
    MainComponent& owner_;
    std::string id_;
    ynot::Track track_;
    juce::TextButton arm_{"R"}, input_{"In 1"}, mute_{"M"}, solo_{"S"}, freeze_{"Freeze"};
};

MainComponent::MainComponent(Session& session, JucePluginHost& host)
    : session_(session), host_(host), theme_(Theme::load(true)) {
    theme_.apply(lnf_);
    setLookAndFeel(&lnf_);

    for (auto* b : {&newButton_, &openButton_, &saveButton_, &playButton_, &recordButton_, &clickButton_,
                    &countInButton_, &monitorButton_, &audioButton_, &scanButton_})
        addAndMakeVisible(*b);
    countInButton_.setClickingTogglesState(true);
    countInButton_.onClick = [this] { session_.setCountInBars(countInButton_.getToggleState() ? 1 : 0); };
    monitorButton_.setClickingTogglesState(true);
    monitorButton_.onClick = [this] { session_.setMonitoring(monitorButton_.getToggleState()); };
    recordButton_.setColour(juce::TextButton::buttonOnColourId, theme_.record);
    recordButton_.onClick = [this] { toggleRecording(); };
    clickButton_.setClickingTogglesState(true);
    clickButton_.onClick = [this] { session_.setMetronome(clickButton_.getToggleState()); };
    addAndMakeVisible(status_);
    addAndMakeVisible(tracks_);
    addAndMakeVisible(compatTitle_);
    addAndMakeVisible(target_);
    addAndMakeVisible(prepareButton_);
    addAndMakeVisible(compat_);

    tracks_.setModel(this);
    compatTitle_.setText("Open this project on", juce::dontSendNotification);
    for (int i = 0; i < 4; ++i) target_.addItem(kTargetNames[i], i + 1);
    target_.setSelectedId(3, juce::dontSendNotification);  // iPhone/iPad: the most common handoff
    target_.onChange = [this] { refreshCompatibility(); };
    compat_.setMultiLine(true);
    compat_.setReadOnly(true);
    compat_.setScrollbarsShown(true);

    newButton_.onClick = [this] {
        auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                       .getChildFile("Ynot").getNonexistentChildFile("Demo Song", ".ynot");
        session_.newDemo(dir);
    };
    openButton_.onClick = [this] { openProject(); };
    saveButton_.onClick = [this] { session_.save(); };
    playButton_.onClick = [this] {
        if (session_.isPlaying()) session_.stop();
        else session_.play();
        refresh();
    };
    audioButton_.onClick = [this] { showAudioSettings(); };
    scanButton_.onClick = [this] { scanPlugins(); };
    prepareButton_.onClick = [this] { prepareForMobile(); };
#if JUCE_IOS || JUCE_ANDROID
    scanButton_.setVisible(false);  // mobile: AUv3 discovery is system-managed (M4)
#endif

    session_.onChange = [this] { refresh(); };
    setSize(1280, 800);
    refresh();
    startTimerHz(10);
}

MainComponent::~MainComponent() {
    session_.onChange = nullptr;
    tracks_.setModel(nullptr);
    setLookAndFeel(nullptr);
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(theme_.background); }

void MainComponent::resized() {
    layout_ = ynot::decideLayout({layoutPlatformOverride.value_or(ynot::currentPlatform()), static_cast<double>(getWidth()),
                                  static_cast<double>(getHeight()), hasPrecisePointer(), hasPrecisePointer()});
    const int target = static_cast<int>(std::max(layout_.minTouchTargetDp, 32.0));
    auto r = getLocalBounds().reduced(12);

    // Toolbar: buttons flow onto extra rows when the window is narrow (phones,
    // split-screen tablets, small desktop windows).
    const bool compact = layout_.layout == ynot::LayoutClass::Compact;
    const int buttonWidth = compact ? std::max(target * 2, 84) : 110;
    auto bar = r.removeFromTop(target);
    for (auto* b : {&playButton_, &recordButton_, &clickButton_, &countInButton_, &monitorButton_, &newButton_,
                    &openButton_, &saveButton_, &audioButton_, &scanButton_}) {
        if (!b->isVisible()) continue;
        if (bar.getWidth() < buttonWidth) {
            r.removeFromTop(6);
            bar = r.removeFromTop(target);
        }
        b->setBounds(bar.removeFromLeft(buttonWidth));
        bar.removeFromLeft(6);
    }
    if (bar.getWidth() < 160) {  // status gets its own row if there's no room beside the buttons
        r.removeFromTop(4);
        bar = r.removeFromTop(24);
    }
    status_.setBounds(bar);
    r.removeFromTop(10);

    const int listWidth = layout_.showPerTrackInspector ? r.getWidth() - std::min(420, r.getWidth() / 3) - 12 : r.getWidth();
    tracks_.setRowHeight(listWidth < kStackBelowWidth ? target * 2 + 24 : std::max(target + 12, 56));
    if (layout_.showPerTrackInspector) {
        auto side = r.removeFromRight(std::min(420, r.getWidth() / 3));
        r.removeFromRight(12);
        compatTitle_.setBounds(side.removeFromTop(24));
        target_.setBounds(side.removeFromTop(target));
        side.removeFromTop(6);
        prepareButton_.setBounds(side.removeFromTop(target));
        side.removeFromTop(6);
        compat_.setBounds(side);
        tracks_.setBounds(r);
    } else {
        auto bottom = r.removeFromBottom(r.getHeight() * 2 / 5);
        tracks_.setBounds(r);
        bottom.removeFromTop(8);
        auto row = bottom.removeFromTop(target);
        target_.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(0, 0));
        row.removeFromLeft(6);
        prepareButton_.setBounds(row);
        compatTitle_.setBounds({});
        bottom.removeFromTop(6);
        compat_.setBounds(bottom);
    }
}

int MainComponent::getNumRows() {
    return session_.hasProject() ? static_cast<int>(session_.project().tracks.size()) : 0;
}

void MainComponent::paintListBoxItem(int, juce::Graphics&, int, int, bool) {}

juce::Component* MainComponent::refreshComponentForRow(int row, bool, juce::Component* existing) {
    auto* rowComp = dynamic_cast<TrackRow*>(existing);
    if (!session_.hasProject() || row >= getNumRows()) {
        delete existing;
        return nullptr;
    }
    if (!rowComp) {
        delete existing;
        rowComp = new TrackRow(*this);
    }
    auto ordered = session_.project().orderedTracks();
    const ynot::Track& t = *ordered[static_cast<size_t>(row)];
    bool canFreeze = true;
    auto check = [&](const ynot::PluginInstance& p) { canFreeze &= p.bypassed || host_.isAvailable(p); };
    if (t.instrument) check(*t.instrument);
    for (auto& p : t.inserts) check(p);
    rowComp->update(t, canFreeze);
    return rowComp;
}

void MainComponent::timerCallback() {
    if (!session_.hasProject()) return;
    auto* dev = session_.device().manager().getCurrentAudioDevice();
    juce::String s;
    s << juce::String(session_.project().name) << "   ";
    s << juce::String(session_.positionSeconds(), 1) << " s   ";
    if (session_.isRecording()) s << "REC " << juce::String(session_.recordingSeconds(), 1) << " s   ";
    if (session_.monitoring() && dev)
        s << "monitor " << juce::String(session_.monitoringLatencyMs(), 1) << " ms   ";
    s << (dev ? dev->getTypeName() + ": " + dev->getName() : juce::String("no audio device"));
    status_.setText(s, juce::dontSendNotification);
}

void MainComponent::refresh() {
    playButton_.setButtonText(session_.isPlaying() ? "Stop" : "Play");
    recordButton_.setToggleState(session_.isRecording(), juce::dontSendNotification);
    recordButton_.setButtonText(session_.isRecording() ? "Stop Rec" : "Record");
    playButton_.setToggleState(session_.isPlaying(), juce::dontSendNotification);
    tracks_.updateContent();
    tracks_.repaint();
    refreshCompatibility();
}

ynot::Platform MainComponent::selectedTarget() const {
    int idx = std::clamp(target_.getSelectedId() - 1, 0, 3);
    return kTargets[idx];
}

void MainComponent::refreshCompatibility() {
    if (!session_.hasProject()) {
        compat_.setText("Create or open a project to check it against other devices.");
        return;
    }
    juce::String out;
    for (auto& i : session_.engineIssues()) out << "On this device: " << i.message << "\n\n";
    auto report = session_.compatibilityFor(selectedTarget());
    if (report.fullyCompatible())
        out << "This project will open and play on " << kTargetNames[target_.getSelectedId() - 1] << ".\n\n";
    for (auto& i : report.issues) {
        out << (i.severity == ynot::Severity::Blocking ? "[!] " : i.severity == ynot::Severity::Warning ? "[~] " : "[i] ");
        out << i.message << "\n";
        for (auto o : i.options) out << "      - " << ynot::toString(o) << "\n";
        out << "\n";
    }
    compat_.setText(out);
    prepareButton_.setEnabled(!session_.tracksToPrepareForMobile().empty());
}

void MainComponent::openProject() {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Open a Ynot project folder", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
                              auto dir = fc.getResult();
                              if (dir == juce::File()) return;
                              try {
                                  session_.open(dir);
                              } catch (const std::exception& e) {
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Can't open project", e.what());
                              }
                          });
}

void MainComponent::toggleRecording() {
    if (session_.isRecording()) {
        auto result = session_.stopRecording();
        juce::String msg;
        if (result.droppedFrames > 0)
            msg << "The disk couldn't keep up: " << juce::String(result.droppedFrames)
                << " samples were lost. Try a larger audio buffer or a faster drive.\n";
        if (!result.error.empty()) msg << juce::String(result.error);
        if (msg.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Recording", msg);
    } else {
        std::string error;
        if (!session_.startRecording(error))
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Can't record yet", error);
    }
    refresh();
}

void MainComponent::showAudioSettings() {
    // Lists every driver JUCE offers on this OS (CoreAudio, ASIO, WASAPI, iOS Audio, Oboe).
    auto* selector = new juce::AudioDeviceSelectorComponent(session_.device().manager(), 0, 8, 2, 2, false, false,
                                                            true, false);
    selector->setSize(520, 420);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(selector);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = theme_.surface;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::scanPlugins() {
    scanButton_.setEnabled(false);
    scanButton_.setButtonText("Scanning...");
    // Runs on the message thread for M1-alpha; an out-of-process scanner replaces this so a
    // crashing plugin can't take the app down.
    host_.scanDefaultLocations();
    scanButton_.setButtonText("Scan Plugins");
    scanButton_.setEnabled(true);
    session_.edit([](ynot::Project&) {});  // reload so newly found plugins become live
}

void MainComponent::prepareForMobile() {
    auto ids = session_.tracksToPrepareForMobile();
    for (auto& id : ids) {
        try {
            session_.freeze(id);
        } catch (const std::exception& e) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Can't freeze", e.what());
            return;
        }
    }
    refresh();
}

}  // namespace ynotapp
