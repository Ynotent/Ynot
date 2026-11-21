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
    device_.onDeviceChanged = [this] { deviceChanged(); };  // on-screen keyboard -> same path as hardware MIDI
}

Session::~Session() {
    if (recording_) stopRecording();
    keyboardState_.removeListener(&collector_);
    device_.manager().removeMidiInputDeviceCallback({}, this);
    stopAudio();
    delete active_.exchange(nullptr);
}

void Session::newDemo(const juce::File& dir) {
    dir.createDirectory();
    project_ = ynot::makeDemoProject(dir.getFullPathName().toStdString());
    history_.clear();
    dir_ = dir;
    loaded_ = true;
    project_.sampleRateHint = sampleRate_;  // a new project starts at the device's current rate
    ynot::saveProjectPackage(project_, dir_.getFullPathName().toStdString());
    media_ = std::make_unique<ynot::PackageMediaProvider>(dir_.getFullPathName().toStdString());
    rebuild();
}

void Session::open(const juce::File& dir) {
    project_ = ynot::loadProjectPackage(dir.getFullPathName().toStdString());
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
}

void Session::edit(const std::function<void(ynot::Project&)>& fn) {
    if (!loaded_) return;
    host_.captureState(project_);  // keep plugin tweaks made in their editors
    history_.record(project_);
    fn(project_);
    rebuild();
}

bool Session::undo() {
    if (!loaded_ || recording_ || !history_.canUndo()) return false;
    host_.captureState(project_);
    history_.undo(project_);
    rebuild();
    return true;
}

bool Session::redo() {
    if (!loaded_ || recording_ || !history_.canRedo()) return false;
    host_.captureState(project_);
    history_.redo(project_);
    rebuild();
    return true;
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
        if (t->armed && t->kind == ynot::TrackKind::Instrument) {
            opts.liveMidiTracks.insert(t->id);  // armed instruments play from MIDI input
            liveMidi = true;
        }
    liveMidiActive_ = liveMidi;
    fresh->prepare(project_, *media_, host_, opts);
    pendingSeek_ = kNoSeek;  // position_ already holds the latest seek; a stale one must not override it
    fresh->seek(position_.load());
    fresh->setMetronome(metronomeOn_.load());
    fresh->setForcedClickUntil(forcedClickUntil_);
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

std::string Session::inputLabel(const std::string& trackId) const {
    auto ch = inputFor(trackId);
    std::string s = "In " + std::to_string(ch[0] + 1);
    if (ch.size() > 1) s += "+" + std::to_string(ch[1] + 1);
    return s;
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
    const int64_t recordStart = punch ? ynot::tickToFrame(project_, punchIn_, sampleRate_)
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
    const int64_t stopFrame = std::min(position_.load(), recordEnd_);  // punch-out ends the take
    auto takes = recordingAudio_ ? recorder_.finish() : std::vector<ynot::RecordedTake>{};
    auto midiEvents = recordingMidi_ ? midiRecorder_.finish() : std::vector<ynot::TimedMidiEvent>{};
    forcedClickUntil_ = std::numeric_limits<int64_t>::min();
    position_ = std::max<int64_t>(0, position_.load());
    result.droppedFrames = recorder_.droppedFrames();
    result.error = recorder_.error();
    const int latency = device_.manager().getCurrentAudioDevice() ? ynot::recordingOffsetFrames(device_) : 0;
    history_.record(project_);
    result.newClips = ynot::commitTakes(project_, dir_.getFullPathName().toStdString(), takes, latency);
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
        if (auto clip = ynot::buildMidiClip(project_, midiEvents, recordStart_, stopFrame, sampleRate_, q)) {
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
    collector_.addMessageToQueue(m);  // MIDI thread -> lock-free hand-off to the audio thread
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
    }
    // Capture before rendering: some drivers share input and output buffers.
    if (engine && recordingAudio_.load()) recorder_.capture(ins, numIns, frames, engine->position());
    if (engine && recordingMidi_.load()) midiRecorder_.capture(liveMidi_.data(), numLive, engine->position());
    if (numOuts < 2) frames = std::min(frames, kScratchFrames);
    float* l = numOuts > 0 ? outs[0] : scratchL_.data();
    float* r = numOuts > 1 ? outs[1] : scratchR_.data();
    const bool running = playing_.load();
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
    for (int c = 2; c < numOuts; ++c) juce::FloatVectorOperations::clear(outs[c], frames);
    inCallback_ = false;
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
