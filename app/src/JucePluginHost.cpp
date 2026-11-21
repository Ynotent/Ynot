#include "JucePluginHost.h"

#include "ynot/platform.hpp"

namespace ynotapp {

std::string portableUid(const juce::PluginDescription& d) {
    // uniqueId is the VST3 class-id hash / AU component code: stable across machines.
    return juce::String::toHexString(d.uniqueId).paddedLeft('0', 8).toStdString() + ":" +
           d.manufacturerName.toStdString() + ":" + d.name.toStdString();
}

std::optional<ynot::PluginFormat> ynotFormatFor(const juce::PluginDescription& d) {
    if (d.pluginFormatName == "VST3") return ynot::PluginFormat::VST3;
    if (d.pluginFormatName == "AudioUnit") {
        // On iOS every Audio Unit is an AUv3 extension. On macOS JUCE scans v2 and v3
        // components together; they're recorded as AU (v2), which is conservative: a
        // macOS AUv3 that also exists on iOS is reported as unavailable there.
        return ynot::currentPlatform() == ynot::Platform::IOS ? ynot::PluginFormat::AUv3
                                                              : ynot::PluginFormat::AudioUnit;
    }
    return std::nullopt;
}

std::string encodeState(const juce::MemoryBlock& mb) {
    return juce::Base64::toBase64(mb.getData(), mb.getSize()).toStdString();
}

juce::MemoryBlock decodeState(const std::string& b64) {
    juce::MemoryOutputStream out;
    if (!b64.empty()) juce::Base64::convertFromBase64(out, juce::String(b64));
    return out.getMemoryBlock();
}

// ---------------------------------------------------------------- adapter

JuceProcessorAdapter::JuceProcessorAdapter(std::unique_ptr<juce::AudioProcessor> p, double sr, int maxBlock)
    : proc_(std::move(p)) {
    proc_->enableAllBuses();
    const int channels = std::max({2, proc_->getTotalNumInputChannels(), proc_->getTotalNumOutputChannels()});
    buffer_.setSize(channels, maxBlock);
    midi_.ensureSize(4096);
    proc_->setRateAndBufferSizeDetails(sr, maxBlock);
    proc_->prepareToPlay(sr, maxBlock);
    for (auto* param : proc_->getParameters()) {
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(param))
            params_[hosted->getParameterID().toStdString()] = param;
        else
            params_[std::to_string(param->getParameterIndex())] = param;
    }
}

JuceProcessorAdapter::~JuceProcessorAdapter() { proc_->releaseResources(); }

void JuceProcessorAdapter::process(float* left, float* right, int frames, const ynot::MidiEvent* events,
                                   int numEvents) {
    frames = std::min(frames, buffer_.getNumSamples());
    // Use the preallocated buffer (no allocation on the audio thread).
    buffer_.setSize(buffer_.getNumChannels(), frames, false, false, true);
    buffer_.clear();
    buffer_.copyFrom(0, 0, left, frames);
    buffer_.copyFrom(1, 0, right, frames);
    midi_.clear();
    for (int i = 0; i < numEvents; ++i) {
        const auto& e = events[i];
        auto msg = e.noteOn ? juce::MidiMessage::noteOn(1, e.pitch, static_cast<juce::uint8>(e.velocity))
                            : juce::MidiMessage::noteOff(1, e.pitch);
        midi_.addEvent(msg, std::clamp(e.frameOffset, 0, frames - 1));
    }
    proc_->processBlock(buffer_, midi_);
    if (proc_->acceptsMidi() && proc_->getTotalNumInputChannels() == 0) {
        // Instruments add to whatever is already on the track.
        juce::FloatVectorOperations::add(left, buffer_.getReadPointer(0), frames);
        juce::FloatVectorOperations::add(right, buffer_.getReadPointer(1), frames);
    } else {
        juce::FloatVectorOperations::copy(left, buffer_.getReadPointer(0), frames);
        juce::FloatVectorOperations::copy(right, buffer_.getReadPointer(1), frames);
    }
}

void JuceProcessorAdapter::setParameter(const std::string& id, double v) {
    auto it = params_.find(id);
    if (it != params_.end()) it->second->setValue(static_cast<float>(std::clamp(v, 0.0, 1.0)));
}

void JuceProcessorAdapter::reset() { proc_->reset(); }

namespace {
// Adapter that tells the host when it's created/destroyed, so the host can
// capture the state of live plugins before saving.
class TrackedAdapter : public JuceProcessorAdapter {
public:
    TrackedAdapter(JucePluginHost& host, std::string id, std::unique_ptr<juce::AudioProcessor> p, double sr, int mb)
        : JuceProcessorAdapter(std::move(p), sr, mb), host_(host), id_(std::move(id)) {
        host_.registerLive(id_, this);
    }
    ~TrackedAdapter() override { host_.unregisterLive(id_, this); }

private:
    JucePluginHost& host_;
    std::string id_;
};
}  // namespace

