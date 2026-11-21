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

bool canScanPlugins() {
#if JUCE_IOS || JUCE_ANDROID
    return false;  // mobile: AUv3 discovery is system-managed (M4); Android has no plugin scan
#else
    return true;
#endif
}
}  // namespace

MainComponent::MainComponent(Session& session, JucePluginHost& host)
    : session_(session),
      host_(host),
      theme_(Theme::load(true)),
      arrange_(session, host, theme_),
      keyboard_(session.keyboardState(), juce::KeyboardComponentBase::horizontalKeyboard) {
    theme_.apply(lnf_);
    setLookAndFeel(&lnf_);
    setWantsKeyboardFocus(true);

    for (auto* b : {&playButton_, &recordButton_, &clickButton_, &countInButton_, &monitorButton_, &keysButton_,
                    &undoButton_, &redoButton_, &newButton_, &openButton_, &saveButton_, &audioButton_, &scanButton_,
                    &moreButton_})
        addAndMakeVisible(*b);
    addChildComponent(keyboard_);
    keyboard_.setWantsKeyboardFocus(false);  // computer keys stay shortcuts (Space, S, Delete...)
    keyboard_.setAvailableRange(24, 108);
    keyboard_.setLowestVisibleKey(48);
    keysButton_.setClickingTogglesState(true);
    keysButton_.onClick = [this] {
        keyboard_.setVisible(keysButton_.getToggleState());
        resized();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&status_, &arrange_, &compatTitle_, &target_,
                                                           &prepareButton_, &compatSummary_, &compat_})
        addAndMakeVisible(*c);

    recordButton_.setColour(juce::TextButton::buttonOnColourId, theme_.record);
    for (auto* b : {&clickButton_, &countInButton_, &monitorButton_}) b->setClickingTogglesState(true);
    keyboard_.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, theme_.primary);
    keyboard_.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, theme_.primary.withAlpha(0.3f));

    playButton_.onClick = [this] { togglePlay(); };
    recordButton_.onClick = [this] { toggleRecording(); };
    clickButton_.onClick = [this] { session_.setMetronome(clickButton_.getToggleState()); };
    countInButton_.onClick = [this] { session_.setCountInBars(countInButton_.getToggleState() ? 1 : 0); };
    monitorButton_.onClick = [this] { session_.setMonitoring(monitorButton_.getToggleState()); };
    undoButton_.onClick = [this] { session_.undo(); };
    redoButton_.onClick = [this] { session_.redo(); };
    newButton_.onClick = [this] { newDemo(); };
    openButton_.onClick = [this] { openProject(); };
    saveButton_.onClick = [this] { session_.save(); };
    audioButton_.onClick = [this] { showAudioSettings(); };
    scanButton_.onClick = [this] { scanPlugins(); };
    moreButton_.onClick = [this] { showMoreMenu(); };
    prepareButton_.onClick = [this] { prepareForMobile(); };
    compatSummary_.onClick = [this] { showCompatibilityDialog(); };

    compatTitle_.setText("Open this project on", juce::dontSendNotification);
    for (int i = 0; i < 4; ++i) target_.addItem(kTargetNames[i], i + 1);
    target_.setSelectedId(3, juce::dontSendNotification);  // iPhone/iPad: the most common handoff
    target_.onChange = [this] { refreshCompatibility(); };
    compat_.setMultiLine(true);
    compat_.setReadOnly(true);
    compat_.setScrollbarsShown(true);

    session_.onChange = [this] { refresh(); };
    setSize(1280, 800);
    refresh();
    startTimerHz(10);
}

MainComponent::~MainComponent() {
    session_.onChange = nullptr;
    setLookAndFeel(nullptr);
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(theme_.background); }

