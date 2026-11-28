#include "MainComponent.h"

#include "AudioImporter.h"
#include "AudioIOPanel.h"
#include "ExportPanel.h"
#include "QuantizeMenus.h"

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
      counter_(session, theme_),
      inspector_(session, arrange_, theme_),
      mixer_(session, theme_),
      steps_(session, theme_),
      keyboard_(session.keyboardState(), juce::KeyboardComponentBase::horizontalKeyboard) {
    theme_.apply(lnf_);
    setLookAndFeel(&lnf_);
    setWantsKeyboardFocus(true);

    for (juce::TextButton* b : std::initializer_list<juce::TextButton*>{
             &startButton_, &playButton_, &recordButton_, &clickButton_, &countInButton_, &monitorButton_, &keysButton_,
                    &undoButton_, &redoButton_, &newButton_, &openButton_, &saveButton_, &audioButton_, &scanButton_,
                    &moreButton_, &quantizeButton_, &punchButton_, &timeSigButton_, &tempoButton_, &rateButton_, &addTrackButton_, &loopButton_,
                    &mixerButton_, &exportButton_, &stepsButton_})
        addAndMakeVisible(*b);
    addChildComponent(steps_);
    stepsButton_.setTooltip("Step sequencer for drum patterns (B)");
    stepsButton_.onClick = [this] { toggleSteps(); };
    arrange_.onOpenPattern = [this](const std::string& t, const std::string& c) { openPattern(t, c); };
    exportButton_.setTooltip("Export the song as WAV, FLAC, M4A... (Cmd/Ctrl+E)");
    exportButton_.onClick = [this] { ExportPanel::show(session_, theme_); };
    addChildComponent(mixer_);
    mixerButton_.setTooltip("Show the mixer (X)");
    mixerButton_.onClick = [this] { toggleMixer(); };
    addTrackButton_.onClick = [this] {
        juce::PopupMenu m;
        addTrackItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(addTrackButton_));
    };
    punchButton_.setColour(juce::TextButton::buttonOnColourId, theme_.record);
    quantizeButton_.onClick = [this] {
        juce::PopupMenu m;
        addQuantizeItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(quantizeButton_));
    };
    punchButton_.onClick = [this] {
        juce::PopupMenu m;
        addPunchItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(punchButton_));
    };
    timeSigButton_.onClick = [this] {
        juce::PopupMenu m;
        addTimeSignatureItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(timeSigButton_));
    };
    loopButton_.setTooltip("Loop a section (L). Option/Alt-drag in the ruler to set it. Recording with the loop on "
                           "records a take on every pass.");
    loopButton_.setColour(juce::TextButton::buttonOnColourId, theme_.secondary);
    loopButton_.onClick = [this] {
        juce::PopupMenu m;
        addLoopItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(loopButton_));
    };
    tempoButton_.setTooltip("Tempo at the playhead. Click to change it or add tempo changes.");
    tempoButton_.onClick = [this] {
        juce::PopupMenu m;
        addTempoItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(tempoButton_));
    };
    rateButton_.setTooltip("Sample rate");
    rateButton_.onClick = [this] {
        juce::PopupMenu m;
        addSampleRateItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(rateButton_));
    };
    addChildComponent(keyboard_);
    // Clicking a button must not steal keyboard focus, or musical typing stops after pressing R/Record.
    for (juce::TextButton* b : std::initializer_list<juce::TextButton*>{
             &startButton_, &playButton_, &recordButton_, &clickButton_, &countInButton_, &monitorButton_, &keysButton_,
                    &undoButton_, &redoButton_, &newButton_, &openButton_, &saveButton_, &audioButton_, &scanButton_,
                    &moreButton_, &prepareButton_, &compatSummary_, &quantizeButton_, &punchButton_, &timeSigButton_,
                    &tempoButton_, &rateButton_, &addTrackButton_, &loopButton_, &mixerButton_, &exportButton_, &stepsButton_})
        b->setMouseClickGrabsKeyboardFocus(false);
    // Musical typing: A W S E D F T G Y H U J K = one octave from middle C. Cmd shortcuts, Space
    // (play/stop) and Return (to start) still work while the keyboard has focus.
    keyboard_.setWantsKeyboardFocus(true);
    keyboard_.setKeyPressBaseOctave(5);
    keyboard_.setAvailableRange(24, 108);
    keyboard_.setLowestVisibleKey(48);
    keysButton_.setClickingTogglesState(true);
    keysButton_.onClick = [this] { showKeyboard(keysButton_.getToggleState()); };
    for (auto* c : std::initializer_list<juce::Component*>{&status_, &arrange_, &compatTitle_, &target_,
                                                           &prepareButton_, &compatSummary_, &compat_})
        addAndMakeVisible(*c);

    recordButton_.setColour(juce::TextButton::buttonOnColourId, theme_.record);
    for (auto* b : {&clickButton_, &countInButton_, &monitorButton_}) b->setClickingTogglesState(true);
    keyboard_.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, theme_.primary);
    keyboard_.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, theme_.primary.withAlpha(0.3f));

    playButton_.onClick = [this] { togglePlay(); };
    startButton_.setTooltip("Go to the start (Return)");
    startButton_.onClick = [this] {
        if (!session_.isRecording()) session_.seekToTick(0);
        arrange_.repaint();
    };
    playButton_.setTooltip("Play / Stop (Space)");
    recordButton_.setTooltip("Record armed tracks");
    addAndMakeVisible(counter_);
    addAndMakeVisible(inspector_);
    arrange_.onFocusTrackChanged = [this](const std::string&) { inspector_.refresh(); };
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#elif !(JUCE_IOS || JUCE_ANDROID)
    addAndMakeVisible(menuBar_);
