// Tests for the JUCE <-> ynot_core adapters. Runs headless on any desktop host.
#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>

#include "../src/JuceAudioDevice.h"
#include "../src/JucePluginHost.h"
#include "ynot/ynot.hpp"

using namespace ynotapp;

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    failures += ok ? 0 : 1;
}

// A minimal "third-party" instrument: square wave while a note is held, with
// one parameter and saveable state, standing in for a real VST3/AU.
class FakeInstrument : public juce::AudioProcessor {
public:
    FakeInstrument() : AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)) {
        addParameter(level_ = new juce::AudioParameterFloat({"level", 1}, "Level", 0.f, 1.f, 0.5f));
    }
    const juce::String getName() const override { return "Fake"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override {
        for (const auto meta : m) {
            auto msg = meta.getMessage();
            if (msg.isNoteOn()) on_ = true;
            if (msg.isNoteOff()) on_ = false;
        }
        for (int i = 0; i < b.getNumSamples(); ++i) {
            float v = on_ ? ((phase_++ / 50) % 2 ? *level_ : -*level_) : 0.f;
            for (int c = 0; c < b.getNumChannels(); ++c) b.setSample(c, i, v);
        }
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& mb) override { mb.replaceAll(&stateBytes_, 4); }
    void setStateInformation(const void* d, int n) override { if (n == 4) std::memcpy(&stateBytes_, d, 4); }
    float level() const { return level_->get(); }
    uint32_t stateBytes_ = 0xA5A5F00D;

private:
    juce::AudioParameterFloat* level_;
    bool on_ = false;
    int phase_ = 0;
};
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    {   // Adapter: MIDI in, audio out, parameters by id
        auto fake = std::make_unique<FakeInstrument>();
        auto* raw = fake.get();
        JuceProcessorAdapter a(std::move(fake), 48000, 256);
        std::vector<float> l(256, 0.f), r(256, 0.f);
        ynot::MidiEvent on{10, true, 60, 100};
        a.process(l.data(), r.data(), 256, &on, 1);
        float peak = 0;
        for (float s : l) peak = std::max(peak, std::abs(s));
        expect(peak > 0.4f, "adapter delivers MIDI and returns instrument audio");
        a.setParameter("level", 0.25);
        expect(std::abs(raw->level() - 0.25f) < 1e-6f, "adapter sets parameters by stable id");
    }

    {   // State encoding is standard base64 and round-trips exactly
        juce::MemoryBlock mb("\x00\x01\x02\xff plugin state", 17);
        auto b64 = encodeState(mb);
        expect(decodeState(b64) == mb, "plugin state round-trips through base64");
        expect(encodeState(juce::MemoryBlock("Man", 3)) == "TWFu", "base64 is RFC 4648 (portable)");
    }

    {   // Portable ids don't depend on install paths
        juce::PluginDescription a, b;
        a.pluginFormatName = b.pluginFormatName = "VST3";
        a.name = b.name = "ProVerb";
        a.manufacturerName = b.manufacturerName = "Acme Audio";
        a.uniqueId = b.uniqueId = 0x1234abcd;
        a.fileOrIdentifier = "C:\\Program Files\\Common Files\\VST3\\ProVerb.vst3";
        b.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/ProVerb.vst3";
        expect(portableUid(a) == portableUid(b), "plugin uid is identical on Windows and macOS installs");
        expect(ynotFormatFor(a) == ynot::PluginFormat::VST3, "VST3 format maps to the core");
    }

    {   // Host honours the platform profile and loads builtins
        JucePluginHost host;
        auto synth = ynot::makeBuiltin(ynot::kBuiltinSynth, "Ynot Synth");
        expect(host.isAvailable(synth) && host.load(synth, 48000, 256) != nullptr, "builtins load through the JUCE host");
        juce::PluginDescription vst;
        vst.pluginFormatName = "VST3";
        vst.name = "ProVerb";
        vst.manufacturerName = "Acme Audio";
        vst.uniqueId = 42;
        host.addKnownPlugin(vst);
        auto inst = host.makeInstance(vst);
        bool desktop = ynot::currentPlatform() == ynot::Platform::MacOS || ynot::currentPlatform() == ynot::Platform::Windows;
        expect(inst && host.isAvailable(*inst) == desktop, "VST3 availability follows the platform profile");
        expect((host.catalog().installed.size() == 1) == desktop, "device catalog lists only loadable plugins");
    }

    {   // Virtual/loopback outputs are flagged; real hardware isn't.
        using D = JuceAudioDevice;
        expect(D::isLikelyVirtualDevice("BlackHole 16ch") && D::isLikelyVirtualDevice("Pro Tools Audio Bridge 2-A") &&
                   D::isLikelyVirtualDevice("Microsoft Teams Audio") && D::isLikelyVirtualDevice("ZoomAudioDevice"),
               "virtual output devices are recognised");
        expect(!D::isLikelyVirtualDevice("MacBook Pro Speakers") && !D::isLikelyVirtualDevice("Scarlett 2i2 USB") &&
                   !D::isLikelyVirtualDevice("External Headphones"),
               "real speakers and interfaces are not flagged");
    }

    std::cout << (failures ? "APP TESTS FAILED" : "app tests passed") << "\n";
    return failures ? 1 : 0;
}
