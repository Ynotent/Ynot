#include "Session.h"

#include <thread>

namespace ynotapp {

Session::Session(JucePluginHost& host) : host_(host) {}

Session::~Session() {
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
    bool ok = device_.open(deviceId, cfg, [this](const float* const*, int, float* const* outs, int numOuts, int n) {
        audioCallback(outs, numOuts, n);
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
    playing_ = false;
    device_.close();
}

void Session::play() { playing_ = true; }

void Session::stop() {
    playing_ = false;
    position_ = 0;
    rebuild();  // rewinds and resets plugin/instrument state
}

double Session::positionSeconds() const { return static_cast<double>(position_.load()) / sampleRate_; }

void Session::audioCallback(float* const* outs, int numOuts, int frames) {
    inCallback_ = true;
    ynot::Engine* engine = active_.load();
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
