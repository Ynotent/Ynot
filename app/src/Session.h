// The open project on this device: model + media + plugin host + live engine.
//
// Edits happen on the message thread. After an edit, rebuild() prepares a new
// ynot::Engine off the audio thread and swaps it in atomically; the audio
// callback never locks or allocates.
#pragma once

#include <atomic>
#include <functional>
#include <memory>

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
    void unfreeze(const std::string& trackId);

    // Audio.
    bool startAudio(const std::string& deviceId = {});
    void stopAudio();
    JuceAudioDevice& device() { return device_; }
    void play();
    void stop();
    bool isPlaying() const { return playing_.load(); }
    double positionSeconds() const;

    // Compatibility for a target platform, using what's installed *here* for this device.
    ynot::CompatReport compatibilityFor(ynot::Platform target) const;
    std::vector<ynot::EngineIssue> engineIssues() const { return issues_; }
    std::vector<std::string> tracksToPrepareForMobile() const;

    std::function<void()> onChange;  // UI refresh hook

private:
    void rebuild();
    void audioCallback(float* const* outs, int numOuts, int frames);

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
};

}  // namespace ynotapp
