#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
int64_t zeroCrossings(const AudioBuffer& b, int64_t from, int64_t to) {
    int64_t n = 0;
    for (int64_t f = from + 1; f < to; ++f) {
        float a = b.samples[static_cast<size_t>((f - 1) * b.channels)];
        float c = b.samples[static_cast<size_t>(f * b.channels)];
        n += (a < 0) != (c < 0);
    }
    return n;
}
}  // namespace

TEST("engine: renders demo with only builtins and reports what's missing") {
    auto dir = th::tempDir("render");
    Project p = makeDemoProject(dir);
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    std::vector<EngineIssue> issues;
    AudioBuffer out = renderProject(p, media, host, 48000, 1.0, &issues);
    // 8 beats @120 = 4 s + 1 s tail
    CHECK_EQ(out.frames(), int64_t(5 * 48000));
    CHECK(th::rms(out) > 0.01);
    bool sawReverb = false, sawOrchestra = false;
    for (auto& i : issues) {
        sawReverb |= i.message.find("Acme ProVerb") != std::string::npos;
        sawOrchestra |= i.message.find("Acme Orchestra") != std::string::npos;
    }
    CHECK(sawReverb);
    CHECK(sawOrchestra);
}

TEST("engine: audio recorded at 44.1k plays at correct pitch at 48k and 96k") {
    auto dir = th::tempDir("sr");
    Project p = makeDemoProject(dir);
    p.tracks[1].mute = true;
    p.tracks[2].mute = true;
    p.tracks[0].automation.clear();
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    for (double sr : {48000.0, 96000.0}) {
        AudioBuffer out = renderProject(p, media, host, sr, 0.0);
        // 1 second window in the middle of the 2 s take: 220 Hz -> ~440 crossings.
        auto zc = zeroCrossings(out, static_cast<int64_t>(0.5 * sr), static_cast<int64_t>(1.5 * sr));
        CHECK(zc >= 436 && zc <= 444);
    }
}

TEST("engine: mute, solo and constant-power pan") {
    auto dir = th::tempDir("mix");
    Project p = makeDemoProject(dir);
    p.tracks[0].automation.clear();
    p.tracks[0].inserts.clear();
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;

    p.tracks[1].solo = true;  // keys only
    AudioBuffer soloKeys = renderProject(p, media, host, 48000, 0.0);
    p.tracks[1].solo = false;
    p.tracks[1].mute = true;
    p.tracks[2].mute = true;
    AudioBuffer voxOnly = renderProject(p, media, host, 48000, 0.0);
    CHECK(th::rms(soloKeys) > 0.01);
    CHECK(th::rms(voxOnly) > 0.01);
    // centre pan is unity: vocal take peak amplitude 0.3 survives intact
    CHECK_NEAR(th::rms(voxOnly, 12000, 60000), 0.3 / std::sqrt(2.0), 0.01);

    p.tracks[0].pan = -1.0;
    AudioBuffer hardLeft = renderProject(p, media, host, 48000, 0.0);
    double right = 0;
    for (int64_t f = 0; f < hardLeft.frames(); ++f) right += std::fabs(hardLeft.samples[static_cast<size_t>(f * 2 + 1)]);
    CHECK(right < 1e-3);
}

TEST("engine: gain automation is applied") {
    auto dir = th::tempDir("auto");
    Project p = makeDemoProject(dir);
    p.tracks[1].mute = p.tracks[2].mute = true;
    p.tracks[0].inserts.clear();
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    AudioBuffer out = renderProject(p, media, host, 48000, 0.0);
    double early = th::rms(out, 0, 4800);        // ~ -6 dB
    double late = th::rms(out, 86400, 91200);    // ~ 0 dB near 1.8-1.9 s
    CHECK(late / early > 1.6);
}

TEST("engine: drives from a NullAudioDevice callback block by block") {
    auto dir = th::tempDir("device");
    Project p = makeDemoProject(dir);
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    Engine engine;
    EngineOptions o;
    o.sampleRate = 48000;
    o.maxBlock = 256;
    engine.prepare(p, media, host, o);

    NullAudioDevice dev;
    AudioStreamConfig cfg;
    cfg.bufferSize = 256;
    double energy = 0;
    CHECK(dev.open("null", cfg, [&](const float* const*, int, float* const* outs, int, int frames) {
        engine.process(outs[0], outs[1], frames);
        for (int i = 0; i < frames; ++i) energy += outs[0][i] * outs[0][i];
    }));
    dev.start();
    dev.pump(100);
    CHECK_EQ(engine.position(), int64_t(25600));
    CHECK(energy > 0.1);
}
