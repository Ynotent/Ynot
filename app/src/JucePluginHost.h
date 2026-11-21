// ynot::PluginHost implemented with JUCE's plugin hosting.
//
// Loads builtins (via the core) plus whatever formats this platform supports:
//   macOS: AU (v2/v3) + VST3 · Windows: VST3 · iOS: AUv3 · Android: builtins only
// Formats the platform profile says are unsupported are never loaded, even if
// JUCE could (e.g. VST3 on the Linux dev host), so behaviour matches the
// compatibility reports users see.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <functional>
#include <mutex>

#include "ynot/compatibility.hpp"
#include "ynot/engine.hpp"

namespace ynotapp {

class SandboxedProcessor;
// Adds the plugin formats this platform can host (VST3 on desktop, AU on Apple).
void addPluginFormats(juce::AudioPluginFormatManager& fm);

// Plugin ids that are the same on every machine (JUCE's own identifier string
// contains a file-path hash, which differs between computers).
std::string portableUid(const juce::PluginDescription& d);
std::optional<ynot::PluginFormat> ynotFormatFor(const juce::PluginDescription& d);

// Standard base64 (RFC 4648) so plugin state survives any device or service.
std::string encodeState(const juce::MemoryBlock& mb);
juce::MemoryBlock decodeState(const std::string& b64);

// Wraps any juce::AudioProcessor as a realtime-safe ynot::PluginProcessor.
class JuceProcessorAdapter : public ynot::PluginProcessor {
public:
    JuceProcessorAdapter(std::unique_ptr<juce::AudioProcessor> p, double sampleRate, int maxBlock);
    ~JuceProcessorAdapter() override;
    void process(float* left, float* right, int frames, const ynot::MidiEvent* events, int numEvents) override;
    void setParameter(const std::string& id, double normalizedValue) override;
    void reset() override;
    juce::AudioProcessor& processor() { return *proc_; }

private:
    std::unique_ptr<juce::AudioProcessor> proc_;
    juce::AudioBuffer<float> buffer_;
    juce::MidiBuffer midi_;
    std::map<std::string, juce::AudioProcessorParameter*> params_;
};

class JucePluginHost : public ynot::PluginHost {
public:
    JucePluginHost();
    ~JucePluginHost() override;

    std::unique_ptr<ynot::PluginProcessor> load(const ynot::PluginInstance& p, double sampleRate,
                                                int maxBlock) override;
    bool isAvailable(const ynot::PluginInstance& p) const override;

    // ---- Out-of-process plugin scanning ------------------------------------------------
    // Each plugin file is inspected by a child process (this same executable started with
    // --scan-one). A plugin that crashes or hangs only takes down the child: it's recorded
    // as failed and skipped next time, and Ynot keeps running.
    enum class ScanOutcome { Found, NoPlugin, Crashed, TimedOut };
    struct ScanReport {
        int found = 0;
        int alreadyKnown = 0;
        std::vector<std::string> failed;  // "file: reason"
    };
    ScanOutcome scanOneOutOfProcess(const juce::File& executable, const juce::String& formatName,
                                    const juce::String& fileOrIdentifier, int timeoutMs, juce::String& detail);
    // Thread-safe to call from a background thread; progress(done, total, current file).
    ScanReport scanOutOfProcess(const juce::File& executable,
                                std::function<void(int, int, const juce::String&)> progress = {},
                                std::function<bool()> shouldCancel = {});
    // Child-process entry point: scans one file and writes descriptions to an XML file.
    static int runScanChild(const juce::StringArray& args);

    // Remember scan results (and failures) between launches. Device-local, never synced.
    void loadCache(const juce::File& f);
    void saveCache(const juce::File& f) const;
    void addKnownPlugin(const juce::PluginDescription& d) { known_.addType(d); }
    void addFormat(std::unique_ptr<juce::AudioPluginFormat> f) { formats_.addFormat(f.release()); }

    // What's installed here, for analyzeCompatibility().
    ynot::DeviceCatalog catalog() const;
    // Creates a project PluginInstance from a scanned plugin.
    std::optional<ynot::PluginInstance> makeInstance(const juce::PluginDescription& d) const;
    const juce::KnownPluginList& knownPlugins() const { return known_; }

    // Run third-party plugins in sandbox processes (this executable with --plugin-sandbox).
    // On by default on desktop once the executable is set.
    void setSandboxExecutable(const juce::File& exe) { sandboxExecutable_ = exe; }
    void setSandboxEnabled(bool on) { sandboxEnabled_ = on; }
    bool sandboxEnabled() const { return sandboxEnabled_; }
    // Message thread: plugins that crashed (or failed to load in a sandbox) since last call.
    std::vector<juce::String> takeCrashReports();

    // Writes each live plugin's current state back into the project (before save/handoff).
    void captureState(ynot::Project& project) const;

private:
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    ynot::BuiltinPluginHost builtin_;
    // Live plugin instances, shared by successive engines so edits don't reload plugins.
    struct LiveEntry {
        std::string instanceId;
        std::weak_ptr<ynot::PluginProcessor> processor;
        SandboxedProcessor* sandbox = nullptr;  // valid while processor is alive
        std::function<bool(juce::MemoryBlock&)> getState;
    };
    mutable std::mutex liveMutex_;
    std::map<std::string, LiveEntry> live_;
    std::vector<std::weak_ptr<SandboxedProcessor>> sandboxes_;
    std::vector<juce::String> crashReports_;
    juce::File sandboxExecutable_;
    bool sandboxEnabled_ = true;
};

}  // namespace ynotapp
