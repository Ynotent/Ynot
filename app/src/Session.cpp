#include "Session.h"

#include <thread>

#include "AudioImporter.h"

namespace ynotapp {

Session::Session(JucePluginHost& host) : host_(host) {
    device_.setStateFile(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                             .getChildFile("Ynot").getChildFile("audio-device.xml"));
    midiBuffer_.ensureSize(8192);
    liveMidi_.resize(kMaxLiveMidi);
    chunkMidi_.resize(kMaxLiveMidi);
    collector_.reset(sampleRate_);
    // Fixed size so the audio thread never sees it reallocate when the device changes.
    scratchL_.assign(kScratchFrames, 0.f);
    scratchR_.assign(kScratchFrames, 0.f);
    keyboardState_.addListener(&collector_);
    midiControls_ = std::make_unique<MidiControlMap>(
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Ynot").getChildFile("midi-controls.json"));
    midiControls_->onControl = [this](const MidiMapping& m, int v) { applyMidiControl(m, v); };
    midiControls_->onTransport = [this](TransportCommand c) { if (onTransportCommand) onTransportCommand(c); };
    midiControls_->onLearned = [this] { if (onChange) onChange(); };
    if (midiControls_->clockOut()) midiClock_.setEnabled(true);
    device_.onDeviceChanged = [this] { deviceChanged(); };
    if (auto json = juce::JSON::parse(ioSettingsFile()); json.isObject())
        mainOutputPair_ = std::max(0, static_cast<int>(json.getProperty("mainOutputPair", 0)));  // on-screen keyboard -> same path as hardware MIDI
}

Session::~Session() {
    alive_->store(false);
    if (recording_) stopRecording();
    keyboardState_.removeListener(&collector_);
    device_.manager().removeMidiInputDeviceCallback({}, this);
    stopAudio();
    delete active_.exchange(nullptr);
}

void Session::newDemo(const juce::File& dir) {
    shownChords_.clear();
    dir.createDirectory();
    project_ = ynot::makeDemoProject(dir.getFullPathName().toStdString());
    history_.clear();
    dir_ = dir;
    loaded_ = true;
    project_.sampleRateHint = sampleRate_;  // a new project starts at the device's current rate
    ynot::saveProjectPackage(project_, dir_.getFullPathName().toStdString());
    modified_ = false;
    rememberRecent(dir_);
    media_ = std::make_unique<ynot::PackageMediaProvider>(dir_.getFullPathName().toStdString());
    rebuild();
}

void Session::newSong(const juce::File& dir) {
    shownChords_.clear();
    dir.createDirectory();
    ynot::Project p;
    p.id = ynot::newId();
    p.name = dir.getFileNameWithoutExtension().toStdString();
    p.tempoBpm = 120;
    p.sampleRateHint = sampleRate_;
    ynot::addTrack(p, ynot::TrackKind::Audio, "Audio 1");
    ynot::addTrack(p, ynot::TrackKind::Instrument, "Instrument 1");
    project_ = std::move(p);
    history_.clear();
    dir_ = dir;
    loaded_ = true;
    ynot::saveProjectPackage(project_, dir_.getFullPathName().toStdString());
    media_ = std::make_unique<ynot::PackageMediaProvider>(dir_.getFullPathName().toStdString());
    modified_ = false;
    rememberRecent(dir_);
    rebuild();
}

juce::String Session::saveAs(const juce::File& newDir) {
    if (!loaded_) return "Nothing to save yet.";
    if (newDir == dir_) {
        save();
        return {};
    }
    if (newDir.exists() && !(newDir.isDirectory() && newDir.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0))
        return "\"" + newDir.getFileName() + "\" already exists. Choose another name.";
    save();
    if (!dir_.copyDirectoryTo(newDir)) return "Couldn't copy the project to \"" + newDir.getFullPathName() + "\".";
    dir_ = newDir;
    project_.name = newDir.getFileNameWithoutExtension().toStdString();
    media_ = std::make_unique<ynot::PackageMediaProvider>(dir_.getFullPathName().toStdString());
    save();
    rememberRecent(dir_);
    rebuild();
    return {};
}

static juce::File recentFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Ynot")
        .getChildFile("recent-projects.txt");
}

juce::StringArray Session::recentProjects() {
    juce::StringArray list;
    list.addLines(recentFile().loadFileAsString());
    list.removeEmptyStrings();
    juce::StringArray existing;
    for (const auto& path : list)
        if (juce::File(path).isDirectory()) existing.add(path);
    return existing;
}

void Session::rememberRecent(const juce::File& dir) {
    auto list = recentProjects();
    list.removeString(dir.getFullPathName());
    list.insert(0, dir.getFullPathName());
    while (list.size() > 10) list.remove(list.size() - 1);
    recentFile().getParentDirectory().createDirectory();
    recentFile().replaceWithText(list.joinIntoString("\n"));
}

void Session::open(const juce::File& dir) {
    shownChords_.clear();
    project_ = ynot::loadProjectPackage(dir.getFullPathName().toStdString());
    modified_ = false;
    rememberRecent(dir);
    history_.clear();
    dir_ = dir;
    loaded_ = true;
    media_ = std::make_unique<ynot::PackageMediaProvider>(dir_.getFullPathName().toStdString());
    rebuild();
    applyProjectRate();
}

void Session::save() {
    if (!loaded_) return;
    host_.captureState(project_);
    ynot::saveProjectPackage(project_, dir_.getFullPathName().toStdString());
    modified_ = false;
}

void Session::edit(const std::function<void(ynot::Project&)>& fn, bool undoStep) {
    if (!loaded_) return;
    host_.captureState(project_);  // keep plugin tweaks made in their editors
    if (undoStep) history_.record(project_);
    modified_ = true;
    fn(project_);
    rebuild();
}

bool Session::undo() {
    if (!loaded_ || recording_ || !history_.canUndo()) return false;
    host_.captureState(project_);
    history_.undo(project_);
    modified_ = true;
    rebuild();
    return true;
}

bool Session::redo() {
    if (!loaded_ || recording_ || !history_.canRedo()) return false;
    host_.captureState(project_);
    history_.redo(project_);
    modified_ = true;
    rebuild();
    return true;
}

void Session::setAuditionTrack(const std::string& trackId) {
    if (auditionTrack_ == trackId) return;
    auditionTrack_ = trackId;
    rebuild();
}

void Session::auditionNote(int note, int velocity) {
    // Same path as a MIDI keyboard: lock-free into the audio thread.
    collector_.addMessageToQueue(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(std::clamp(velocity, 1, 127)))
                                     .withTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001));
    collector_.addMessageToQueue(juce::MidiMessage::noteOff(1, note)
                                     .withTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001 + 0.15));
}

