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
    // Restore the device this user last chose on this machine (device-local, never synced).
    std::unique_ptr<juce::XmlElement> saved;
    if (deviceId.empty() && stateFile_.existsAsFile()) saved = juce::XmlDocument::parse(stateFile_);
    juce::String err = manager_.initialise(numIns, cfg.outputChannels, saved.get(), true);
    const bool restored = saved != nullptr && err.isEmpty();
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
    if (err.isEmpty() && !restored) {
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
    manager_.addChangeListener(this);  // save future changes made in Audio settings
    return true;
}

void JuceAudioDevice::changeListenerCallback(juce::ChangeBroadcaster*) {
    if (stateFile_ != juce::File())
        if (auto xml = manager_.createStateXml()) {
            stateFile_.getParentDirectory().createDirectory();
            xml->writeTo(stateFile_);
        }
    if (onDeviceChanged) onDeviceChanged();
}

juce::String JuceAudioDevice::rateLabel(double rate) {
    const double k = rate / 1000.0;
    return juce::String(k, std::abs(k - std::round(k)) < 1e-6 ? 0 : 1) + " kHz";  // "48 kHz", "44.1 kHz"
}

std::vector<double> JuceAudioDevice::availableSampleRates() const {
    std::vector<double> out;
    if (auto* d = manager_.getCurrentAudioDevice())
        for (double r : d->getAvailableSampleRates()) out.push_back(r);
    return out;
}

juce::String JuceAudioDevice::setSampleRate(double rate) {
    auto* d = manager_.getCurrentAudioDevice();
    if (d == nullptr) return "No audio device is open.";
    const auto name = d->getName();  // the device object may be replaced below
    if (!d->getAvailableSampleRates().contains(rate)) return name + " doesn't support " + rateLabel(rate) + ".";
    auto setup = manager_.getAudioDeviceSetup();
    if (std::abs(setup.sampleRate - rate) < 0.5) return {};
    setup.sampleRate = rate;
    auto err = manager_.setAudioDeviceSetup(setup, true);
    if (err.isEmpty() && std::abs(currentSampleRate() - rate) > 0.5)
        err = name + " stayed at " + rateLabel(currentSampleRate()) + ".";
    return err;
}

std::string JuceAudioDevice::virtualDeviceWarning() const {
    auto* d = manager_.getCurrentAudioDevice();
    if (!d) return {};
    const auto out = manager_.getAudioDeviceSetup().outputDeviceName;
    return isLikelyVirtualDevice(out) ? out.toStdString() : std::string();
}

bool JuceAudioDevice::isLikelyVirtualDevice(const juce::String& name) {
    // Loopback/routing/conferencing drivers: audio sent here is usually not heard.
    for (const char* v : {"BlackHole", "Soundflower", "Loopback", "Audio Bridge", "Teams Audio", "ZoomAudio",
                          "Zoom Audio", "VB-Audio", "VB-Cable", "CABLE Input", "Background Music", "Krisp",
                          "Screen Record", "QuickTime"})
        if (name.containsIgnoreCase(v)) return true;
    return false;
}

void JuceAudioDevice::start() { running_ = true; }
void JuceAudioDevice::stop() { running_ = false; }

void JuceAudioDevice::close() {
    running_ = false;
    manager_.removeChangeListener(this);
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