void MainComponent::resized() {
    layout_ = ynot::decideLayout({layoutPlatformOverride.value_or(ynot::currentPlatform()),
                                  static_cast<double>(getWidth()), static_cast<double>(getHeight()),
                                  hasPrecisePointer(), hasPrecisePointer()});
    const int target = static_cast<int>(std::max(layout_.minTouchTargetDp, 32.0));
    auto r = getLocalBounds().reduced(12);

    // Toolbar. Phones keep transport + editing and move file/device actions into "More".
    // Phones: Count-in and Monitor live in "More" as tick items to keep the toolbar to two rows.
    std::vector<juce::TextButton*> toolbar{&playButton_, &recordButton_, &clickButton_};
    countInButton_.setVisible(!compact());
    monitorButton_.setVisible(!compact());
    if (!compact()) {
        toolbar.push_back(&countInButton_);
        toolbar.push_back(&monitorButton_);
    }
    for (auto* b : {&keysButton_, &undoButton_, &redoButton_}) toolbar.push_back(b);
    for (auto* b : {&newButton_, &openButton_, &saveButton_, &audioButton_, &scanButton_}) {
        const bool show = !compact() && (b != &scanButton_ || canScanPlugins());
        b->setVisible(show);
        if (show) toolbar.push_back(b);
    }
    moreButton_.setVisible(compact());
    if (compact()) toolbar.push_back(&moreButton_);

    const int buttonWidth = compact() ? std::max(target * 2 - 8, 80) : 96;
    auto bar = r.removeFromTop(target);
    for (auto* b : toolbar) {
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

    if (layout_.showPerTrackInspector) {
        auto side = r.removeFromRight(std::min(380, r.getWidth() / 4 + 60));
        r.removeFromRight(12);
        compatTitle_.setBounds(side.removeFromTop(24));
        target_.setBounds(side.removeFromTop(target));
        side.removeFromTop(6);
        prepareButton_.setBounds(side.removeFromTop(target));
        side.removeFromTop(6);
        compat_.setBounds(side);
        compatSummary_.setVisible(false);
        for (auto* c : std::initializer_list<juce::Component*>{&compatTitle_, &prepareButton_, &compat_}) c->setVisible(true);
    } else {
        // Phones / narrow windows: one compact row; the full report opens on demand.
        auto row = r.removeFromBottom(target);
        r.removeFromBottom(8);
        target_.setBounds(row.removeFromLeft(row.getWidth() / 2));
        row.removeFromLeft(6);
        compatSummary_.setBounds(row);
        compatSummary_.setVisible(true);
        for (auto* c : std::initializer_list<juce::Component*>{&compatTitle_, &prepareButton_, &compat_}) c->setVisible(false);
    }
    if (keyboard_.isVisible()) {
        // Bigger keys for fingers than for a mouse.
        const bool touch = layout_.minTouchTargetDp >= 44;
        keyboard_.setKeyWidth(touch ? 40.f : 22.f);
        keyboard_.setBounds(r.removeFromBottom(touch ? 140 : 90));
        r.removeFromBottom(8);
    }
    arrange_.setBounds(r);
    arrange_.setLayout(layout_);
}

bool MainComponent::keyPressed(const juce::KeyPress& key) {
    const auto cmd = juce::ModifierKeys::commandModifier;
    if (key == juce::KeyPress('z', cmd, 0)) return session_.undo(), true;
    if (key == juce::KeyPress('z', cmd | juce::ModifierKeys::shiftModifier, 0) ||
        key == juce::KeyPress('y', cmd, 0))
        return session_.redo(), true;
    if (key == juce::KeyPress('s', cmd, 0)) return session_.save(), true;
    if (key == juce::KeyPress::spaceKey) return togglePlay(), true;
    return arrange_.keyPressed(key);
}

void MainComponent::timerCallback() {
    if (!session_.hasProject()) return;
    auto* dev = session_.device().manager().getCurrentAudioDevice();
    juce::String s;
    s << juce::String(session_.project().name) << "   ";
    s << juce::String(session_.positionSeconds(), 1) << " s   ";
    if (session_.isRecording()) s << "REC " << juce::String(session_.recordingSeconds(), 1) << " s   ";
    if (session_.monitoring() && dev) s << "monitor " << juce::String(session_.monitoringLatencyMs(), 1) << " ms   ";
    s << (dev ? dev->getTypeName() + ": " + dev->getName() : juce::String("no audio device"));
    if (keyboard_.isVisible()) {
        bool instrumentArmed = false;
        for (auto& t : session_.project().tracks) instrumentArmed |= t.armed && t.kind == ynot::TrackKind::Instrument;
        if (!instrumentArmed) s << "   Arm an instrument track (R) to play";
    }
    status_.setText(s, juce::dontSendNotification);
    playButton_.setButtonText(session_.isPlaying() ? "Stop" : "Play");
}

void MainComponent::refresh() {
    playButton_.setButtonText(session_.isPlaying() ? "Stop" : "Play");
    playButton_.setToggleState(session_.isPlaying(), juce::dontSendNotification);
    recordButton_.setToggleState(session_.isRecording(), juce::dontSendNotification);
    recordButton_.setButtonText(session_.isRecording() ? "Stop Rec" : "Record");
    undoButton_.setEnabled(session_.canUndo() && !session_.isRecording());
    redoButton_.setEnabled(session_.canRedo() && !session_.isRecording());
    arrange_.refresh();
    refreshCompatibility();
}

ynot::Platform MainComponent::selectedTarget() const {
    int idx = std::clamp(target_.getSelectedId() - 1, 0, 3);
    return kTargets[idx];
}

void MainComponent::refreshCompatibility() {
    if (!session_.hasProject()) {
        compatText_ = "Create or open a project to check it against other devices.";
        compat_.setText(compatText_);
        compatSummary_.setButtonText("No project");
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
    compatText_ = out;
    compat_.setText(out);
    prepareButton_.setEnabled(!session_.tracksToPrepareForMobile().empty());
    const int problems = report.count(ynot::Severity::Blocking) + report.count(ynot::Severity::Warning);
    compatSummary_.setButtonText(problems == 0 ? juce::String("Compatible") : juce::String(problems) + " issue" +
                                                                                  (problems == 1 ? "" : "s"));
    compatSummary_.setColour(juce::TextButton::textColourOffId,
                             report.count(ynot::Severity::Blocking) > 0 ? theme_.record : theme_.text);
}

void MainComponent::togglePlay() {
    if (session_.isRecording()) toggleRecording();
    else if (session_.isPlaying()) session_.stop();
    else session_.play();
    refresh();
}

void MainComponent::newDemo() {
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                   .getChildFile("Ynot").getNonexistentChildFile("Demo Song", ".ynot");
    session_.newDemo(dir);
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

void MainComponent::showMoreMenu() {
    juce::PopupMenu m;
    m.addItem("New Demo", [this] { newDemo(); });
    m.addItem("Open...", [this] { openProject(); });
    m.addItem("Save", [this] { session_.save(); });
    m.addSeparator();
    m.addItem("Count-in (1 bar)", true, session_.countInBars() > 0, [this] {
        session_.setCountInBars(session_.countInBars() > 0 ? 0 : 1);
        countInButton_.setToggleState(session_.countInBars() > 0, juce::dontSendNotification);
    });
    m.addItem("Monitor input", true, session_.monitoring(), [this] {
        session_.setMonitoring(!session_.monitoring());
        monitorButton_.setToggleState(session_.monitoring(), juce::dontSendNotification);
    });
    m.addSeparator();
    m.addItem("Split clip at playhead", arrange_.hasSelection(), false, [this] { arrange_.splitSelectedAtPlayhead(); });
    m.addItem("Delete clip", arrange_.hasSelection(), false, [this] { arrange_.deleteSelected(); });
    m.addSeparator();
    m.addItem("Prepare for iPhone & Android", !session_.tracksToPrepareForMobile().empty(), false,
              [this] { prepareForMobile(); });
    m.addItem("Audio settings...", [this] { showAudioSettings(); });
    if (canScanPlugins()) m.addItem("Scan plugins", [this] { scanPlugins(); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(moreButton_));
}

void MainComponent::showCompatibilityDialog() {
    auto* editor = new juce::TextEditor();
    editor->setMultiLine(true);
    editor->setReadOnly(true);
    editor->setScrollbarsShown(true);
    editor->setText(compatText_);
    editor->setSize(std::max(300, getWidth() - 24), std::max(300, getHeight() * 2 / 3));
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(editor);
    o.dialogTitle = "Open this project on " + juce::String(kTargetNames[target_.getSelectedId() - 1]);
    o.dialogBackgroundColour = theme_.surface;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::toggleRecording() {
    if (session_.isRecording()) {
        auto result = session_.stopRecording();
        juce::String msg;
        if (result.droppedMidiEvents > 0)
            msg << juce::String(result.droppedMidiEvents) << " MIDI events were lost (take too dense).\n";
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
    session_.reload();  // newly found plugins become live (not an undoable edit)
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
