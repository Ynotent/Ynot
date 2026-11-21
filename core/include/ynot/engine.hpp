// Shared audio engine.
//
// The same block-based render path drives live playback (from a platform audio
// device callback), offline bounce, and track freezing, so a mix sounds the
// same everywhere. process() does not allocate or lock; everything is built in
// prepare() on a non-realtime thread.
//
// Platform-specific work is isolated behind two interfaces:
//   * PluginHost  — loads plugins (builtin everywhere; VST3/AU on desktop,
//                   AUv3 on Apple, AAP on Android — implemented in the app shells)
//   * AudioDevice — see audio_device.hpp
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

struct MidiEvent {
    int frameOffset;  // within the current block
    bool noteOn;
    int pitch;
    int velocity;
};

class PluginProcessor {
public:
    virtual ~PluginProcessor() = default;
    // In-place stereo processing. Instruments ignore input and add their output.
    virtual void process(float* left, float* right, int frames, const MidiEvent* events,
                         int numEvents) = 0;
    virtual void setParameter(const std::string& id, double normalizedValue) = 0;
    virtual void reset() = 0;
};

class PluginHost {
public:
    virtual ~PluginHost() = default;
    // Returns nullptr if the plugin cannot be loaded on this device.
    virtual std::unique_ptr<PluginProcessor> load(const PluginInstance& p, double sampleRate,
                                                  int maxBlock) = 0;
    virtual bool isAvailable(const PluginInstance& p) const = 0;
};

// Loads Ynot's builtin processors. Platform hosts wrap this and add their own formats.
class BuiltinPluginHost : public PluginHost {
public:
    std::unique_ptr<PluginProcessor> load(const PluginInstance& p, double sampleRate,
                                          int maxBlock) override;
    bool isAvailable(const PluginInstance& p) const override;
    static std::vector<std::string> builtinIds();
};

// Builtin ids.
inline constexpr const char* kBuiltinSynth = "ynot.synth.basic";
inline constexpr const char* kBuiltinGain = "ynot.fx.gain";
inline constexpr const char* kBuiltinFilter = "ynot.fx.lowpass";

// Supplies decoded media (cached) for the engine.
class MediaProvider {
public:
    virtual ~MediaProvider() = default;
    virtual const AudioBuffer* get(const MediaRef& m) = 0;
};

class PackageMediaProvider : public MediaProvider {
public:
    explicit PackageMediaProvider(std::string packageDir) : dir_(std::move(packageDir)) {}
    const AudioBuffer* get(const MediaRef& m) override;
    void put(const std::string& mediaId, AudioBuffer buf) { cache_[mediaId] = std::move(buf); }

private:
    std::string dir_;
    std::map<std::string, AudioBuffer> cache_;
};

struct EngineIssue {
    std::string trackId;
    std::string message;
};

struct EngineOptions {
    double sampleRate = 48000;
    int maxBlock = 512;
    // When set, render only this track, pre-fader and ignoring mute/solo and
    // its own freeze (used to create freezes).
    std::string isolateTrackPreFader;
};

class Engine {
public:
    Engine();
    ~Engine();
    // Non-realtime. Builds the render graph from a snapshot of the project.
    void prepare(const Project& project, MediaProvider& media, PluginHost& host,
                 const EngineOptions& opts);
    // Realtime-safe. Renders the next `frames` (<= maxBlock) into non-interleaved stereo.
    void process(float* left, float* right, int frames);
    void seek(int64_t frame);
    int64_t position() const { return pos_; }
    int maxBlock() const { return opts_.maxBlock; }
    // Live-only click (never part of bounces or freezes). Realtime-safe toggle.
    void setMetronome(bool on) { metronome_.store(on); }
    bool metronome() const { return metronome_.load(); }
    const std::vector<EngineIssue>& issues() const { return issues_; }

private:
    struct TrackState;
    std::vector<std::unique_ptr<TrackState>> tracks_;
    std::vector<EngineIssue> issues_;
    EngineOptions opts_;
    double secondsPerTick_ = 0;
    int64_t pos_ = 0;
    std::vector<float> scratchL_, scratchR_;
    std::atomic<bool> metronome_{false};
    int beatsPerBar_ = 4;
    int clickRemaining_ = 0;
    double clickPhase_ = 0, clickInc_ = 0;
    void renderClick(float* left, float* right, int frames, int64_t blockStart);
};

// Offline helpers built on Engine.
AudioBuffer renderProject(const Project& p, MediaProvider& media, PluginHost& host,
                          double sampleRate, double tailSeconds = 1.0,
                          std::vector<EngineIssue>* issues = nullptr);
AudioBuffer renderTrackPreFader(const Project& p, const std::string& trackId, MediaProvider& media,
                                PluginHost& host, double sampleRate, double tailSeconds = 1.0);

double dbToGain(double db);

}  // namespace ynot