#endif
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
    scanButton_.setButtonText("Plugins");
    scanButton_.onClick = [this] {
        juce::PopupMenu m;
        addPluginItems(m);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(scanButton_));
    };
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

    // Drag & drop and the track menu route imports through the same path as the file chooser.
    arrange_.onImportFiles = [this](const juce::Array<juce::File>& files, const std::string& trackId, int64_t tick) {
        importFiles(files, trackId, tick);
    };
    arrange_.onImportRequest = [this](const std::string& trackId) { importAudio(trackId); };
    session_.onChange = [this] { refresh(); };
    setSize(1280, 800);
    refresh();
    startTimerHz(10);
}

MainComponent::~MainComponent() {
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    session_.onChange = nullptr;
    setLookAndFeel(nullptr);
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(theme_.background); }

// Desktop and tablet layout, like a studio DAW: transport bar with a big position counter,
// a tool row, the inspector on the left, tracks in the middle, editors docked underneath.
void MainComponent::layoutDesktop() {
    auto r = getLocalBounds();
#if !(JUCE_MAC || JUCE_IOS || JUCE_ANDROID)
    menuBar_.setBounds(r.removeFromTop(24));
#endif
    r = r.reduced(12, 8);
    auto place = [](juce::Rectangle<int>& row, juce::Component& c, int w, bool fromRight = false) {
        c.setVisible(true);
        c.setBounds(fromRight ? row.removeFromRight(w) : row.removeFromLeft(w));
        if (fromRight) row.removeFromRight(6);
        else row.removeFromLeft(6);
    };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&moreButton_, &newButton_, &openButton_})
        c->setVisible(false);

    // Row 1: transport, counter, recording modes; save state and device check on the right.
    auto row1 = r.removeFromTop(46);
    place(row1, startButton_, 46);
    place(row1, playButton_, 58);
    place(row1, recordButton_, 58);
    row1.removeFromLeft(6);
    place(row1, counter_, 280);
    row1.removeFromLeft(6);
    for (auto* b : {&loopButton_, &clickButton_, &countInButton_, &punchButton_, &monitorButton_}) place(row1, *b, 82);
    place(row1, saveButton_, 96, true);
    compatSummary_.setVisible(!compatPanelVisible_);
    if (!compatPanelVisible_ && row1.getWidth() > 160) place(row1, compatSummary_, std::min(300, row1.getWidth()), true);
    else if (!compatPanelVisible_) compatSummary_.setVisible(false);

    // Row 2: song settings and editing on the left, windows and panels on the right.
    r.removeFromTop(6);
    auto row2 = r.removeFromTop(30);
    place(row2, addTrackButton_, 90);
    place(row2, tempoButton_, 96);
    place(row2, timeSigButton_, 60);
    place(row2, rateButton_, 82);
    place(row2, quantizeButton_, 112);
    place(row2, undoButton_, 64);
    place(row2, redoButton_, 64);
    place(row2, keysButton_, 64, true);
    place(row2, stepsButton_, 70, true);
    place(row2, mixerButton_, 70, true);
    place(row2, exportButton_, 80, true);
    if (canScanPlugins()) place(row2, scanButton_, 80, true);
    else scanButton_.setVisible(false);
    place(row2, audioButton_, 80, true);
    status_.setBounds(row2.reduced(4, 0));
    r.removeFromTop(8);

    // Device compatibility (Stage 17): side panel when shown, else the summary chip above.
    for (auto* c : std::initializer_list<juce::Component*>{&compatTitle_, &target_, &prepareButton_, &compat_})
        c->setVisible(compatPanelVisible_);
    if (compatPanelVisible_) {
        auto side = r.removeFromRight(std::min(340, r.getWidth() / 4 + 40));
        r.removeFromRight(10);
        compatTitle_.setBounds(side.removeFromTop(24));
        target_.setBounds(side.removeFromTop(32));
        side.removeFromTop(6);
        prepareButton_.setBounds(side.removeFromTop(32));
        side.removeFromTop(6);
        compat_.setBounds(side);
    }
    inspector_.setVisible(inspectorVisible_ && r.getWidth() >= 900);
    if (inspector_.isVisible()) {
        inspector_.setBounds(r.removeFromLeft(240));
        r.removeFromLeft(8);
    }
    if (keyboard_.isVisible()) {
        keyboard_.setKeyWidth(22.f);
        keyboard_.setBounds(r.removeFromBottom(90));
        r.removeFromBottom(8);
    }
    juce::Component* panel = mixer_.isVisible() ? static_cast<juce::Component*>(&mixer_)
                             : steps_.isVisible() ? static_cast<juce::Component*>(&steps_)
                                                  : nullptr;
    if (panel) {
        const int want = panel == &steps_ ? 60 + 26 * 11 : 340;
        panel->setBounds(r.removeFromBottom(std::min(want, r.getHeight() / 2)));
        r.removeFromBottom(8);
    }
    arrange_.setVisible(true);
    arrange_.setBounds(r);
    arrange_.setLayout(layout_);
}

