// The open project on this device: model + media + plugin host + live engine.
//
// Edits happen on the message thread. After an edit, rebuild() prepares a new
// ynot::Engine off the audio thread and swaps it in atomically; the audio
// callback never locks or allocates.
#pragma once

#include <algorithm>
#include <atomic>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <thread>

#include "JuceAudioDevice.h"
#include "JucePluginHost.h"
#include "ynot/ynot.hpp"

namespace ynotapp {

class Session {
public:
    explicit Session(JucePluginHost& host);
    ~Session();

    // Project lifecycle.
    void newDemo(const juce::File& packageDir);
    void open(const juce::File& packageDir);
    void save();
    bool hasProject() const { return loaded_; }
    const ynot::Project& project() const { return project_; }
    juce::File packageDir() const { return dir_; }

    // Edits (message thread). Each one rebuilds the engine.
    void edit(const std::function<void(ynot::Project&)>& fn);
    void freeze(const std::string& trackId);
    void reload() { rebuild(); }  // re-resolve plugins/media without an undo step
    bool undo();
    bool redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // Playhead (message thread). Seeks are handed to the audio thread lock-free.
    void seekToTick(int64_t tick);
    int64_t positionTick() const;
    // Decoded audio for drawing waveforms (message thread; cached).
    const ynot::AudioBuffer* mediaAudio(const std::string& mediaId);
    void unfreeze(const std::string& trackId);

    // Audio.
    bool startAudio(const std::string& deviceId = {});
    void stopAudio();
    JuceAudioDevice& device() { return device_; }
    void play();
    void stop();
    bool isPlaying() const { return playing_.load(); }
    double positionSeconds() const;

    // Recording (armed audio tracks). Input channel choices are device-local and
    // never saved into the project, because another device has different inputs.
    bool startRecording(std::string& error);
    struct RecordResult {
        std::vector<std::string> newClips;
        int64_t droppedFrames = 0;
        std::string error;
    };
    RecordResult stopRecording();
    bool isRecording() const { return recording_.load(); }
    double recordingSeconds() const;
    int numInputChannels() const;
    std::vector<int> inputFor(const std::string& trackId) const;
    void cycleInput(const std::string& trackId);
    std::string inputLabel(const std::string& trackId) const;

    // Hear armed tracks' inputs through their effects and fader (device-local setting).
    void setMonitoring(bool on);
    bool monitoring() const { return monitoring_.load(); }
    double monitoringLatencyMs() const;  // input + output + buffer, as the driver reports it
    // Bars of click before recording starts (0 = off).
    void setCountInBars(int bars) { countInBars_ = std::clamp(bars, 0, 4); }
    int countInBars() const { return countInBars_; }

    void setMetronome(bool on);
    bool metronome() const { return metronomeOn_.load(); }

    // Device-free operation for smoke tests and CI: declare simulated inputs,
    // then run audio callbacks directly.
    void simulateInputsForTesting(int numInputs) { simulatedInputs_ = numInputs; }
    void processForTesting(const float* const* ins, int numIns, float* const* outs, int numOuts, int frames);

    // Compatibility for a target platform, using what's installed *here* for this device.
    ynot::CompatReport compatibilityFor(ynot::Platform target) const;
    std::vector<ynot::EngineIssue> engineIssues() const { return issues_; }
    std::vector<std::string> tracksToPrepareForMobile() const;

    std::function<void()> onChange;  // UI refresh hook

private:
    void rebuild();
    void audioCallback(const float* const* ins, int numIns, float* const* outs, int numOuts, int frames);

    JucePluginHost& host_;
    JuceAudioDevice device_;
    ynot::Project project_;
    juce::File dir_;
    bool loaded_ = false;
    std::unique_ptr<ynot::PackageMediaProvider> media_;
    std::vector<ynot::EngineIssue> issues_;

    double sampleRate_ = 48000;
    int blockSize_ = 512;
    std::atomic<ynot::Engine*> active_{nullptr};
    std::atomic<bool> inCallback_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int64_t> position_{0};
    std::vector<float> scratchL_, scratchR_;

    ynot::Recorder recorder_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> draining_{false};
    std::thread drainThread_;
    std::map<std::string, std::vector<int>> inputs_;
    std::atomic<bool> metronomeOn_{false};
    std::atomic<bool> monitoring_{false};
    int countInBars_ = 0;
    int64_t forcedClickUntil_ = std::numeric_limits<int64_t>::min();
    ynot::History history_;
    static constexpr int64_t kNoSeek = std::numeric_limits<int64_t>::min();
    std::atomic<int64_t> pendingSeek_{kNoSeek};
    int simulatedInputs_ = 0;
};

}  // namespace ynotapp
