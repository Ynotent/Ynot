#pragma once

#include <cmath>
#include <filesystem>
#include <string>

#include "ynot/ynot.hpp"

namespace th {

// Fresh temp directory per call.
inline std::string tempDir(const std::string& name) {
    auto d = std::filesystem::temp_directory_path() / ("ynot-test-" + name + "-" + ynot::newId().substr(0, 8));
    std::filesystem::create_directories(d);
    return d.string();
}

inline double rms(const ynot::AudioBuffer& b, int64_t fromFrame = 0, int64_t toFrame = -1) {
    if (toFrame < 0) toFrame = b.frames();
    double s = 0;
    int64_t n = 0;
    for (int64_t f = fromFrame; f < toFrame; ++f)
        for (int c = 0; c < b.channels; ++c) {
            double v = b.samples[static_cast<size_t>(f * b.channels + c)];
            s += v * v;
            ++n;
        }
    return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

// Simulates a desktop host (macOS/Windows) where the demo's third-party
// plugins are installed: the "instrument" emits a square tone per note, the
// "reverb" is a gentle gain. Builtins are delegated to the real builtin host.
class FakeDesktopHost : public ynot::PluginHost {
public:
    explicit FakeDesktopHost(ynot::Platform p) : platform_(p) {}
    bool isAvailable(const ynot::PluginInstance& p) const override {
        if (p.format == ynot::PluginFormat::Builtin) return builtin_.isAvailable(p);
        return ynot::platformLoadsFormat(platform_, p.format) && p.vendor == "Acme Audio";
    }
    std::unique_ptr<ynot::PluginProcessor> load(const ynot::PluginInstance& p, double sr, int mb) override {
        if (p.format == ynot::PluginFormat::Builtin) return builtin_.load(p, sr, mb);
        if (!isAvailable(p)) return nullptr;
        if (p.format == ynot::PluginFormat::AudioUnit) return std::make_unique<Square>(sr);
        return std::make_unique<Half>();
    }

private:
    struct Square : ynot::PluginProcessor {
        explicit Square(double sr) : sr(sr) {}
        double sr, phase = 0, inc = 0;
        bool on = false;
        void process(float* l, float* r, int n, const ynot::MidiEvent* ev, int ne) override {
            int e = 0;
            for (int i = 0; i < n; ++i) {
                while (e < ne && ev[e].frameOffset <= i) {
                    on = ev[e].noteOn;
                    inc = 440.0 * std::pow(2.0, (ev[e].pitch - 69) / 12.0) / sr;
                    ++e;
                }
                if (!on) continue;
                phase += inc;
                if (phase >= 1) phase -= 1;
                float v = phase < 0.5 ? 0.2f : -0.2f;
                l[i] += v;
                r[i] += v;
            }
        }
        void setParameter(const std::string&, double) override {}
        void reset() override { on = false; phase = 0; }
    };
    struct Half : ynot::PluginProcessor {
        void process(float* l, float* r, int n, const ynot::MidiEvent*, int) override {
            for (int i = 0; i < n; ++i) { l[i] *= 0.5f; r[i] *= 0.5f; }
        }
        void setParameter(const std::string&, double) override {}
        void reset() override {}
    };
    ynot::Platform platform_;
    ynot::BuiltinPluginHost builtin_;
};

inline ynot::DeviceCatalog macWithAcme() {
    ynot::DeviceCatalog c{ynot::Platform::MacOS, {}, false};
    c.installed.insert({ynot::PluginFormat::VST3, "ACME-PROVERB-0001"});
    c.installed.insert({ynot::PluginFormat::AudioUnit, "aumu:Orch:Acme"});
    return c;
}

}  // namespace th