void MainComponent::resized() {
    layout_ = ynot::decideLayout({layoutPlatformOverride.value_or(ynot::currentPlatform()),
                                  static_cast<double>(getWidth()), static_cast<double>(getHeight()),
                                  hasPrecisePointer(), hasPrecisePointer()});
    const int target = static_cast<int>(std::max(layout_.minTouchTargetDp, 32.0));
    if (!compact()) {
        layoutDesktop();
        return;
    }
    counter_.setVisible(false);
    inspector_.setVisible(false);
    startButton_.setVisible(false);
    target_.setVisible(true);
    auto r = getLocalBounds().reduced(12);
#if !(JUCE_MAC || JUCE_IOS || JUCE_ANDROID)
    // Narrow desktop windows: the menu bar stays, above the phone-style toolbar.
    menuBar_.setBounds(getLocalBounds().removeFromTop(24));
    r.removeFromTop(24);
#endif

    // Toolbar. Phones keep transport + editing and move file/device actions into "More".
    // Phones: Count-in and Monitor live in "More" as tick items to keep the toolbar to two rows.
    std::vector<juce::TextButton*> toolbar{&playButton_, &recordButton_, &clickButton_};
    countInButton_.setVisible(!compact());
    monitorButton_.setVisible(!compact());
    for (auto* b : {&quantizeButton_, &punchButton_, &timeSigButton_, &tempoButton_, &rateButton_, &addTrackButton_})
        b->setVisible(!compact());
    toolbar.push_back(&loopButton_);  // phones too: looping is part of playing
    toolbar.push_back(&mixerButton_);
    toolbar.push_back(&stepsButton_);
    if (!compact()) {
        for (auto* b : {&countInButton_, &monitorButton_, &punchButton_, &quantizeButton_, &timeSigButton_,
                        &tempoButton_, &rateButton_, &addTrackButton_})
            toolbar.push_back(b);
    }
    for (auto* b : {&keysButton_, &undoButton_, &redoButton_}) toolbar.push_back(b);
    for (auto* b : {&newButton_, &openButton_, &saveButton_, &exportButton_, &audioButton_, &scanButton_}) {
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
    // Bottom panel (mixer or step sequencer): docks under the arrangement when there's room,
    // otherwise (phones) takes over the view.
    juce::Component* panel = mixer_.isVisible() ? static_cast<juce::Component*>(&mixer_)
                             : steps_.isVisible() ? static_cast<juce::Component*>(&steps_)
                                                  : nullptr;
    if (panel) {
        if (r.getHeight() >= 620 && !compact()) {
            const int want = panel == &steps_ ? 60 + 26 * 11 : 340;
            panel->setBounds(r.removeFromBottom(std::min(want, r.getHeight() / 2)));
            r.removeFromBottom(8);
        } else {
            panel->setBounds(r);
            r = r.withHeight(0);
        }
    }
    arrange_.setVisible(r.getHeight() > 0);
    arrange_.setBounds(r);
    arrange_.setLayout(layout_);
}

namespace {
bool isMusicalTypingKey(const juce::KeyPress& key) {
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) return false;
    const auto c = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
    return c != 0 && juce::String("awsedftgyhujkolp;").containsChar(c);
}
}  // namespace

bool MainComponent::keyStateChanged(bool isKeyDown) {
    // Note on/off from the computer keyboard, whichever part of the window has focus.
    if (keyboard_.isVisible() && !keyboard_.hasKeyboardFocus(false)) return keyboard_.keyStateChanged(isKeyDown);
    return false;
}

bool MainComponent::keyPressed(const juce::KeyPress& key) {
    if (keyboard_.isVisible() && isMusicalTypingKey(key)) return true;  // handled as a note
    const auto cmd = juce::ModifierKeys::commandModifier;
    if (key == juce::KeyPress('z', cmd, 0)) return session_.undo(), true;
    if (key == juce::KeyPress('z', cmd | juce::ModifierKeys::shiftModifier, 0) ||
        key == juce::KeyPress('y', cmd, 0))
        return session_.redo(), true;
    if (key == juce::KeyPress('s', cmd, 0)) return session_.save(), true;
    if (key == juce::KeyPress('e', cmd, 0)) return ExportPanel::show(session_, theme_), true;
    if (key == juce::KeyPress('s', cmd | juce::ModifierKeys::shiftModifier, 0)) return saveAs(), true;
    if (key == juce::KeyPress('n', cmd, 0)) return newSong(), true;
    if (key == juce::KeyPress('o', cmd, 0)) return openProject(), true;
    if (key == juce::KeyPress('i', cmd, 0)) return importAudio({}), true;
    if (!keyboard_.isVisible() && !key.getModifiers().isAnyModifierKeyDown() &&
        (key.getTextCharacter() == 'i' || key.getTextCharacter() == 'I')) {
        inspectorVisible_ = !inspectorVisible_;
        resized();
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() && (key.getTextCharacter() == 'x' || key.getTextCharacter() == 'X'))
        return toggleMixer(), true;
    if (!key.getModifiers().isAnyModifierKeyDown() && (key.getTextCharacter() == 'b' || key.getTextCharacter() == 'B'))
        return toggleSteps(), true;
    if (!key.getModifiers().isAnyModifierKeyDown() && (key.getTextCharacter() == 'l' || key.getTextCharacter() == 'L')) {
        session_.setLoopEnabled(!session_.loopEnabled());
        refresh();
        return true;
    }
    if (key == juce::KeyPress::spaceKey) return togglePlay(), true;
    // Return/Enter or Home: back to the start (keeps playing if the transport is running).
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::homeKey) {
        if (!session_.isRecording()) session_.seekToTick(0);
        arrange_.repaint();
        return true;
    }
    return arrange_.keyPressed(key);
}

