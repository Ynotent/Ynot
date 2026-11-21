#include "ynot/audio_device.hpp"

namespace ynot {

std::vector<AudioDeviceInfo> NullAudioDevice::enumerate() {
    return {{"null", "Null Device (offline)", AudioApi::Null, 2, 2, {44100, 48000, 96000}, {64, 128, 256, 512, 1024}, false}};
}

bool NullAudioDevice::open(const std::string& deviceId, const AudioStreamConfig& cfg, AudioCallback cb) {
    if (deviceId != "null" || cfg.bufferSize <= 0) return false;
    cfg_ = cfg;
    cb_ = std::move(cb);
    in_.assign(cfg.inputChannels.size(), std::vector<float>(static_cast<size_t>(cfg.bufferSize), 0.f));
    out_.assign(static_cast<size_t>(cfg.outputChannels), std::vector<float>(static_cast<size_t>(cfg.bufferSize), 0.f));
    return true;
}

void NullAudioDevice::pump(int blocks) {
    if (!running_ || !cb_) return;
    std::vector<const float*> ins;
    std::vector<float*> outs;
    for (auto& c : in_) ins.push_back(c.data());
    for (auto& c : out_) outs.push_back(c.data());
    for (int b = 0; b < blocks; ++b)
        cb_(ins.data(), static_cast<int>(ins.size()), outs.data(), static_cast<int>(outs.size()), cfg_.bufferSize);
}

}  // namespace ynot