void Session::pushMixToEngine() {
    auto* e = active_.load();
    if (!e) return;
    bool anySolo = false;
    for (const auto& t : project_.tracks) anySolo |= t.solo;
    for (const auto& t : project_.tracks)
        e->setTrackMix(e->trackIndex(t.id), t.gainDb, t.pan, t.mute || (anySolo && !t.solo));
    e->setMasterGain(project_.masterGainDb);
}

void Session::beginMixChange() {
    if (loaded_) history_.record(project_);
    modified_ = true;
}

void Session::setTrackGain(const std::string& id, double gainDb) {
    if (auto* t = project_.findTrack(id)) t->gainDb = std::clamp(gainDb, -96.0, 12.0);
    pushMixToEngine();
}

void Session::setTrackPan(const std::string& id, double pan) {
    if (auto* t = project_.findTrack(id)) t->pan = std::clamp(pan, -1.0, 1.0);
    pushMixToEngine();
}

void Session::setTrackMute(const std::string& id, bool on) {
    beginMixChange();
    if (auto* t = project_.findTrack(id)) t->mute = on;
    pushMixToEngine();
    if (onChange) onChange();
}

void Session::setTrackSolo(const std::string& id, bool on) {
    beginMixChange();
    if (auto* t = project_.findTrack(id)) t->solo = on;
    pushMixToEngine();
    if (onChange) onChange();
}

void Session::setMasterGain(double gainDb) {
    project_.masterGainDb = std::clamp(gainDb, -96.0, 12.0);
    pushMixToEngine();
}

Session::Meter Session::takeMeter(const std::string& trackId) {
    auto* e = active_.load();
    if (!e) return {};
    const int i = trackId.empty() ? -1 : e->trackIndex(trackId);
    if (!trackId.empty() && i < 0) return {};
    return {e->takePeak(i, 0), e->takePeak(i, 1)};
}

void Session::applyLoopToEngine(ynot::Engine& e) const {
    // Punch recording records a fixed range, so the loop pauses while it runs.
    const bool on = loopEnabled() && !(recording_ && punchActive_);
    e.setLoop(project_.loop.startTick, project_.loop.endTick, on);
}

void Session::setLoopRange(int64_t startTick, int64_t endTick) {
    if (recording_) return;
    modified_ = true;
    if (endTick < startTick) std::swap(startTick, endTick);
    project_.loop.startTick = std::max<int64_t>(0, startTick);
    project_.loop.endTick = std::max(project_.loop.startTick + ynot::kTicksPerQuarter, endTick);  // at least a beat
    if (auto* e = active_.load()) applyLoopToEngine(*e);
    if (onChange) onChange();
}

void Session::setLoopEnabled(bool on) {
    if (recording_) return;
    modified_ = true;
    if (on && !project_.loop.valid()) {  // first use: the 4 bars from the playhead's bar
        const int64_t bar = positionTick() / project_.barTicks() * project_.barTicks();
        project_.loop.startTick = bar;
        project_.loop.endTick = bar + 4 * project_.barTicks();
    }
    project_.loop.enabled = on;
    if (auto* e = active_.load()) applyLoopToEngine(*e);
    if (onChange) onChange();
}

bool Session::selectTake(const std::string& trackId, const std::string& clipId, int index) {
    bool ok = false;
    edit([&](ynot::Project& p) { ok = ynot::selectTake(p, trackId, clipId, index); });
    return ok;
}

void Session::seekToTick(int64_t tick) {
    const int64_t frame = ynot::tickToFrame(project_, std::max<int64_t>(0, tick), sampleRate_);
    position_ = frame;
    pendingSeek_ = frame;  // applied by the audio thread at the start of its next block
}

int64_t Session::positionTick() const {
    return ynot::frameToTick(project_, position_.load(), sampleRate_);
}

const ynot::AudioBuffer* Session::mediaAudio(const std::string& mediaId) {
    if (!loaded_ || !media_) return nullptr;
    const auto* m = project_.findMedia(mediaId);
    return m ? media_->get(*m) : nullptr;
}

void Session::freeze(const std::string& trackId) {
    if (!loaded_) return;
    host_.captureState(project_);
    history_.record(project_);
    ynot::freezeTrack(project_, dir_.getFullPathName().toStdString(), trackId, *media_, host_,
                      ynot::currentPlatform(), sampleRate_);
    save();
    rebuild();
}

void Session::unfreeze(const std::string& trackId) {
    edit([&](ynot::Project& p) { ynot::unfreezeTrack(p, trackId); });
}

void Session::rebuild() {
    if (!loaded_) return;
    auto* fresh = new ynot::Engine();
    ynot::EngineOptions opts;
    opts.sampleRate = sampleRate_;
    opts.maxBlock = blockSize_;
    if (monitoring_.load())
        for (const auto* t : project_.orderedTracks())
            if (t->armed && t->kind == ynot::TrackKind::Audio) opts.monitorInputs[t->id] = inputFor(t->id);
    bool liveMidi = false;
    for (const auto* t : project_.orderedTracks())
        if (t->kind == ynot::TrackKind::Instrument && (t->armed || t->id == auditionTrack_)) {
            opts.liveMidiTracks.insert(t->id);  // armed instruments play from MIDI input
            liveMidi = true;
        }
    liveMidiActive_ = liveMidi;
    fresh->prepare(project_, *media_, host_, opts);
    pendingSeek_ = kNoSeek;  // position_ already holds the latest seek; a stale one must not override it
    fresh->seek(position_.load());
    fresh->setMetronome(metronomeOn_.load());
    fresh->setForcedClickUntil(forcedClickUntil_);
    applyLoopToEngine(*fresh);
    issues_ = fresh->issues();

    ynot::Engine* old = active_.exchange(fresh);
    // Wait for any callback that may still be using the old engine to finish.
    while (inCallback_.load()) std::this_thread::yield();
    delete old;
    if (onChange) onChange();
}