void MainComponent::timerCallback() {
    // Save state: the Save button lights up when there are unsaved changes.
    if (session_.hasProject()) {
        const bool modified = session_.isModified();
        saveButton_.setButtonText(modified ? "Save" : "Saved");
        saveButton_.setToggleState(modified, juce::dontSendNotification);
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) {
            const auto title = juce::String(session_.project().name) + (modified ? " (edited)" : "") + " - Ynot";
            if (w->getName() != title) w->setName(title);
        }
    }
    // A sandboxed plugin crashed: tell the user (once) — playback carries on without it.
    for (const auto& report : host_.takeCrashReports())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Plugin problem",
                                               report + "\n\nUse Plugins > Restart crashed plugins to try again.");
    if (!session_.hasProject()) return;
    auto* dev = session_.device().manager().getCurrentAudioDevice();
    juce::String s;
    s << juce::String(session_.project().name) << "   ";
    s << juce::String(session_.positionSeconds(), 1) << " s   ";
    if (session_.isRecording()) s << "REC " << juce::String(session_.recordingSeconds(), 1) << " s   ";
    if (session_.monitoring() && dev) s << "monitor " << juce::String(session_.monitoringLatencyMs(), 1) << " ms   ";
    s << (dev ? dev->getTypeName() + ": " + dev->getName() : juce::String("no audio device"));
    juce::String armedInstrument;
    for (auto& t : session_.project().tracks)
        if (t.armed && t.kind == ynot::TrackKind::Instrument && armedInstrument.isEmpty()) armedInstrument = t.name;
    if (keyboard_.isVisible() && armedInstrument.isEmpty()) s << "   Arm an instrument track (R) to play";
    else if (armedInstrument.isNotEmpty() && !session_.isRecording())
        s << "   " << armedInstrument << " armed: playing live. Press Record to capture.";
    if (keyboard_.isVisible() && armedInstrument.isNotEmpty())
        s << (keyboard_.hasKeyboardFocus(false) ? "   Type A-K to play notes"
                                                : "   Click the piano to play notes with A-K");
    if (session_.isStretching()) s << "   Stretching clip...";
    const auto virtualOut = session_.device().virtualDeviceWarning();
    if (!virtualOut.empty()) {
        // The most common "I hear nothing": the Mac's default output is a loopback driver.
        status_.setColour(juce::Label::textColourId, theme_.accent);
        s = "Output is \"" + juce::String(virtualOut) + "\", a virtual device, so you may hear nothing. "
            "Click Audio and choose your speakers or headphones.   " + s;
    } else {
        status_.setColour(juce::Label::textColourId, theme_.text);
    }
    status_.setText(s, juce::dontSendNotification);
    playButton_.setButtonText(session_.isPlaying() ? "Stop" : "Play");
}

void MainComponent::refresh() {
    playButton_.setButtonText(session_.isPlaying() ? "Stop" : "Play");
    playButton_.setToggleState(session_.isPlaying(), juce::dontSendNotification);
    inspector_.refresh();
    menuItemsChanged();
    recordButton_.setToggleState(session_.isRecording(), juce::dontSendNotification);
    recordButton_.setButtonText(session_.isRecording() ? "Stop Rec" : "Record");
    undoButton_.setEnabled(session_.canUndo() && !session_.isRecording());
    punchButton_.setToggleState(session_.punchEnabled(), juce::dontSendNotification);
    loopButton_.setToggleState(session_.loopEnabled(), juce::dontSendNotification);
    mixerButton_.setToggleState(mixer_.isVisible(), juce::dontSendNotification);
    stepsButton_.setToggleState(steps_.isVisible(), juce::dontSendNotification);
    mixer_.refresh();
    if (steps_.isVisible()) steps_.refresh();
    if (session_.hasProject())
        timeSigButton_.setButtonText(juce::String(session_.project().timeSigNumerator) + "/" +
                                     juce::String(session_.project().timeSigDenominator));
    rateButton_.setButtonText(JuceAudioDevice::rateLabel(session_.sampleRate()));
    if (session_.hasProject())
        tempoButton_.setButtonText(bpmText(session_.project().tempoAt(session_.positionTick())) + " BPM");
    rateButton_.setEnabled(!session_.isRecording());
    {
        const auto* preset = ynot::findQuantizePreset(session_.recordQuantizeId());
        quantizeButton_.setButtonText(preset ? "Q: " + juce::String(ynot::gridName(preset->settings.gridTicks)) +
                                                   (preset->settings.swingPercent > 50.0
                                                        ? " " + juce::String(static_cast<int>(preset->settings.swingPercent)) + "%"
                                                        : juce::String())
                                             : juce::String("Quantize"));
    }
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

void MainComponent::newSong() {
    auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                   .getChildFile("Ynot").getNonexistentChildFile("Untitled", ".ynot");
    session_.newSong(dir);
}

void MainComponent::saveAs() {
    if (!session_.hasProject()) return;
    chooser_ = std::make_unique<juce::FileChooser>(
        "Save the song as", session_.packageDir().getParentDirectory().getNonexistentChildFile(
                                juce::String(session_.project().name) + " copy", ".ynot"),
        "*.ynot");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              auto dir = fc.getResult();
                              if (dir == juce::File()) return;
                              if (dir.getFileExtension() != ".ynot") dir = dir.withFileExtension(".ynot");
                              const auto err = session_.saveAs(dir);
                              if (err.isNotEmpty())
                                  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                         "Save As", err);
                              refresh();
                          });
}

void MainComponent::openRecent(const juce::File& dir) {
    try {
        session_.open(dir);
    } catch (const std::exception& e) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Can't open project", e.what());
    }
}

