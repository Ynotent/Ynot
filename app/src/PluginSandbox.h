// Out-of-process plugin processing ("sandboxing").
//
// A third-party plugin runs inside a helper process (this executable started with
// --plugin-sandbox). Audio and MIDI go through shared memory; a named semaphore wakes
// the helper, so idle sandboxes use no CPU. The host's audio thread never blocks
// indefinitely: it waits at most a fraction of the block duration for the result, and
// if the helper is late (or has died) that plugin is skipped for the block — effects
// pass audio through, instruments go silent — while the rest of the mix continues.
// A crashed helper is detected, reported and the plugin stays bypassed; Ynot keeps
// running. Processing happens inside the same block, so no latency is added.
//
// Desktop only (macOS, Windows, Linux dev host). Mobile plugin formats (AUv3) are
// already sandboxed by the OS.
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <string>

#include "ynot/engine.hpp"

namespace ynotapp {

namespace sandbox {

constexpr uint32_t kMagic = 0x594E5342;  // "YNSB"
constexpr uint32_t kVersion = 2;
constexpr int kMaxBlock = 2048;
constexpr int kMaxMidi = 512;
constexpr int kMaxParams = 64;

struct ParamChange {
    char id[56];
    float value;
};

// Lives in shared memory. Only lock-free atomics and plain data.
struct Shared {
    uint32_t magic;
    uint32_t version;
    std::atomic<uint64_t> requestSeq;  // host: block number submitted
    std::atomic<uint64_t> doneSeq;     // helper: block number finished
    std::atomic<uint32_t> ready;       // helper: plugin loaded
    std::atomic<uint32_t> failed;      // helper: plugin couldn't be loaded
    std::atomic<uint32_t> quit;        // host: please exit
    std::atomic<uint64_t> heartbeat;   // host: increments while alive (helper exits if it stops)
    std::atomic<uint32_t> stateRequest, stateDone;  // host asks for the plugin's state
    std::atomic<uint32_t> resetRequest;              // host asks for reset (seek/rewind)
    std::atomic<uint32_t> editorRequest;             // host asks to show the plugin's window
    std::atomic<uint64_t> paramWrite, paramRead;
    ParamChange params[kMaxParams];
    int32_t isInstrument;
    int32_t frames;
    int32_t numMidi;
    float in[2][kMaxBlock];
    float out[2][kMaxBlock];
    ynot::MidiEvent midi[kMaxMidi];
};

}  // namespace sandbox

// Host side: a ynot::PluginProcessor whose DSP runs in a helper process.
class SandboxedProcessor : public ynot::PluginProcessor {
public:
    struct Spec {
        juce::File executable;       // the Ynot binary (helper)
        juce::String formatName;     // "VST3", "AudioUnit", "Builtin", or test formats
        juce::String identifier;     // plugin file/identifier (or builtin uid)
        std::string uid;             // portable uid, picks the right plugin inside multi-plugin files
        juce::MemoryBlock state;     // initial state (or JSON params for builtins)
        double sampleRate = 48000;
        int maxBlock = 512;
        bool isInstrument = false;
        juce::String displayName;
    };

    // Starts the helper and waits (message thread) up to `loadTimeoutMs` for it to load the
    // plugin. Check ok() afterwards.
    explicit SandboxedProcessor(const Spec& spec, int loadTimeoutMs = 30000);
    ~SandboxedProcessor() override;

    bool ok() const { return ok_; }
    juce::String error() const { return error_; }

    // ynot::PluginProcessor (audio thread)
    void process(float* left, float* right, int frames, const ynot::MidiEvent* events, int numEvents) override;
    void setParameter(const std::string& id, double normalizedValue) override;
    void reset() override;

    // Message thread.
    bool getState(juce::MemoryBlock& out, int timeoutMs = 2000);
    // Shows the plugin's own window, opened by the helper process (so a crash in the
    // plugin's UI can't take Ynot down either).
    void showEditor();
    bool pollCrashed();  // true once, when the helper has died unexpectedly
    bool isDead() const { return dead_.load(); }
    const juce::String& displayName() const { return spec_.displayName; }

    // Diagnostics.
    int64_t lateBlocks() const { return late_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Spec spec_;
    bool ok_ = false;
    juce::String error_;
    std::atomic<bool> dead_{false};
    bool crashReported_ = false;
    std::atomic<int64_t> late_{0};
    uint64_t seq_ = 0;
};

// Helper-process entry point (--plugin-sandbox ...). Runs until told to quit, the host
// disappears, or the plugin crashes the process.
int runSandboxChild(const juce::StringArray& args);
void shutdownSandboxChild();

}  // namespace ynotapp
