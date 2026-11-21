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

namespace {
// Pseudo-format used only by tests to exercise crash/hang/success paths of the child.
constexpr const char* kTestFormat = "YnotScanTest";

// VST3 hosting exists only on desktop; on iOS/Android JUCE declares the class without an
// implementation, so it must not even be compiled there.
void addDesktopFormats(juce::AudioPluginFormatManager& fm) {
    const auto platform = ynot::currentPlatform();
#if JUCE_PLUGINHOST_VST3 && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
    if (ynot::platformLoadsFormat(platform, ynot::PluginFormat::VST3)) fm.addFormat(new juce::VST3PluginFormat());
#endif
#if JUCE_PLUGINHOST_AU && (JUCE_MAC || JUCE_IOS)
    fm.addFormat(new juce::AudioUnitPluginFormat());
#endif
    juce::ignoreUnused(platform, fm);
}
}  // namespace

JucePluginHost::JucePluginHost() { addDesktopFormats(formats_); }

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


int JucePluginHost::runScanChild(const juce::StringArray& args) {
    // args: --scan-one <format> <fileOrIdentifier> <outFile>
    const int i = args.indexOf("--scan-one");
    if (i < 0 || i + 3 >= args.size()) return 2;
    const juce::String formatName = args[i + 1].unquoted(), id = args[i + 2].unquoted();
    const juce::File out(args[i + 3].unquoted());
    juce::XmlElement root("PLUGINS");
    if (formatName == kTestFormat) {
        if (id == "crash") std::abort();                         // simulates a plugin crashing the scanner
        if (id == "hang") for (;;) juce::Thread::sleep(1000);    // simulates a plugin that never returns
        juce::PluginDescription d;
        d.name = "Scan Test Plugin";
        d.pluginFormatName = kTestFormat;
        d.fileOrIdentifier = id;
        d.manufacturerName = "Ynot";
        d.uniqueId = 7;
        root.addChildElement(d.createXml().release());
        return root.writeTo(out) ? 0 : 3;
    }
    juce::AudioPluginFormatManager fm;
    addDesktopFormats(fm);
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName) {
            juce::OwnedArray<juce::PluginDescription> found;
            f->findAllTypesForFile(found, id);
            for (auto* d : found) root.addChildElement(d->createXml().release());
            return root.writeTo(out) ? 0 : 3;
        }
    return 4;  // unknown format
}

JucePluginHost::ScanOutcome JucePluginHost::scanOneOutOfProcess(const juce::File& exe, const juce::String& formatName,
                                                                const juce::String& id, int timeoutMs,
                                                                juce::String& detail) {
    auto out = juce::File::createTempFile(".xml");
    juce::ChildProcess child;
    juce::StringArray cmd{exe.getFullPathName(), "--scan-one", formatName, id, out.getFullPathName()};
    if (!child.start(cmd, 0)) {
        detail = "could not start the scanner";
        return ScanOutcome::Crashed;
    }
    if (!child.waitForProcessToFinish(timeoutMs)) {
        child.kill();
        detail = "timed out after " + juce::String(timeoutMs / 1000) + " s";
        out.deleteFile();
        return ScanOutcome::TimedOut;
    }
    const auto code = child.getExitCode();
    auto xml = juce::XmlDocument::parse(out);
    out.deleteFile();
    if (code != 0 || xml == nullptr) {
        detail = "crashed while loading (exit " + juce::String(static_cast<int>(code)) + ")";
        return ScanOutcome::Crashed;
    }
    int n = 0;
    for (auto* e : xml->getChildIterator()) {
        juce::PluginDescription d;
        if (d.loadFromXml(*e)) {
            known_.addType(d);
            ++n;
        }
    }
    return n > 0 ? ScanOutcome::Found : ScanOutcome::NoPlugin;
}

JucePluginHost::ScanReport JucePluginHost::scanOutOfProcess(const juce::File& exe,
                                                            std::function<void(int, int, const juce::String&)> progress,
                                                            std::function<bool()> shouldCancel) {
    ScanReport report;
    struct Item { juce::AudioPluginFormat* format; juce::String id; };
    std::vector<Item> todo;
    for (auto* f : formats_.getFormats())
        for (auto& id : f->searchPathsForPlugins(f->getDefaultLocationsToSearch(), true, false)) {
            if (known_.getBlacklistedFiles().contains(id)) continue;  // failed before: skip
            bool knownAlready = false;
            for (const auto& t : known_.getTypes()) knownAlready |= t.fileOrIdentifier == id;
            if (knownAlready) { ++report.alreadyKnown; continue; }
            todo.push_back({f, id});
        }
    for (size_t i = 0; i < todo.size(); ++i) {
        if (shouldCancel && shouldCancel()) break;
        const auto& item = todo[i];
        const auto shortName = juce::File::createFileWithoutCheckingPath(item.id).getFileName();
        if (progress) progress(static_cast<int>(i), static_cast<int>(todo.size()), shortName.isEmpty() ? item.id : shortName);
        juce::String detail;
        switch (scanOneOutOfProcess(exe, item.format->getName(), item.id, 60000, detail)) {
            case ScanOutcome::Found: ++report.found; break;
            case ScanOutcome::NoPlugin: break;
            case ScanOutcome::Crashed:
            case ScanOutcome::TimedOut:
                known_.addToBlacklist(item.id);  // don't retry it on every scan
                report.failed.push_back((shortName.isEmpty() ? item.id : shortName).toStdString() + ": " +
                                        detail.toStdString());
                break;
        }
    }
    if (progress) progress(static_cast<int>(todo.size()), static_cast<int>(todo.size()), {});
    return report;
}

void JucePluginHost::loadCache(const juce::File& f) {
    if (auto xml = juce::XmlDocument::parse(f)) known_.recreateFromXml(*xml);
}

void JucePluginHost::saveCache(const juce::File& f) const {
    if (auto xml = known_.createXml()) {
        f.getParentDirectory().createDirectory();
        xml->writeTo(f);
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
