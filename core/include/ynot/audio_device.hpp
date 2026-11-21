// Audio I/O abstraction.
//
// The shared engine never talks to an OS audio API directly. Each native shell
// implements AudioDevice with its platform-optimized backend:
//
//   macOS    CoreAudio (AUHAL)            apps/macos   — aggregate devices, pro interfaces
//   Windows  ASIO (preferred) / WASAPI    apps/windows — WASAPI exclusive + shared fallback
//   iOS      AVAudioSession + RemoteIO    apps/ios     — route changes, interruptions, AUv3
//   Android  AAudio (via Oboe) / OpenSL   apps/android — low-latency path, USB class-compliant
//
// The NullAudioDevice here drives the engine deterministically for tests and
// offline rendering on any host (including the Linux CI runner).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ynot/platform.hpp"

namespace ynot {

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    AudioApi api = AudioApi::Null;
    int maxInputChannels = 0;
    int maxOutputChannels = 2;
    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;
    bool isExternalInterface = false;  // USB/Thunderbolt/class-compliant
};

struct AudioStreamConfig {
    double sampleRate = 48000;
    int bufferSize = 256;
    std::vector<int> inputChannels;   // physical inputs to open (for recording)
    int outputChannels = 2;
};

// Called on the realtime thread. inputs/outputs are non-interleaved.
using AudioCallback = std::function<void(const float* const* inputs, int numInputs, float* const* outputs,
                                         int numOutputs, int frames)>;

class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    virtual std::vector<AudioDeviceInfo> enumerate() = 0;
    virtual bool open(const std::string& deviceId, const AudioStreamConfig& cfg, AudioCallback cb) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;
    // Round-trip latency in frames as reported by the driver; used for
    // recording latency compensation so takes line up on every platform.
    virtual int inputLatencyFrames() const = 0;
    virtual int outputLatencyFrames() const = 0;
};

class NullAudioDevice : public AudioDevice {
public:
    std::vector<AudioDeviceInfo> enumerate() override;
    bool open(const std::string& deviceId, const AudioStreamConfig& cfg, AudioCallback cb) override;
    void start() override { running_ = true; }
    void stop() override { running_ = false; }
    void close() override { cb_ = nullptr; }
    int inputLatencyFrames() const override { return 0; }
    int outputLatencyFrames() const override { return 0; }
    // Test driver: run `blocks` callbacks synchronously. Inputs are silent.
    void pump(int blocks);
    const std::vector<std::vector<float>>& lastOutput() const { return out_; }

private:
    AudioStreamConfig cfg_;
    AudioCallback cb_;
    bool running_ = false;
    std::vector<std::vector<float>> in_, out_;
};

// Compensates a recorded take for driver latency: returns the number of frames
// to trim from the start so the take aligns with what the performer heard.
inline int recordingOffsetFrames(const AudioDevice& d) {
    return d.inputLatencyFrames() + d.outputLatencyFrames();
}

}  // namespace ynot
