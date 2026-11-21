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
#include <mutex>

#include "ynot/compatibility.hpp"
#include "ynot/engine.hpp"

namespace ynotapp {

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

    // Scans the platform's default plugin folders. On desktop this is slow and
    // runs off the audio thread; M1 moves it to an out-of-process scanner.
    void scanDefaultLocations(std::function<void(const juce::String&)> progress = {});
    void addKnownPlugin(const juce::PluginDescription& d) { known_.addType(d); }
    void addFormat(std::unique_ptr<juce::AudioPluginFormat> f) { formats_.addFormat(f.release()); }

    // What's installed here, for analyzeCompatibility().
    ynot::DeviceCatalog catalog() const;
    // Creates a project PluginInstance from a scanned plugin.
    std::optional<ynot::PluginInstance> makeInstance(const juce::PluginDescription& d) const;
    const juce::KnownPluginList& knownPlugins() const { return known_; }

    // Bookkeeping for loaded instances (called by the adapters this host creates).
    void registerLive(const std::string& id, JuceProcessorAdapter* a);
    void unregisterLive(const std::string& id, JuceProcessorAdapter* a);

    // Writes each live plugin's current state back into the project (before save/handoff).
    void captureState(ynot::Project& project) const;

private:
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    ynot::BuiltinPluginHost builtin_;
    mutable std::mutex liveMutex_;
    std::multimap<std::string, JuceProcessorAdapter*> live_;  // plugin instance id -> loaded adapter
};

}  // namespace ynotapp