bool Session::startAudio(const std::string& deviceId) {
    ynot::AudioStreamConfig cfg;
    cfg.sampleRate = 48000;
    cfg.bufferSize = 256;
    cfg.inputChannels = {0, 1};  // first stereo pair; more inputs can be enabled in Audio settings
#if JUCE_ANDROID
    if (!juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio)) {
        // Ask once; reopen with inputs when the user answers.
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, [this, deviceId](bool granted) {
            if (granted) startAudio(deviceId);
        });
        cfg.inputChannels.clear();
    }
#endif
    bool ok = device_.open(deviceId, cfg, [this](const float* const* ins, int numIns, float* const* outs, int numOuts, int n) {
        audioCallback(ins, numIns, outs, numOuts, n);
    });
    if (!ok) return false;
    // Use every input and output of an audio interface, not just the first pair.
    if (!cfg.inputChannels.empty()) device_.enableAllChannels();
    sampleRate_ = device_.currentSampleRate();
    blockSize_ = std::max(device_.currentBufferSize(), 64);
    collector_.reset(sampleRate_);
    // Listen to every MIDI input (USB/Bluetooth controllers, virtual ports).
    for (auto& d : juce::MidiInput::getAvailableDevices())
        device_.manager().setMidiInputDeviceEnabled(d.identifier, true);
    device_.manager().removeMidiInputDeviceCallback({}, this);
    device_.manager().addMidiInputDeviceCallback({}, this);
    rebuild();  // engine must match the device's actual rate and block size
    device_.start();
    applyProjectRate();
    return true;
}

const std::vector<double>& Session::standardSampleRates() {
    static const std::vector<double> rates{44100, 48000, 88200, 96000, 192000};
    return rates;
}

std::vector<double> Session::availableSampleRates() const {
    if (device_.manager().getCurrentAudioDevice() != nullptr) return device_.availableSampleRates();
    return standardSampleRates();  // no hardware (tests, simulation): any standard rate
}

juce::String Session::setSampleRate(double rate) {
    const auto rates = availableSampleRates();
    if (std::find(rates.begin(), rates.end(), rate) == rates.end())
        return JuceAudioDevice::rateLabel(rate) + " isn't available on this audio device.";
    if (recording_) return "Stop recording before changing the sample rate.";
    if (device_.manager().getCurrentAudioDevice() != nullptr) {
        auto err = device_.setSampleRate(rate);
        if (err.isNotEmpty()) return err;
        deviceChanged();  // apply now; the later change notification is then a no-op
    } else {
        applyDeviceFormat(rate, blockSize_);
    }
    if (loaded_ && std::abs(project_.sampleRateHint - sampleRate_) > 0.5) {
        project_.sampleRateHint = sampleRate_;
        save();
    }
    return {};
}

void Session::applyProjectRate() {
    if (!loaded_ || device_.manager().getCurrentAudioDevice() == nullptr) return;
    const double want = project_.sampleRateHint;
    if (std::abs(want - sampleRate_) < 0.5) return;
    auto rates = device_.availableSampleRates();
    if (std::find(rates.begin(), rates.end(), want) == rates.end()) return;  // play at the device's rate instead
    if (device_.setSampleRate(want).isEmpty()) deviceChanged();
}