juce::StringArray MainComponent::getMenuBarNames() { return {"File", "Edit", "Track", "Transport", "View"}; }

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&) {
    juce::PopupMenu m;
    const bool hasSong = session_.hasProject();
    auto add = [&m](const juce::String& text, const juce::String& keys, bool enabled, bool ticked,
                    std::function<void()> action) {
        juce::PopupMenu::Item item(text);
        item.shortcutKeyDescription = keys;
        item.isEnabled = enabled;
        item.isTicked = ticked;
        item.action = std::move(action);
        m.addItem(std::move(item));
    };
#if JUCE_MAC
    const juce::String cmd = juce::CharPointer_UTF8("\xe2\x8c\x98"), shift = juce::CharPointer_UTF8("\xe2\x87\xa7");
#else
    const juce::String cmd = "Ctrl+", shift = "Shift+";
#endif
    switch (index) {
        case 0: {  // File
            add("New Song", cmd + "N", true, false, [this] { newSong(); });
            add("New Demo Song", {}, true, false, [this] { newDemo(); });
            add("Open...", cmd + "O", true, false, [this] { openProject(); });
            juce::PopupMenu recent;
            for (const auto& path : Session::recentProjects()) {
                const juce::File f(path);
                recent.addItem(f.getFileNameWithoutExtension() + "   (" + f.getParentDirectory().getFullPathName() + ")",
                               [this, f] { openRecent(f); });
            }
            if (recent.getNumItems() == 0) recent.addItem("No recent songs", false, false, [] {});
            m.addSubMenu("Open Recent", recent);
            m.addSeparator();
            add("Save", cmd + "S", hasSong, false, [this] { session_.save(); refresh(); });
            add("Save As...", shift + cmd + "S", hasSong, false, [this] { saveAs(); });
            add("Export...", cmd + "E", hasSong, false, [this] { ExportPanel::show(session_, theme_); });
            m.addSeparator();
            add("Import Audio...", cmd + "I", hasSong, false, [this] { importAudio({}); });
            add("Audio Interface...", {}, true, false, [this] { showAudioSettings(); });
            if (canScanPlugins()) {
                juce::PopupMenu pm;
                addPluginItems(pm);
                m.addSubMenu("Plugins", pm);
            }
            break;
        }
        case 1: {  // Edit
            add("Undo", cmd + "Z", session_.canUndo(), false, [this] { session_.undo(); });
            add("Redo", shift + cmd + "Z", session_.canRedo(), false, [this] { session_.redo(); });
            m.addSeparator();
            const bool sel = arrange_.hasSelection();
            add("Cut", cmd + "X", sel, false, [this] { arrange_.cutSelection(); });
            add("Copy", cmd + "C", sel, false, [this] { arrange_.copySelection(); });
            add("Paste at Playhead", cmd + "V", hasSong, false, [this] { arrange_.paste(); });
            add("Duplicate", cmd + "D", sel, false, [this] { arrange_.duplicateSelection(); });
            add("Delete", "Delete", sel, false, [this] { arrange_.deleteSelected(); });
            m.addSeparator();
            add("Select All", cmd + "A", hasSong, false, [this] { arrange_.selectAll(); });
            add("Split at Playhead", "S", sel, false, [this] { arrange_.splitSelectedAtPlayhead(); });
            break;
        }
        case 2: {  // Track
            addTrackItems(m);
            m.addSeparator();
            const auto focus = arrange_.focusedTrackId();
            add("Instrument & Effects for the Selected Track...", {}, !focus.empty(), false,
                [this, focus] { arrange_.showTrackMenu(focus, &inspector_); });
            juce::PopupMenu q;
            addQuantizeItems(q);
            m.addSubMenu("Quantize", q);
            break;
        }
        case 3: {  // Transport
            add(session_.isPlaying() ? "Stop" : "Play", "Space", hasSong, false, [this] { togglePlay(); });
            add(session_.isRecording() ? "Stop Recording" : "Record", {}, hasSong, false, [this] { toggleRecording(); });
            add("Go to Start", "Return", hasSong, false, [this] { session_.seekToTick(0); arrange_.repaint(); });
            m.addSeparator();
            add("Loop", "L", hasSong, session_.loopEnabled(), [this] { session_.setLoopEnabled(!session_.loopEnabled()); refresh(); });
            add("Click", {}, true, session_.metronome(), [this] {
                session_.setMetronome(!session_.metronome());
                clickButton_.setToggleState(session_.metronome(), juce::dontSendNotification);
            });
            add("Count-in (1 bar)", {}, true, session_.countInBars() > 0, [this] {
                session_.setCountInBars(session_.countInBars() > 0 ? 0 : 1);
                countInButton_.setToggleState(session_.countInBars() > 0, juce::dontSendNotification);
            });
            juce::PopupMenu punch, tempo, ts, rate;
            addPunchItems(punch);
            addTempoItems(tempo);
            addTimeSignatureItems(ts);
            addSampleRateItems(rate);
            m.addSubMenu("Punch Recording", punch);
            m.addSubMenu("Tempo", tempo, hasSong);
            m.addSubMenu("Time Signature", ts, hasSong);
            m.addSubMenu("Sample Rate", rate);
            break;
        }
        case 4: {  // View
            add("Mixer", "X", true, mixer_.isVisible(), [this] { toggleMixer(); });
            add("Step Sequencer", "B", true, steps_.isVisible(), [this] { toggleSteps(); });
            add("On-screen Keyboard", {}, true, keyboard_.isVisible(), [this] { showKeyboard(!keyboard_.isVisible()); });
            add("Inspector", "I", true, inspectorVisible_, [this] {
                inspectorVisible_ = !inspectorVisible_;
                resized();
            });
            add("Device Compatibility Panel", {}, true, compatPanelVisible_, [this] {
                compatPanelVisible_ = !compatPanelVisible_;
                resized();
            });
            break;
        }
        default: break;
    }
    return m;
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
    m.addItem("Save As...", [this] { saveAs(); });
    m.addItem("Export song...", [this] { ExportPanel::show(session_, theme_); });
    m.addSeparator();
    m.addItem("Count-in (1 bar)", true, session_.countInBars() > 0, [this] {
        session_.setCountInBars(session_.countInBars() > 0 ? 0 : 1);
        countInButton_.setToggleState(session_.countInBars() > 0, juce::dontSendNotification);
    });
    m.addItem("Monitor input", true, session_.monitoring(), [this] {
        session_.setMonitoring(!session_.monitoring());
        monitorButton_.setToggleState(session_.monitoring(), juce::dontSendNotification);
    });
    {
        juce::PopupMenu tracks;
        addTrackItems(tracks);
        m.addSubMenu("Add track", tracks);
    }
    {
        juce::PopupMenu q, punch, ts;
        addQuantizeItems(q);
        addPunchItems(punch);
        addTimeSignatureItems(ts);
        m.addSubMenu("Quantize", q);
        m.addSubMenu("Punch recording", punch);
        m.addSubMenu("Time signature", ts);
        juce::PopupMenu tempo;
        addTempoItems(tempo);
        m.addSubMenu("Tempo", tempo, session_.hasProject());
        juce::PopupMenu rate;
        addSampleRateItems(rate);
        m.addSubMenu("Sample rate (" + JuceAudioDevice::rateLabel(session_.sampleRate()) + ")", rate,
                     !session_.isRecording());
    }
    m.addSeparator();
    m.addItem("Split clip at playhead", arrange_.hasSelection(), false, [this] { arrange_.splitSelectedAtPlayhead(); });
    m.addItem("Delete clip", arrange_.hasSelection(), false, [this] { arrange_.deleteSelected(); });
    m.addSeparator();
    m.addItem("Prepare for iPhone & Android", !session_.tracksToPrepareForMobile().empty(), false,
              [this] { prepareForMobile(); });
    m.addItem("Audio interface...", [this] { showAudioSettings(); });
    if (canScanPlugins()) {
        juce::PopupMenu pm;
        addPluginItems(pm);
        m.addSubMenu("Plugins", pm);
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(moreButton_));
}