// ---------------------------------------------------------------- host

JucePluginHost::JucePluginHost() {
    const auto platform = ynot::currentPlatform();
#if JUCE_PLUGINHOST_VST3
    if (ynot::platformLoadsFormat(platform, ynot::PluginFormat::VST3))
        formats_.addFormat(new juce::VST3PluginFormat());
#endif
#if JUCE_PLUGINHOST_AU && (JUCE_MAC || JUCE_IOS)
    formats_.addFormat(new juce::AudioUnitPluginFormat());
#endif
    juce::ignoreUnused(platform);
}

JucePluginHost::~JucePluginHost() = default;

void JucePluginHost::registerLive(const std::string& id, JuceProcessorAdapter* a) {
    std::lock_guard<std::mutex> lock(liveMutex_);
    live_.emplace(id, a);
}

void JucePluginHost::unregisterLive(const std::string& id, JuceProcessorAdapter* a) {
    std::lock_guard<std::mutex> lock(liveMutex_);
    auto [b, e] = live_.equal_range(id);
    for (auto it = b; it != e; ++it)
        if (it->second == a) { live_.erase(it); return; }
}

bool JucePluginHost::isAvailable(const ynot::PluginInstance& p) const {
    if (p.format == ynot::PluginFormat::Builtin) return builtin_.isAvailable(p);
    if (!ynot::platformLoadsFormat(ynot::currentPlatform(), p.format)) return false;
    for (const auto& d : known_.getTypes())
        if (ynotFormatFor(d) == p.format && portableUid(d) == p.uid) return true;
    return false;
}

std::unique_ptr<ynot::PluginProcessor> JucePluginHost::load(const ynot::PluginInstance& p, double sr,
                                                            int maxBlock) {
    if (p.format == ynot::PluginFormat::Builtin) return builtin_.load(p, sr, maxBlock);
    if (!isAvailable(p)) return nullptr;
    juce::PluginDescription desc;
    bool found = false;
    for (const auto& d : known_.getTypes())
        if (ynotFormatFor(d) == p.format && portableUid(d) == p.uid) { desc = d; found = true; break; }
    if (!found) return nullptr;

    juce::String error;
    // Synchronous creation must run on the message thread. AUv3 on iOS needs the
    // async API; the iOS shell (M4) adds that path.
    std::unique_ptr<juce::AudioProcessor> inst = formats_.createPluginInstance(desc, sr, maxBlock, error);
    if (!inst) return nullptr;
    auto state = decodeState(p.stateBase64);
    if (state.getSize() > 0) inst->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    return std::make_unique<TrackedAdapter>(*this, p.id, std::move(inst), sr, maxBlock);
}

void JucePluginHost::scanDefaultLocations(std::function<void(const juce::String&)> progress) {
    auto deadMansPedal = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ynot-scan.txt");
    for (auto* format : formats_.getFormats()) {
        juce::PluginDirectoryScanner scanner(known_, *format, format->getDefaultLocationsToSearch(), true,
                                             deadMansPedal);
        juce::String name;
        while (scanner.scanNextFile(true, name))
            if (progress) progress(format->getName() + ": " + name);
    }
}

ynot::DeviceCatalog JucePluginHost::catalog() const {
    ynot::DeviceCatalog c{ynot::currentPlatform(), {}, false};
    for (const auto& d : known_.getTypes())
        if (auto f = ynotFormatFor(d); f && ynot::platformLoadsFormat(c.platform, *f))
            c.installed.insert({*f, portableUid(d)});
    return c;
}

std::optional<ynot::PluginInstance> JucePluginHost::makeInstance(const juce::PluginDescription& d) const {
    auto f = ynotFormatFor(d);
    if (!f) return std::nullopt;
    ynot::PluginInstance p;
    p.id = ynot::newId();
    p.name = d.name.toStdString();
    p.vendor = d.manufacturerName.toStdString();
    p.format = *f;
    p.uid = portableUid(d);
    p.version = d.version.toStdString();
    p.seenOn.insert(ynot::currentPlatform());
    return p;
}

void JucePluginHost::captureState(ynot::Project& project) const {
    std::lock_guard<std::mutex> lock(liveMutex_);
    auto capture = [&](ynot::PluginInstance& p) {
        auto it = live_.find(p.id);
        if (it == live_.end()) return;
        juce::MemoryBlock mb;
        it->second->processor().getStateInformation(mb);
        p.stateBase64 = encodeState(mb);
        p.seenOn.insert(ynot::currentPlatform());
    };
    for (auto& t : project.tracks) {
        if (t.instrument) capture(*t.instrument);
        for (auto& p : t.inserts) capture(p);
    }
}

}  // namespace ynotapp
