#include "JuceAudioDevice.h"

namespace ynotapp {

JuceAudioDevice::JuceAudioDevice() = default;

JuceAudioDevice::~JuceAudioDevice() { close(); }

ynot::AudioApi JuceAudioDevice::apiForTypeName(const juce::String& t) {
    if (t == "CoreAudio") return ynot::AudioApi::CoreAudio;
    if (t == "ASIO") return ynot::AudioApi::ASIO;
    if (t.startsWith("Windows Audio")) return ynot::AudioApi::WASAPI;  // shared / exclusive / low latency
    if (t == "iOS Audio") return ynot::AudioApi::AVAudioSession;
    if (t == "Android Oboe") return ynot::AudioApi::AAudio;
    if (t == "Android OpenSLES" || t == "Android Audio") return ynot::AudioApi::OpenSLES;
    return ynot::AudioApi::Null;  // ALSA/JACK on the Linux dev host
}

std::vector<ynot::AudioDeviceInfo> JuceAudioDevice::enumerate() {
    std::vector<ynot::AudioDeviceInfo> out;
    for (auto* type : manager_.getAvailableDeviceTypes()) {
        type->scanForDevices();
        for (auto& name : type->getDeviceNames(false)) {
            ynot::AudioDeviceInfo info;
            info.id = (type->getTypeName() + "/" + name).toStdString();
            info.name = name.toStdString();
            info.api = apiForTypeName(type->getTypeName());
            // Opening each device to read its capabilities is slow and can fail on busy devices,
            // so full details are only filled in for the open device (see open()).
            info.maxOutputChannels = 2;
            info.isExternalInterface = info.api == ynot::AudioApi::ASIO ||
                                       name.containsIgnoreCase("usb") || name.containsIgnoreCase("thunderbolt");
            out.push_back(std::move(info));
        }
    }
    return out;
}

bool JuceAudioDevice::open(const std::string& deviceId, const ynot::AudioStreamConfig& cfg, ynot::AudioCallback cb) {
    close();
    const int numIns = static_cast<int>(cfg.inputChannels.size());
    juce::String err = manager_.initialiseWithDefaultDevices(numIns, cfg.outputChannels);
    if (err.isEmpty() && !deviceId.empty()) {
        juce::String id(deviceId);
        auto type = id.upToFirstOccurrenceOf("/", false, false);
        auto name = id.fromFirstOccurrenceOf("/", false, false);
        manager_.setCurrentAudioDeviceType(type, true);
        auto setup = manager_.getAudioDeviceSetup();
        setup.outputDeviceName = name;
        setup.inputDeviceName = numIns > 0 ? name : juce::String();
        err = manager_.setAudioDeviceSetup(setup, true);
    }
    if (err.isEmpty()) {
        auto setup = manager_.getAudioDeviceSetup();
        setup.sampleRate = cfg.sampleRate;
        setup.bufferSize = cfg.bufferSize;
        setup.useDefaultInputChannels = cfg.inputChannels.empty();
        setup.inputChannels.clear();
        for (int ch : cfg.inputChannels) setup.inputChannels.setBit(ch);
        err = manager_.setAudioDeviceSetup(setup, true);  // driver may substitute nearest supported values
    }
    if (err.isEmpty() && manager_.getCurrentAudioDevice() == nullptr) err = "no audio device is available";
    lastError_ = err.toStdString();
    if (err.isNotEmpty()) return false;
    cb_ = std::move(cb);
    manager_.addAudioCallback(this);
    return true;
}

void JuceAudioDevice::start() { running_ = true; }
void JuceAudioDevice::stop() { running_ = false; }

void JuceAudioDevice::close() {
    running_ = false;
    manager_.removeAudioCallback(this);  // blocks until the callback has returned
    manager_.closeAudioDevice();
    cb_ = nullptr;
}

int JuceAudioDevice::inputLatencyFrames() const {
    auto* d = manager_.getCurrentAudioDevice();
    return d ? d->getInputLatencyInSamples() : 0;
}
int JuceAudioDevice::outputLatencyFrames() const {
    auto* d = manager_.getCurrentAudioDevice();
    return d ? d->getOutputLatencyInSamples() : 0;
}
double JuceAudioDevice::currentSampleRate() const {
    auto* d = manager_.getCurrentAudioDevice();
    return d ? d->getCurrentSampleRate() : 0.0;
}
int JuceAudioDevice::currentBufferSize() const {
    auto* d = manager_.getCurrentAudioDevice();
    return d ? d->getCurrentBufferSizeSamples() : 0;
}

void JuceAudioDevice::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out,
                                                       int numOut, int numSamples,
                                                       const juce::AudioIODeviceCallbackContext&) {
    if (!running_ || !cb_) {
        for (int c = 0; c < numOut; ++c)
            if (out[c]) juce::FloatVectorOperations::clear(out[c], numSamples);
        return;
    }
    cb_(in, numIn, out, numOut, numSamples);
}

}  // namespace ynotapp