void MainComponent::importAudio(const std::string& trackId) {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Import audio (WAV, MP3, M4A/MP4, FLAC, Ogg, AIFF, WMA/WMV on Windows)",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory), importFilePatterns());
    const int chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                             juce::FileBrowserComponent::canSelectMultipleItems;
    chooser_->launchAsync(chooserFlags, [this, trackId](const juce::FileChooser& fc) {
        const auto files = fc.getResults();
        for (const auto& f : files) importFiles({f}, trackId, session_.positionTick());
    });
}

void MainComponent::importFiles(const juce::Array<juce::File>& files, const std::string& trackId, int64_t startTick) {
    juce::Component::SafePointer<MainComponent> safe(this);
    for (const auto& f : files) {
        status_.setText("Importing " + f.getFileName() + "...", juce::dontSendNotification);
        session_.importAudioFile(f, trackId, startTick, [safe, name = f.getFileName()](const juce::String& error) {
            if (!safe) return;
            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Couldn't import " + name, error);
        });
    }
}

void MainComponent::addTrackItems(juce::PopupMenu& m) {
    m.addItem("Import audio file... (WAV, MP3, M4A/MP4, FLAC, WMA/WMV...)", session_.hasProject(), false,
              [this] { importAudio({}); });
    m.addSeparator();
    m.addItem("Audio track (record a mic or instrument)", session_.hasProject(), false, [this] {
        session_.edit([](ynot::Project& p) { ynot::addTrack(p, ynot::TrackKind::Audio); });
    });
    m.addItem("Instrument track (MIDI, starts with Ynot Synth)", session_.hasProject(), false, [this] {
        session_.edit([](ynot::Project& p) { ynot::addTrack(p, ynot::TrackKind::Instrument); });
    });
    m.addItem("Drum track (Ynot Drums + step sequencer)", session_.hasProject(), false, [this] {
        const int64_t bar = session_.positionTick() / session_.project().barTicks() * session_.project().barTicks();
        std::string id;
        session_.edit([&](ynot::Project& p) { id = ynot::addDrumTrack(p, bar, 1); });
        if (const auto* t = session_.project().findTrack(id); t && !t->midiClips.empty()) openPattern(id, t->midiClips[0].id);
    });
}

void MainComponent::addPluginItems(juce::PopupMenu& m) {
    m.addItem(scanning_ ? "Scanning..." : "Scan for plugins", !scanning_, false, [this] { scanPlugins(); });
    m.addItem("Run plugins safely (separate processes)", true, host_.sandboxEnabled(), [this] {
        host_.setSandboxEnabled(!host_.sandboxEnabled());
        session_.reload();  // reload plugins in the chosen mode
    });
    m.addItem("Restart crashed plugins", [this] { session_.reload(); });
}

void MainComponent::addQuantizeItems(juce::PopupMenu& m) {
    m.addSubMenu("While recording MIDI", quantizePresetMenu(session_.recordQuantizeId(), [this](const std::string& id) {
        session_.setRecordQuantize(id);
        refresh();
    }, true));
    const bool midiSelected = arrange_.hasSelection() && arrange_.selectedIsMidi();
    m.addSubMenu("Quantize selected clip", quantizePresetMenu({}, [this](const std::string& id) {
        session_.quantizeClip(arrange_.selectedTrackId(), arrange_.selectedClipId(), id);
    }, false), midiSelected);
    m.addSubMenu("Strength", strengthMenu(session_.quantizeStrength(), [this](double s) {
        session_.setQuantizeStrength(s);
    }));
}

