#include "Session.h"

#include <thread>

namespace ynotapp {

Session::Session(JucePluginHost& host) : host_(host) {}

Session::~Session() {
    if (recording_) stopRecording();
    stopAudio();
    delete active_.exchange(nullptr);
}

void Session::newDemo(const juce::File& dir) {
    dir.createDirectory();
    project_ = ynot::makeDemoProject(dir.getFullPathName().toStdString());
    dir_ = dir;
    loaded_ = true;
    ynot::saveProjectPackage(project_, dir_.getFullPathName().toStdString());
    media_ = std::make_unique<ynot::PackageMediaProvider>(dir_.getFullPathName().toStdString());
    rebuild();
}

void Session::open(const juce::File& dir) {
    project_ = ynot::loadProjectPackage(dir.getFullPathName().toStdString());
    dir_ = dir;
    loaded_ = true;
    media_ = std::make_unique<ynot::PackageMediaProvider>(dir_.getFullPathName().toStdString());
    rebuild();
}

void Session::save() {
    if (!loaded_) return;
    host_.captureState(project_);
    ynot::saveProjectPackage(project_, dir_.getFullPathName().toStdString());
}

void Session::edit(const std::function<void(ynot::Project&)>& fn) {
    if (!loaded_) return;
    host_.captureState(project_);  // keep plugin tweaks made in their editors
    fn(project_);
    rebuild();
}

void Session::freeze(const std::string& trackId) {
    if (!loaded_) return;
    host_.captureState(project_);
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
    fresh->prepare(project_, *media_, host_, opts);
    fresh->seek(position_.load());
    fresh->setMetronome(metronomeOn_.load());
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
    scratchL_.assign(static_cast<size_t>(blockSize_), 0.f);
    scratchR_.assign(static_cast<size_t>(blockSize_), 0.f);
    rebuild();  // engine must match the device's actual rate and block size
    device_.start();
    return true;
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
    if (onChange) onChange();
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
    if (cfg.targets.empty()) {
        error = "Arm an audio track (R) to record.";
        return false;
    }
    if (numInputChannels() == 0) {
        error = "The current audio device has no inputs enabled. Choose an input in Audio settings.";
        return false;
    }
    if (!recorder_.start(cfg)) {
        error = recorder_.error();
        return false;
    }
    draining_ = true;
    drainThread_ = std::thread([this] {
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
    auto takes = recorder_.finish();
    result.droppedFrames = recorder_.droppedFrames();
    result.error = recorder_.error();
    const int latency = device_.manager().getCurrentAudioDevice() ? ynot::recordingOffsetFrames(device_) : 0;
    result.newClips = ynot::commitTakes(project_, dir_.getFullPathName().toStdString(), takes, latency);
    save();
    rebuild();
    return result;
}

double Session::recordingSeconds() const {
    return static_cast<double>(recorder_.recordedFrames()) / sampleRate_;
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
    // Capture before rendering: some drivers share input and output buffers.
    if (engine && recording_.load()) recorder_.capture(ins, numIns, frames, engine->position());
    float* l = numOuts > 0 ? outs[0] : scratchL_.data();
    float* r = numOuts > 1 ? outs[1] : scratchR_.data();
    if (engine && playing_.load()) {
        // Drivers may deliver blocks larger than requested; render in chunks.
        for (int done = 0; done < frames;) {
            int n = std::min(frames - done, engine->maxBlock());
            engine->process(l + done, r + done, n);
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