void Session::deviceChanged() {
    if (auto* d = device_.manager().getCurrentAudioDevice())
        applyDeviceFormat(d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
    else if (onChange) onChange();
}

void Session::applyDeviceFormat(double rate, int block) {
    block = std::clamp(block, 64, kScratchFrames);
    if (rate <= 0) return;
    if (std::abs(rate - sampleRate_) < 0.5 && block == blockSize_) {
        if (onChange) onChange();  // e.g. channels changed: refresh input labels
        return;
    }
    // A take can't continue across a rate change (its frames would play back at the wrong speed).
    if (recording_) stopRecording();
    const int64_t tick = positionTick();  // keep the playhead at the same musical position
    sampleRate_ = rate;
    blockSize_ = block;
    collector_.reset(sampleRate_);
    if (loaded_) rebuild();
    seekToTick(tick);
    if (onChange) onChange();
}

void Session::stopAudio() {
    if (recording_) stopRecording();
    playing_ = false;
    device_.close();
}

int Session::numInputChannels() const {
    if (auto* d = device_.manager().getCurrentAudioDevice())
        return d->getActiveInputChannels().countNumberOfSetBits();
    return simulatedInputs_;
}

std::vector<int> Session::inputFor(const std::string& trackId) const {
    auto it = inputs_.find(trackId);
    if (it != inputs_.end()) return it->second;
    // Default: armed tracks take mono inputs in order (1, 2, ...).
    int index = 0;
    for (const auto* t : project_.orderedTracks()) {
        if (t->id == trackId) break;
        if (t->armed && t->kind == ynot::TrackKind::Audio) ++index;
    }
    const int n = std::max(1, numInputChannels());
    return {index % n};
}

void Session::cycleInput(const std::string& trackId) {
    // Order: In 1, In 2, ..., then stereo pairs 1+2, 3+4, ...
    const int n = std::max(1, numInputChannels());
    std::vector<std::vector<int>> choices;
    for (int c = 0; c < n; ++c) choices.push_back({c});
    for (int c = 0; c + 1 < n; c += 2) choices.push_back({c, c + 1});
    auto current = inputFor(trackId);
    size_t i = 0;
    while (i < choices.size() && choices[i] != current) ++i;
    inputs_[trackId] = choices[(i + 1) % choices.size()];
    if (monitoring_.load()) rebuild();  // rebuild() notifies the UI
    else if (onChange) onChange();
}

std::string Session::inputChannelName(int channel) const {
    const auto names = device_.inputChannelNames();
    if (channel >= 0 && channel < names.size()) return names[channel].toStdString();
    return {};
}

std::string Session::inputLabel(const std::string& trackId) const {
    auto ch = inputFor(trackId);
    std::string s = "In " + std::to_string(ch[0] + 1);
    if (ch.size() > 1) s += "+" + std::to_string(ch[1] + 1);
    return s;
}

std::vector<Session::InputChoice> Session::inputChoices() const {
    std::vector<InputChoice> out;
    const int n = std::max(1, numInputChannels());
    auto named = [this](int c) {
        const juce::String name(inputChannelName(c));
        // Drivers often name channels just "Input 3"; only show names that add something.
        const bool generic = name.isEmpty() || (name.containsIgnoreCase("input") && name.length() <= 9);
        return generic ? std::string() : " (" + name.toStdString() + ")";
    };
    for (int c = 0; c < n; ++c) out.push_back({{c}, "In " + std::to_string(c + 1) + named(c)});
    for (int c = 0; c + 1 < n; c += 2)
        out.push_back({{c, c + 1}, "In " + std::to_string(c + 1) + "+" + std::to_string(c + 2) + " (stereo)"});
    return out;
}

void Session::setInput(const std::string& trackId, const std::vector<int>& channels) {
    if (channels.empty() || recording_) return;
    inputs_[trackId] = channels;
    if (monitoring_.load()) rebuild();
    else if (onChange) onChange();
}

float Session::takeInputPeak(int channel) {
    if (channel < 0 || channel >= kMaxMeteredInputs) return 0.f;
    return inputPeaks_[channel].exchange(0.f);
}

juce::File Session::ioSettingsFile() const {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Ynot")
        .getChildFile("io-settings.json");
}

int Session::numOutputPairs() const {
    if (device_.manager().getCurrentAudioDevice() == nullptr) return std::max(1, simulatedOutputs_ / 2);
    return std::max(1, (device_.outputChannelNames().size() + 1) / 2);
}

void Session::setMainOutputPair(int pair) {
    mainOutputPair_ = std::clamp(pair, 0, std::max(0, numOutputPairs() - 1));
    auto* obj = new juce::DynamicObject();
    obj->setProperty("mainOutputPair", mainOutputPair_.load());
    ioSettingsFile().getParentDirectory().createDirectory();
    ioSettingsFile().replaceWithText(juce::JSON::toString(juce::var(obj)));
    if (onChange) onChange();
}

std::string Session::outputPairName(int pair) const {
    const auto names = device_.outputChannelNames();
    std::string s = "Out " + std::to_string(pair * 2 + 1) + "+" + std::to_string(pair * 2 + 2);
    if (pair * 2 + 1 < names.size()) {
        const auto a = names[pair * 2], b = names[pair * 2 + 1];
        const bool generic = a.containsIgnoreCase("output") && a.length() <= 10;
        if (!generic) s += " (" + a.toStdString() + " / " + b.toStdString() + ")";
    }
    return s;
}

std::vector<int> Session::availableBufferSizes() const {
    if (device_.manager().getCurrentAudioDevice() != nullptr) return device_.availableBufferSizes();
    return {32, 64, 128, 256, 512, 1024, 2048};  // no hardware (tests, simulation)
}

juce::String Session::setBufferSize(int frames) {
    const auto sizes = availableBufferSizes();
    if (std::find(sizes.begin(), sizes.end(), frames) == sizes.end())
        return "A buffer of " + juce::String(frames) + " samples isn't available on this audio device.";
    if (recording_) return "Stop recording before changing the buffer size.";
    if (device_.manager().getCurrentAudioDevice() != nullptr) {
        auto err = device_.setBufferSize(frames);
        if (err.isNotEmpty()) return err;
        deviceChanged();
    } else {
        applyDeviceFormat(sampleRate_, frames);
    }
    return {};
}

bool Session::startRecording(std::string& error) {
    if (!loaded_ || recording_) return false;
    ynot::Recorder::Config cfg;
    cfg.sampleRate = sampleRate_;
    cfg.maxBlock = blockSize_;
    cfg.packageDir = dir_.getFullPathName().toStdString();
    for (const auto* t : project_.orderedTracks())
        if (t->armed && t->kind == ynot::TrackKind::Audio) cfg.targets.push_back({t->id, inputFor(t->id)});
    bool midiTargets = false;
    for (const auto* t : project_.orderedTracks())
        midiTargets |= t->armed && t->kind == ynot::TrackKind::Instrument;
    const bool audioTargets = !cfg.targets.empty();
    if (!audioTargets && !midiTargets) {
        error = "Arm a track (R) to record: audio tracks record your input, instrument tracks record MIDI.";
        return false;
    }
    if (audioTargets && numInputChannels() == 0) {
        error = "The current audio device has no inputs enabled. Choose an input in Audio settings.";
        return false;
    }
    // Count-in / punch pre-roll: start the transport early so the performer hears the lead-up,
    // but only record from the record point (the playhead, or punch-in).
    const bool punch = punchEnabled_ && punchOut_ > punchIn_;
    // Cycle recording: with the loop on (and punch off), record from the loop start, pass after pass.
    const bool cycle = !punch && loopEnabled();
    const int64_t recordStart = punch   ? ynot::tickToFrame(project_, punchIn_, sampleRate_)
                                : cycle ? ynot::tickToFrame(project_, project_.loop.startTick, sampleRate_)
                                        : std::max<int64_t>(0, position_.load());
    const int64_t recordEnd = punch ? ynot::tickToFrame(project_, punchOut_, sampleRate_)
                                    : std::numeric_limits<int64_t>::max();
    const int64_t countIn = ynot::tickToFrame(project_, int64_t(countInBars_) * project_.barTicks(), sampleRate_);
    // Punch always gets at least one bar of pre-roll so you can hear where you're coming in.
    const int64_t preRoll = punch ? std::max(countIn, ynot::tickToFrame(project_, project_.barTicks(), sampleRate_))
                                  : countIn;
    const int latency = device_.manager().getCurrentAudioDevice() ? ynot::recordingOffsetFrames(device_) : 0;
    cfg.startAtFrame = recordStart;
    // Audio arrives `latency` frames late, so keep capturing that much past punch-out.
    if (punch) cfg.stopAtFrame = recordEnd + latency;
    if (audioTargets && !recorder_.start(cfg)) {
        error = recorder_.error();
        return false;
    }
    if (midiTargets) midiRecorder_.start(recordStart, recordEnd);
    recordingAudio_ = audioTargets;
    recordingMidi_ = midiTargets;
    recordStart_ = recordStart;
    recordEnd_ = recordEnd;
    punchActive_ = punch;
    cycleActive_ = cycle;
    recordClock_ = recordStart - preRoll;
    cycleLoopFrames_ = cycle ? ynot::tickToFrame(project_, project_.loop.endTick, sampleRate_) - recordStart : 0;
    // Live waveform buffers: room for 30 minutes per armed track (the drawing simply stops after).
    livePeaks_.clear();
    for (const auto& target : cfg.targets) {
        auto lp = std::make_unique<LivePeaks>();
        lp->trackId = target.trackId;
        lp->channels = target.inputChannels;
        const auto capacity = static_cast<size_t>(sampleRate_ * 1800.0 / kLivePeakFrames);
        lp->lo.assign(capacity, 0.f);
        lp->hi.assign(capacity, 0.f);
        livePeaks_.push_back(std::move(lp));
    }
    forcedClickUntil_ = countIn > 0 ? recordStart : std::numeric_limits<int64_t>::min();
    position_ = recordStart - preRoll;
    rebuild();  // engine starts at the pre-roll position with the count-in click
    draining_ = audioTargets;
    if (audioTargets) drainThread_ = std::thread([this] {
        while (draining_.load()) {
            recorder_.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    recording_ = true;  // the audio callback starts capturing from its next block
    playing_ = true;
    if (onChange) onChange();
    return true;
}

Session::RecordResult Session::stopRecording() {
    RecordResult result;
    if (!recording_) return result;
    recording_ = false;
    playing_ = false;
    // Make sure the audio thread has left capture() before the recorder is torn down.
    while (inCallback_.load()) std::this_thread::yield();
    draining_ = false;
    if (drainThread_.joinable()) drainThread_.join();
    const int64_t stopFrame = std::min(recordClock_.load(), recordEnd_);  // punch-out ends the take
    auto takes = recordingAudio_ ? recorder_.finish() : std::vector<ynot::RecordedTake>{};
    auto midiEvents = recordingMidi_ ? midiRecorder_.finish() : std::vector<ynot::TimedMidiEvent>{};
    forcedClickUntil_ = std::numeric_limits<int64_t>::min();
    position_ = std::max<int64_t>(0, position_.load());
    result.droppedFrames = recorder_.droppedFrames();
    result.error = recorder_.error();
    const int latency = device_.manager().getCurrentAudioDevice() ? ynot::recordingOffsetFrames(device_) : 0;
    history_.record(project_);
    result.newClips = cycleActive_ ? ynot::commitCycleTakes(project_, dir_.getFullPathName().toStdString(), takes,
                                                            latency, project_.loop.startTick, project_.loop.endTick)
                                   : ynot::commitTakes(project_, dir_.getFullPathName().toStdString(), takes, latency);
    if (recordingMidi_) {
        if (punchActive_ && stopFrame > recordStart_) {
            // Punch replaces the old notes inside the range (outside is untouched).
            const int64_t in = ynot::frameToTick(project_, recordStart_, sampleRate_);
            const int64_t out = ynot::frameToTick(project_, stopFrame, sampleRate_);
            for (auto& t : project_.tracks)
                if (t.armed && t.kind == ynot::TrackKind::Instrument) ynot::eraseNotesInRange(t, in, out);
        }
        // MIDI is timestamped at the engine frame it was played, so no latency offset applies.
        const ynot::QuantizeSettings* q = recordQuantize();
        const int64_t loopEndFrame = ynot::tickToFrame(project_, project_.loop.endTick, sampleRate_);
        auto built = cycleActive_ ? ynot::buildCycleMidiClip(project_, midiEvents, recordStart_, loopEndFrame,
                                                             stopFrame, sampleRate_, q)
                                  : ynot::buildMidiClip(project_, midiEvents, recordStart_, stopFrame, sampleRate_, q);
        if (auto clip = built) {
            for (auto& t : project_.tracks)
                if (t.armed && t.kind == ynot::TrackKind::Instrument) {
                    auto c = *clip;
                    c.id = ynot::newId();  // each armed instrument gets its own copy
                    t.midiClips.push_back(c);
                    result.newClips.push_back(c.id);
                }
        }
        result.droppedMidiEvents = midiRecorder_.droppedEvents();
    }
    recordingAudio_ = recordingMidi_ = false;
    punchActive_ = false;
    cycleActive_ = false;
    recordEnd_ = std::numeric_limits<int64_t>::max();
    save();
    rebuild();
    return result;
}

double Session::recordingSeconds() const {
    if (recordingAudio_) return static_cast<double>(recorder_.recordedFrames()) / sampleRate_;
    return static_cast<double>(std::max<int64_t>(0, position_.load() - recordStart_)) / sampleRate_;
}

void Session::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) {
    // Learned knobs and transport buttons control Ynot; everything else plays the instruments.
    if (midiControls_->handle(m)) return;
    if (m.isSysEx() || m.isMidiClock() || m.isActiveSense()) return;
    collector_.addMessageToQueue(m);  // MIDI thread -> lock-free hand-off to the audio thread
}

void Session::applyMidiControl(const MidiMapping& map, int value) {
    if (!loaded_) return;
    // A burst of knob movement is one undo step.
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastMidiControlMs_ > 1000) beginMixChange();
    lastMidiControlMs_ = now;
    if (map.target == MidiTarget::MasterVolume) {
        setMasterGain(ccToVolumeDb(value));
    } else if (map.trackIndex >= 0 && map.trackIndex < static_cast<int>(project_.tracks.size())) {
        const auto id = project_.tracks[static_cast<size_t>(map.trackIndex)].id;
        if (map.target == MidiTarget::TrackVolume) setTrackGain(id, ccToVolumeDb(value));
        else if (map.target == MidiTarget::TrackPan) setTrackPan(id, ccToPan(value));
    }
    if (onChange) onChange();
}

// ---------------------------------------------------------------- AI + sampler

template <typename Work, typename Then>
void Session::runInBackground(Work work, Then then) {
    auto alive = alive_;
    juce::Thread::launch([alive, work = std::move(work), then = std::move(then)]() mutable {
        auto result = work();
        juce::MessageManager::callAsync([alive, then = std::move(then), result = std::move(result)]() mutable {
            if (alive->load()) then(std::move(result));
        });
    });
}

ynot::AiAvailability Session::aiAvailable(ynot::AiFeature f) {
    return ynot::aiAvailability(f, ynot::Tier::Pro, ynot::currentPlatform(), true, false);
}

void Session::analyzeSong(std::function<void(const SongAnalysis&)> done) {
    if (!loaded_) return;
    const auto avail = aiAvailable(ynot::AiFeature::ChordAndKeyDetection);
    if (!avail.available) {
        done({{}, {}, juce::String(avail.reason)});
        return;
    }
    runInBackground(
        [p = project_, dir = dir_.getFullPathName().toStdString()] {
            ynot::PackageMediaProvider media(dir);  // its own decoder cache: no sharing with the UI
            SongAnalysis r;
            r.key = ynot::detectSongKey(p, media);
            r.chords = ynot::detectChords(p, media);
            return r;
        },
        [done](SongAnalysis r) { done(r); });
}

int Session::applyKeyToTune(const ynot::KeyEstimate& k) {
    int n = 0;
    for (const auto& t : project_.tracks)
        for (const auto& fx : t.inserts) n += fx.uid == ynot::kBuiltinTune ? 1 : 0;
    if (n == 0) return 0;
    edit([&](ynot::Project& p) {
        for (auto& t : p.tracks)
            for (auto& fx : t.inserts)
                if (fx.uid == ynot::kBuiltinTune) {
                    fx.params["key"] = static_cast<int64_t>(k.tuneKey());
                    fx.params["scale"] = static_cast<int64_t>(k.tuneScale());
                }
    });
    return n;
}

void Session::convertClipToMidi(const std::string& trackId, const std::string& clipId,
                                std::function<void(const juce::String&)> done) {
    if (!loaded_) return;
    const auto avail = aiAvailable(ynot::AiFeature::TranscribeAudioToMidi);
    if (!avail.available) {
        done(juce::String(avail.reason));
        return;
    }
    runInBackground(
        [p = project_, dir = dir_.getFullPathName().toStdString(), trackId, clipId] {
            ynot::PackageMediaProvider media(dir);
            ynot::Project copy = p;
            auto id = ynot::audioClipToMidi(copy, media, trackId, clipId);
            std::optional<ynot::Track> made;
            if (id) made = *copy.findTrack(*id);
            return made;
        },
        [this, done, trackId](std::optional<ynot::Track> made) {
            if (!made) {
                done("No clear melody was found. Audio to MIDI works on one note at a time (a voice, bass or lead).");
                return;
            }
            edit([&](ynot::Project& p) {
                // Right under the source track.
                auto it = std::find_if(p.tracks.begin(), p.tracks.end(), [&](const ynot::Track& t) { return t.id == trackId; });
                p.tracks.insert(it == p.tracks.end() ? p.tracks.end() : it + 1, *made);
            });
            done({});
        });
}

void Session::detectClipTempo(const std::string& trackId, const std::string& clipId,
                              std::function<void(ynot::TempoEstimate)> done) {
    if (!loaded_ || !media_) return;
    const auto audio = ynot::clipAudio(project_, *media_, trackId, clipId);
    runInBackground([audio] { return ynot::detectTempo(audio); }, [done](ynot::TempoEstimate e) { done(e); });
}

juce::String Session::newSamplerFromFileNow(const juce::File& file, std::string& trackId) {
    if (!loaded_) return "Open or create a song first.";
    auto decoded = decodeAudioFile(file);
    if (!decoded.ok) return decoded.error;
    edit([&](ynot::Project& p) {
        trackId = ynot::addSamplerTrack(p, dir_.getFullPathName().toStdString(), decoded.audio,
                                        file.getFileNameWithoutExtension().toStdString());
    });
    return {};
}

void Session::newSamplerFromFile(const juce::File& file, std::function<void(const juce::String&, const std::string&)> done) {
    if (!loaded_) return;
    runInBackground([file] { return decodeAudioFile(file); },
                    [this, file, done](DecodedAudio d) {
                        if (!d.ok) {
                            done(d.error, {});
                            return;
                        }
                        std::string id;
                        edit([&](ynot::Project& p) {
                            id = ynot::addSamplerTrack(p, dir_.getFullPathName().toStdString(), d.audio,
                                                       file.getFileNameWithoutExtension().toStdString());
                        });
                        done({}, id);
                    });
}

void Session::loadSampleFile(const std::string& trackId, const juce::File& file, std::function<void(const juce::String&)> done) {
    if (!loaded_) return;
    runInBackground([file] { return decodeAudioFile(file); },
                    [this, trackId, done](DecodedAudio d) {
                        if (!d.ok) {
                            done(d.error);
                            return;
                        }
                        edit([&](ynot::Project& p) { ynot::loadSample(p, dir_.getFullPathName().toStdString(), trackId, d.audio); });
                        done({});
                    });
}

std::string Session::sendClipToSampler(const std::string& trackId, const std::string& clipId) {
    if (!loaded_ || !media_) return {};
    const auto audio = ynot::clipAudio(project_, *media_, trackId, clipId);
    if (audio.frames() <= 0) return {};
    const auto* t = project_.findTrack(trackId);
    const std::string name = (t ? t->name : std::string("Clip")) + " chops";
    std::string id;
    edit([&](ynot::Project& p) { id = ynot::addSamplerTrack(p, dir_.getFullPathName().toStdString(), audio, name); });
    return id;
}

void Session::setMidiClockOut(bool on) {
    midiClock_.setEnabled(on);
    midiControls_->setClockOut(on);
}

int Session::numMidiInputs() const {
    int n = 0;
    for (auto& d : juce::MidiInput::getAvailableDevices())
        n += device_.manager().isMidiInputDeviceEnabled(d.identifier) ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------- quantize / time signature / punch

void Session::importAudioFile(const juce::File& file, const std::string& trackId, int64_t startTick,
                              std::function<void(const juce::String&)> done) {
    if (!loaded_) {
        if (done) done("Open or create a project first.");
        return;
    }
    juce::Thread::launch([this, file, trackId, startTick, done] {
        auto decoded = std::make_shared<DecodedAudio>(decodeAudioFile(file));
        juce::MessageManager::callAsync([this, file, trackId, startTick, done, decoded] {
            const auto err = decoded->ok ? commitImport(file, *decoded, trackId, startTick) : decoded->error;
            if (done) done(err);
        });
    });
}

ExportJob Session::prepareExport(const ExportSettings& s) {
    host_.captureState(project_);  // plugin settings changed in their windows
    return ynotapp::prepareExport(project_, *media_, host_, s);
}

void Session::stretchClip(const std::string& trackId, const std::string& clipId, double ratio,
                          ynot::StretchMode mode, std::function<void(const juce::String&)> done) {
    if (!loaded_ || !media_) {
        if (done) done("Open or create a project first.");
        return;
    }
    std::string error;
    auto job = ynot::prepareStretch(project_, *media_, trackId, clipId, ratio, mode, &error);
    if (!job) {
        if (done) done(error);
        return;
    }
    ++stretchJobs_;
    if (onChange) onChange();
    auto shared = std::make_shared<ynot::StretchJob>(std::move(*job));
    const auto dir = dir_;
    juce::Thread::launch([this, shared, dir, done] {
        auto rendered = std::make_shared<ynot::AudioBuffer>(ynot::renderStretch(*shared));
        juce::MessageManager::callAsync([this, shared, dir, done, rendered] {
            --stretchJobs_;
            juce::String err;
            if (dir != dir_) err = "The project was closed before the stretch finished.";
            else if (!ynot::findClip(project_, shared->trackId, shared->clipId))
                err = "The clip was deleted while it was being stretched.";
            else {
                bool ok = false;
                edit([&](ynot::Project& p) { ok = ynot::commitStretch(p, dir_.getFullPathName().toStdString(), *shared, *rendered); });
                if (ok) save();
                else err = "The clip was changed or deleted while it was being stretched.";
            }
            if (onChange) onChange();
            if (done) done(err);
        });
    });
}

juce::String Session::stretchClipNow(const std::string& trackId, const std::string& clipId, double ratio,
                                     ynot::StretchMode mode) {
    if (!loaded_ || !media_) return "Open or create a project first.";
    std::string error;
    auto job = ynot::prepareStretch(project_, *media_, trackId, clipId, ratio, mode, &error);
    if (!job) return error;
    const auto rendered = ynot::renderStretch(*job);
    bool ok = false;
    edit([&](ynot::Project& p) { ok = ynot::commitStretch(p, dir_.getFullPathName().toStdString(), *job, rendered); });
    if (!ok) return "The clip couldn't be stretched.";
    save();
    return {};
}

juce::String Session::importAudioFileNow(const juce::File& file, const std::string& trackId, int64_t startTick) {
    if (!loaded_) return "Open or create a project first.";
    const auto decoded = decodeAudioFile(file);
    return decoded.ok ? commitImport(file, decoded, trackId, startTick) : decoded.error;
}

juce::String Session::commitImport(const juce::File& file, const DecodedAudio& decoded, const std::string& trackId,
                                   int64_t startTick) {
    bool ok = false;
    edit([&](ynot::Project& p) {
        std::string target = trackId;
        const auto* t = p.findTrack(target);
        if (!t || t->kind != ynot::TrackKind::Audio)
            target = ynot::addTrack(p, ynot::TrackKind::Audio, file.getFileNameWithoutExtension().toStdString());
        ok = !ynot::importAudioClip(p, dir_.getFullPathName().toStdString(), target, decoded.audio, startTick).empty();
    });
    save();  // the media file is already in the package; keep project.json in step
    return ok ? juce::String() : juce::String("Couldn't add the audio to the project.");
}

void Session::setRecordQuantize(const std::string& presetId) {
    recordQuantizeId_ = ynot::findQuantizePreset(presetId) ? presetId : std::string();
}

const ynot::QuantizeSettings* Session::recordQuantize() {
    const auto* preset = ynot::findQuantizePreset(recordQuantizeId_);
    if (!preset) return nullptr;
    recordQuantizeSettings_ = preset->settings;
    recordQuantizeSettings_.strength = quantizeStrength_;
    return &recordQuantizeSettings_;
}

bool Session::quantizeClip(const std::string& trackId, const std::string& clipId, const std::string& presetId) {
    const auto* preset = ynot::findQuantizePreset(presetId);
    if (!preset || !loaded_) return false;
    auto settings = preset->settings;
    settings.strength = quantizeStrength_;
    bool found = false;
    edit([&](ynot::Project& p) {
        if (auto* t = p.findTrack(trackId))
            for (auto& c : t->midiClips)
                if (c.id == clipId) {
                    ynot::quantizeClip(c, settings);
                    found = true;
                }
    });
    return found;
}

void Session::setTimeSignature(int numerator, int denominator) {
    edit([&](ynot::Project& p) {
        p.timeSigNumerator = std::clamp(numerator, 1, 32);
        p.timeSigDenominator = denominator;
    });
}

void Session::setPunchRange(int64_t inTick, int64_t outTick) {
    if (outTick < inTick) std::swap(inTick, outTick);
    punchIn_ = std::max<int64_t>(0, inTick);
    punchOut_ = std::max(punchIn_ + ynot::kMinClipTicks, outTick);
    if (onChange) onChange();
}

void Session::setMonitoring(bool on) {
    monitoring_ = on;
    rebuild();
}

double Session::monitoringLatencyMs() const {
    auto* d = device_.manager().getCurrentAudioDevice();
    if (!d) return 0.0;
    const double frames = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples() +
                          d->getCurrentBufferSizeSamples();
    return frames / d->getCurrentSampleRate() * 1000.0;
}

void Session::setMetronome(bool on) {
    metronomeOn_ = on;
    if (auto* e = active_.load()) e->setMetronome(on);
}

void Session::processForTesting(const float* const* ins, int numIns, float* const* outs, int numOuts, int frames) {
    audioCallback(ins, numIns, outs, numOuts, frames);
}

void Session::play() { playing_ = true; }

void Session::stop() {
    if (recording_) { stopRecording(); return; }
    playing_ = false;
    position_ = 0;
    rebuild();  // rewinds and resets plugin/instrument state
}

double Session::positionSeconds() const { return static_cast<double>(position_.load()) / sampleRate_; }

void Session::audioCallback(const float* const* ins, int numIns, float* const* outs, int numOuts, int frames) {
    inCallback_ = true;
    ynot::Engine* engine = active_.load();
    if (engine) {
        const int64_t seek = pendingSeek_.exchange(kNoSeek);
        if (seek != kNoSeek) engine->seek(seek);
    }
    // Live MIDI for this block (hardware controllers + on-screen keyboard), sample-accurate.
    midiBuffer_.clear();
    collector_.removeNextBlockOfMessages(midiBuffer_, frames);
    int numLive = 0;
    for (const auto meta : midiBuffer_) {
        const auto& m = meta.getMessage();
        if (numLive >= kMaxLiveMidi) break;
        if (m.isNoteOn()) liveMidi_[static_cast<size_t>(numLive++)] = {meta.samplePosition, true, m.getNoteNumber(), m.getVelocity()};
        else if (m.isNoteOff()) liveMidi_[static_cast<size_t>(numLive++)] = {meta.samplePosition, false, m.getNoteNumber(), 0};
        else if (m.isController())
            liveMidi_[static_cast<size_t>(numLive++)] =
                ynot::MidiEvent::control(meta.samplePosition, m.getControllerNumber(), m.getControllerValue());
        else if (m.isPitchWheel())
            liveMidi_[static_cast<size_t>(numLive++)] = ynot::MidiEvent::bend(meta.samplePosition, m.getPitchWheelValue());
    }
    // Capture before rendering: some drivers share input and output buffers.
    // Recording uses its own continuous clock: the playhead jumps back at the loop end, the
    // recording doesn't (cycle passes follow each other in the take).
    const bool capturing = engine && (recordingAudio_.load() || recordingMidi_.load());
    const int64_t clock = recordClock_.load();
    if (engine && recordingAudio_.load()) {
        recorder_.capture(ins, numIns, frames, clock);
        captureLivePeaks(ins, numIns, frames, clock);
    }
    if (engine && recordingMidi_.load()) midiRecorder_.capture(liveMidi_.data(), numLive, clock);
    if (capturing) recordClock_ = clock + frames;
    for (int c = 0; c < std::min(numIns, kMaxMeteredInputs); ++c)
        if (ins[c]) {
            const float pk = juce::FloatVectorOperations::findMaximum(ins[c], frames);
            const float pk2 = -juce::FloatVectorOperations::findMinimum(ins[c], frames);
            const float v = std::max(pk, pk2);
            if (v > inputPeaks_[c].load(std::memory_order_relaxed)) inputPeaks_[c].store(v, std::memory_order_relaxed);
        }
    if (numOuts < 2) frames = std::min(frames, kScratchFrames);
    // The song goes to the chosen output pair; every other output stays silent.
    const int base = numOuts >= 2 ? std::min(mainOutputPair_.load(std::memory_order_relaxed) * 2, numOuts - 2) : 0;
    float* l = numOuts > 0 ? outs[base] : scratchL_.data();
    float* r = numOuts > 1 ? outs[base + 1] : scratchR_.data();
    const bool running = playing_.load();
    if (engine) {
        const int64_t at = engine->position();
        midiClock_.processBlock(running, engine->bpmAtFrame(at), sampleRate_, frames, engine->tickAtFrame(at),
                                static_cast<double>(ynot::kTicksPerQuarter));
    }
    if (engine && (running || monitoring_.load() || liveMidiActive_.load())) {
        // Drivers may deliver blocks larger than requested; render in chunks.
        const float* chunkIns[16] = {};
        const int numChunkIns = std::min(numIns, 16);
        for (int done = 0; done < frames;) {
            int n = std::min(frames - done, engine->maxBlock());
            for (int c = 0; c < numChunkIns; ++c) chunkIns[c] = ins[c] ? ins[c] + done : nullptr;
            int numChunkMidi = 0;
            for (int k = 0; k < numLive; ++k) {
                const auto& ev = liveMidi_[static_cast<size_t>(k)];
                if (ev.frameOffset >= done && ev.frameOffset < done + n) {
                    chunkMidi_[static_cast<size_t>(numChunkMidi)] = ev;
                    chunkMidi_[static_cast<size_t>(numChunkMidi++)].frameOffset -= done;
                }
            }
            engine->process(l + done, r + done, n, chunkIns, numChunkIns, running, chunkMidi_.data(), numChunkMidi);
            done += n;
        }
        position_ = engine->position();
    } else {
        juce::FloatVectorOperations::clear(l, frames);
        juce::FloatVectorOperations::clear(r, frames);
    }
    for (int c = 0; c < numOuts; ++c)
        if (c != base && c != base + 1) juce::FloatVectorOperations::clear(outs[c], frames);
    inCallback_ = false;
}

void Session::captureLivePeaks(const float* const* ins, int numIns, int frames, int64_t clock) {
    for (auto& lpPtr : livePeaks_) {
        auto& lp = *lpPtr;
        int n = lp.count.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i) {
            const int64_t f = clock + i;
            if (f < recordStart_ || f >= recordEnd_) continue;  // count-in / after punch-out
            float lo = 0, hi = 0;
            for (int ch : lp.channels)
                if (ch >= 0 && ch < numIns && ins[ch]) {
                    lo = std::min(lo, ins[ch][i]);
                    hi = std::max(hi, ins[ch][i]);
                }
            if (lp.curN == 0) lp.curLo = lo, lp.curHi = hi;
            lp.curLo = std::min(lp.curLo, lo);
            lp.curHi = std::max(lp.curHi, hi);
            if (++lp.curN == kLivePeakFrames) {
                if (n < static_cast<int>(lp.lo.size())) {
                    lp.lo[static_cast<size_t>(n)] = lp.curLo;
                    lp.hi[static_cast<size_t>(n)] = lp.curHi;
                    ++n;
                }
                lp.curN = 0;
            }
        }
        lp.count.store(n, std::memory_order_release);
    }
}

Session::LiveWave Session::liveWaveform(const std::string& trackId) const {
    LiveWave w;
    if (!recording_.load()) return w;
    for (const auto& lp : livePeaks_) {
        if (lp->trackId != trackId) continue;
        const int count = lp->count.load(std::memory_order_acquire);
        int first = 0;
        int64_t firstFrame = recordStart_;
        if (cycleActive_ && cycleLoopFrames_ > 0) {  // only the pass being recorded now
            const int64_t pass = static_cast<int64_t>(count) * kLivePeakFrames / cycleLoopFrames_;
            first = static_cast<int>(pass * cycleLoopFrames_ / kLivePeakFrames);
            firstFrame = recordStart_ + static_cast<int64_t>(first) * kLivePeakFrames - pass * cycleLoopFrames_;
        }
        w.lo = lp->lo.data() + first;
        w.hi = lp->hi.data() + first;
        w.count = count - first;
        w.firstFrame = firstFrame;
        return w;
    }
    return w;
}

ynot::CompatReport Session::compatibilityFor(ynot::Platform target) const {
    // For this device use the real catalog; for others assume a clean install
    // (builtins only), the honest default until devices publish their catalogs (M3).
    ynot::DeviceCatalog cat = target == ynot::currentPlatform() ? host_.catalog()
                                                                : ynot::DeviceCatalog::builtinOnly(target);
    return ynot::analyzeCompatibility(project_, cat);
}

std::vector<std::string> Session::tracksToPrepareForMobile() const {
    return ynot::tracksNeedingFreezeFor(project_, host_.catalog(),
                                        {ynot::DeviceCatalog::builtinOnly(ynot::Platform::IOS),
                                         ynot::DeviceCatalog::builtinOnly(ynot::Platform::Android)});
}

}  // namespace ynotapp