void MainComponent::addPunchItems(juce::PopupMenu& m) {
    m.addItem("Punch recording", true, session_.punchEnabled(), [this] {
        session_.setPunchEnabled(!session_.punchEnabled());
        refresh();
    });
    m.addSeparator();
    m.addItem("Set punch in at playhead", [this] {
        session_.setPunchRange(session_.positionTick(), std::max(session_.punchOut(), session_.positionTick() + 1));
        refresh();
    });
    m.addItem("Set punch out at playhead", [this] {
        session_.setPunchRange(std::min(session_.punchIn(), session_.positionTick()), session_.positionTick());
        refresh();
    });
    m.addItem("Punch range = selected clip", arrange_.hasSelection(), false, [this] {
        if (auto span = ynot::findClip(session_.project(), arrange_.selectedTrackId(), arrange_.selectedClipId())) {
            session_.setPunchRange(span->startTick, span->startTick + span->lengthTick);
            session_.setPunchEnabled(true);
            refresh();
        }
    });
    m.addSeparator();
    m.addItem("Tip: Shift+drag in the ruler to set the range", false, false, [] {});
}

void MainComponent::addTimeSignatureItems(juce::PopupMenu& m) {
    if (!session_.hasProject()) return;
    m = timeSignatureMenu(session_.project().timeSigNumerator, session_.project().timeSigDenominator,
                          [this](int n, int d) {
                              session_.setTimeSignature(n, d);
                              refresh();
                          });
}

void MainComponent::toggleMixer() {
    steps_.setVisible(false);
    mixer_.setVisible(!mixer_.isVisible());
    resized();
    refresh();
}

void MainComponent::toggleSteps() {
    mixer_.setVisible(false);
    steps_.setVisible(!steps_.isVisible());
    if (!steps_.isVisible()) session_.setAuditionTrack({});
    resized();
    refresh();
}

void MainComponent::openPattern(const std::string& trackId, const std::string& clipId) {
    mixer_.setVisible(false);
    steps_.open(trackId, clipId);
    steps_.setVisible(true);
    resized();
    refresh();
}

void MainComponent::addLoopItems(juce::PopupMenu& m) {
    if (!session_.hasProject()) return;
    const bool rec = session_.isRecording();
    m.addItem("Loop  (L)", !rec, session_.loopEnabled(), [this] {
        session_.setLoopEnabled(!session_.loopEnabled());
        refresh();
    });
    m.addSeparator();
    const auto& p = session_.project();
    const int64_t bar = session_.positionTick() / p.barTicks() * p.barTicks();
    for (int bars : {1, 2, 4, 8})
        m.addItem("Loop " + juce::String(bars) + (bars == 1 ? " bar" : " bars") + " from the playhead's bar", !rec, false,
                  [this, bar, bars] {
                      session_.setLoopRange(bar, bar + bars * session_.project().barTicks());
                      session_.setLoopEnabled(true);
                      refresh();
                  });
    m.addItem("Loop the selected clip", !rec && arrange_.hasSelection(), false, [this] {
        if (auto span = ynot::findClip(session_.project(), arrange_.selectedTrackId(), arrange_.selectedClipId())) {
            session_.setLoopRange(span->startTick, span->startTick + span->lengthTick);
            session_.setLoopEnabled(true);
            refresh();
        }
    });
    m.addItem("Loop the punch range", !rec, false, [this] {
        session_.setLoopRange(session_.punchIn(), session_.punchOut());
        session_.setLoopEnabled(true);
        refresh();
    });
    m.addSeparator();
    m.addItem("Tip: Option/Alt-drag in the ruler to draw the loop", false, false, [] {});
    m.addItem("Recording with the loop on records a take on every pass", false, false, [] {});
}

void MainComponent::askNumber(const juce::String& title, const juce::String& message, double initial, double lo,
                              double hi, std::function<void(double)> onOk) {
    auto* w = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("value", bpmText(initial));
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, title, lo, hi, onOk](int result) {
        if (result != 1 || !safe) return;
        const auto text = w->getTextEditorContents("value").retainCharacters("0123456789.");
        const double v = text.getDoubleValue();
        if (text.isEmpty() || v < lo || v > hi) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title,
                                                   "Enter a number from " + bpmText(lo) + " to " + bpmText(hi) + ".");
            return;
        }
        onOk(v);
        safe->focusSoon();
    }), true);
}

