// Tests for the JUCE <-> ynot_core adapters. Runs headless on any desktop host.
#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>

#include "../src/AudioImporter.h"
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

    {   // WAV in all its common flavours imports correctly (rate kept, exact length, level).
        struct Case { const char* file; double rate; int channels; };
        const Case cases[] = {{"wav-8bit-mono.wav", 44100, 1},     {"wav-16bit-48k.wav", 48000, 2},
                              {"wav-24bit-96k.wav", 96000, 2},     {"wav-24bit-192k.wav", 192000, 2},
                              {"wav-32bit-int.wav", 44100, 2},     {"wav-32bit-float.wav", 48000, 2},
                              {"wav-5.1-surround.wav", 48000, 2},  // surround -> front pair
                              {"wav-rf64.wav", 48000, 2}};
        const juce::File dir(YNOT_FIXTURE_DIR);
        bool all = true;
        for (const auto& c : cases) {
            auto d = decodeAudioFile(dir.getChildFile(c.file));
            float peak = 0;
            for (float v : d.audio.samples) peak = std::max(peak, std::abs(v));
            const auto expectedFrames = static_cast<int64_t>(c.rate * 0.1);
            // The fixtures' tone: mono peaks at 1/8 full scale; ffmpeg made the stereo files at -3 dB
            // (0.088, measured with ffmpeg astats); the 5.1 file has the tone only in the centre
            // channel, which the downmix puts in both sides at -3 dB (also 0.088).
            const float want = c.channels == 1 ? 0.125f : 0.0884f;
            const bool good = d.ok && std::abs(d.audio.sampleRate - c.rate) < 1 && d.audio.channels == c.channels &&
                              d.audio.frames() == expectedFrames && std::abs(peak - want) < 0.004f;
            std::cout << "  info " << c.file << ": "
                      << (good ? juce::String("ok (") + juce::String(d.audio.frames()) + " frames, peak " +
                                     juce::String(peak, 3) + ")"
                               : "FAILED " + d.error + " rate " + juce::String(d.audio.sampleRate) + " ch " +
                                     juce::String(d.audio.channels) + " frames " + juce::String(d.audio.frames()) +
                                     " peak " + juce::String(peak, 3))
                      << "\n";
            all &= good;
        }
        expect(all, "WAV: 8/16/24/32-bit, float, 44.1-192 kHz, mono, 5.1 and RF64 all import correctly");
    }

    {   // Damaged files must be refused, never imported as noise.
        juce::Random rng(1234);
        for (const char* ext : {".mp3", ".wav", ".flac", ".m4a", ".wmv"}) {
            auto f = juce::File::createTempFile(ext);
            juce::MemoryBlock junk(40000);
            for (size_t i = 0; i < junk.getSize(); ++i) junk[i] = static_cast<char>(rng.nextInt(256));
            f.replaceWithData(junk.getData(), junk.getSize());
            auto d = decodeAudioFile(f);
            f.deleteFile();
            std::cout << "  info random bytes as " << ext << ": "
                      << (d.ok ? "DECODED " + juce::String(d.audio.frames()) + " frames" : juce::String("refused")) << "\n";
            expect(!d.ok, (std::string("damaged ") + ext + " file is refused, not imported as noise").c_str());
        }
    }

    std::cout << (failures ? "APP TESTS FAILED" : "app tests passed") << "\n";
    return failures ? 1 : 0;
}
