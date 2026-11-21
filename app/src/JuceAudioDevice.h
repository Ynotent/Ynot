// ynot::AudioDevice implemented with juce::AudioDeviceManager.
//
// One implementation gives the shared engine each platform's native audio API:
//   macOS: CoreAudio · Windows: ASIO (if the SDK is enabled) and WASAPI ·
//   iOS: AVAudioSession/RemoteIO · Android: Oboe/AAudio · Linux (dev): ALSA/JACK
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "ynot/audio_device.hpp"

namespace ynotapp {

class JuceAudioDevice : public ynot::AudioDevice,
                        private juce::AudioIODeviceCallback,
                        private juce::ChangeListener {
public:
    JuceAudioDevice();
    ~JuceAudioDevice() override;

    std::vector<ynot::AudioDeviceInfo> enumerate() override;
    // deviceId is "<driver type>/<device name>", e.g. "ASIO/Focusrite USB ASIO".
    // An empty id opens the system default device.
    bool open(const std::string& deviceId, const ynot::AudioStreamConfig& cfg, ynot::AudioCallback cb) override;
    void start() override;
    void stop() override;
    void close() override;
    int inputLatencyFrames() const override;
    int outputLatencyFrames() const override;

    juce::AudioDeviceManager& manager() { return manager_; }
    const juce::AudioDeviceManager& manager() const { return manager_; }
    std::string lastError() const { return lastError_; }
    double currentSampleRate() const;
    int currentBufferSize() const;

    static ynot::AudioApi apiForTypeName(const juce::String& typeName);

    // Where the chosen device is remembered on this machine. Set before open().
    void setStateFile(const juce::File& f) { stateFile_ = f; }
    // Name of the output device if it's a virtual/loopback device the user likely can't hear.
    std::string virtualDeviceWarning() const;
    static bool isLikelyVirtualDevice(const juce::String& deviceName);

private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    juce::AudioDeviceManager manager_;
    juce::File stateFile_;
    ynot::AudioCallback cb_;
    std::atomic<bool> running_{false};
    std::string lastError_;
};

}  // namespace ynotapp