void MainComponent::addTempoItems(juce::PopupMenu& m) {
    if (!session_.hasProject()) return;
    const auto& p = session_.project();
    auto where = [&p](int64_t tick) {
        const int64_t bar = tick / p.barTicks() + 1, beat = (tick % p.barTicks()) / p.beatTicks() + 1;
        return "bar " + juce::String(bar) + (beat > 1 ? " beat " + juce::String(beat) : juce::String());
    };
    m.addItem("Song tempo: " + bpmText(p.tempoBpm) + " BPM...", [this] {
        askNumber("Song tempo", "Tempo at the start of the song, in BPM (20 to 400).", session_.project().tempoBpm,
                  ynot::kMinBpm, ynot::kMaxBpm, [this](double bpm) {
                      session_.edit([bpm](ynot::Project& pr) { ynot::setTempo(pr, bpm); });
                  });
    });
    // A change lands on the playhead's beat.
    const int64_t at = ynot::snapTick(session_.positionTick(), p.beatTicks());
    m.addItem("Change tempo at " + where(at) + " (playhead)...", at > 0, false, [this, at] {
        askNumber("Tempo change", "New tempo from this point on, in BPM (20 to 400).",
                  session_.project().tempoAt(at), ynot::kMinBpm, ynot::kMaxBpm, [this, at](double bpm) {
                      session_.edit([at, bpm](ynot::Project& pr) { ynot::setTempoChange(pr, at, bpm); });
                  });
    });
    if (!p.tempoChanges.empty()) {
        m.addSeparator();
        m.addSectionHeader("Tempo changes");
        for (const auto& c : p.tempoChanges) {
            juce::PopupMenu sub;
            const int64_t tick = c.tick;
            sub.addItem("Go there", [this, tick] { session_.seekToTick(tick); refresh(); });
            sub.addItem("Change...", [this, tick, bpm = c.bpm] {
                askNumber("Tempo change", "Tempo from this point on, in BPM (20 to 400).", bpm, ynot::kMinBpm,
                          ynot::kMaxBpm, [this, tick](double v) {
                              session_.edit([tick, v](ynot::Project& pr) { ynot::setTempoChange(pr, tick, v); });
                          });
            });
            sub.addItem("Remove", [this, tick] {
                session_.edit([tick](ynot::Project& pr) { ynot::removeTempoChange(pr, tick); });
            });
            m.addSubMenu(where(c.tick).replaceSection(0, 1, "B") + ": " + bpmText(c.bpm) + " BPM", sub);
        }
    }
    m.addSeparator();
    m.addItem("Audio keeps its length when the tempo changes; use Time stretch to fit it.", false, false, [] {});
}

void MainComponent::addSampleRateItems(juce::PopupMenu& m) {
    const auto available = session_.availableSampleRates();
    const double current = session_.sampleRate();
    auto has = [&](double r) { return std::find(available.begin(), available.end(), r) != available.end(); };
    auto addRate = [&](double r) {
        const bool ok = has(r);
        m.addItem(JuceAudioDevice::rateLabel(r) + (ok ? juce::String() : "  (not on this device)"), ok,
                  std::abs(r - current) < 0.5, [this, r] {
                      const auto err = session_.setSampleRate(r);
                      if (err.isNotEmpty())
                          juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                 "Sample rate", err);
                      refresh();
                  });
    };
    for (double r : Session::standardSampleRates()) addRate(r);
    for (double r : available)  // anything else the interface offers (e.g. 32 or 176.4 kHz)
        if (r >= 32000 && std::find(Session::standardSampleRates().begin(), Session::standardSampleRates().end(), r) ==
                              Session::standardSampleRates().end())
            addRate(r);
    m.addSeparator();
    m.addItem("Saved with the project. Audio at other rates is converted on playback.", false, false, [] {});
    juce::PopupMenu buffers;
    for (int n : session_.availableBufferSizes())
        buffers.addItem(AudioIOPanel::bufferLabel(n, session_.sampleRate()), !session_.isRecording(),
                        n == session_.bufferSize(), [this, n] {
                            const auto err = session_.setBufferSize(n);
                            if (err.isNotEmpty())
                                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                       "Buffer size", err);
                            refresh();
                        });
    m.addSubMenu("Buffer size (" + juce::String(session_.bufferSize()) + ")", buffers);
    m.addItem("Audio interface...", [this] { showAudioSettings(); });
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

void MainComponent::showAudioSettings() { AudioIOPanel::show(session_, theme_); }

void MainComponent::scanPlugins() {
    if (scanning_) return;
    scanning_ = true;
    scanButton_.setEnabled(false);
    scanButton_.setButtonText("Scanning...");
    // Each plugin is inspected in a helper process (see JucePluginHost::scanOutOfProcess), on a
    // background thread so the window stays responsive.
    juce::Component::SafePointer<MainComponent> safe(this);
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::Thread::launch([safe, exe, host = &host_] {
        auto report = host->scanOutOfProcess(exe, [safe](int done, int total, const juce::String& name) {
            juce::MessageManager::callAsync([safe, done, total, name] {
                if (safe) safe->scanButton_.setButtonText("Scanning " + juce::String(done + 1 > total ? total : done + 1) +
                                                          "/" + juce::String(total));
                juce::ignoreUnused(name);
            });
        });
        juce::MessageManager::callAsync([safe, report] {
            if (!safe) return;
            safe->scanning_ = false;
            safe->host_.saveCache(pluginCacheFile());
            safe->scanButton_.setButtonText("Plugins");
            safe->scanButton_.setEnabled(true);
            safe->session_.reload();  // newly found plugins become live (not an undoable edit)
            juce::String msg;
            msg << "Found " << report.found << " new plugin" << (report.found == 1 ? "" : "s");
            if (report.alreadyKnown > 0) msg << " (" << report.alreadyKnown << " already known)";
            msg << ".";
            if (!report.failed.empty()) {
                msg << "\n\n" << static_cast<int>(report.failed.size())
                    << " couldn't be scanned and will be skipped (Ynot kept running):\n";
                for (size_t i = 0; i < report.failed.size() && i < 12; ++i) msg << "  - " << report.failed[i] << "\n";
                if (report.failed.size() > 12) msg << "  ...and " << static_cast<int>(report.failed.size() - 12) << " more\n";
            }
            juce::AlertWindow::showMessageBoxAsync(report.failed.empty() ? juce::MessageBoxIconType::InfoIcon
                                                                          : juce::MessageBoxIconType::WarningIcon,
                                                   "Plugin scan", msg);
        });
    });
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
