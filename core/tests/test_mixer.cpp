#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
// One instrument track holding a long note, so there's steady signal to measure.
Project oneTone() {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    Track t;
    t.id = "k";
    t.kind = TrackKind::Instrument;
    t.instrument = makeBuiltin(kBuiltinSynth, "Synth");
    t.midiClips = {{"m", 0, 8 * kTicksPerQuarter, {{0, 8 * kTicksPerQuarter, 69, 100}}}};
    p.tracks = {t};
    return p;
}

float blockPeak(Engine& e, int blocks = 20) {
    std::vector<float> l(512), r(512);
    float peak = 0;
    for (int b = 0; b < blocks; ++b) {
        e.process(l.data(), r.data(), 512);
        for (float v : l) peak = std::max(peak, std::abs(v));
    }
    return peak;
}
}  // namespace

TEST("mixer: fader, pan and mute change the sound immediately without a rebuild") {
    Project p = oneTone();
    BuiltinPluginHost host;
    PackageMediaProvider media(th::tempDir("mixer"));
    Engine e;
    EngineOptions o;
    e.prepare(p, media, host, o);
    CHECK_EQ(e.numTracks(), 1);
    CHECK_EQ(e.trackIndex("k"), 0);
    CHECK_EQ(e.trackIndex("nope"), -1);
    const float full = blockPeak(e);
    CHECK(full > 0.05f);
    e.setTrackMix(0, -6.0206, 0.0, false);  // -6 dB = half
    CHECK_NEAR(blockPeak(e), full * 0.5f, full * 0.02f);
    e.setTrackMix(0, 0.0, 1.0, false);  // hard right: nothing on the left
    CHECK(blockPeak(e) < 1e-6f);
    e.setTrackMix(0, 0.0, 0.0, true);  // muted
    CHECK(blockPeak(e) < 1e-6f);
    e.setTrackMix(0, 0.0, 0.0, false);
    CHECK_NEAR(blockPeak(e), full, full * 0.02f);
    e.setMasterGain(-6.0206);
    CHECK_NEAR(blockPeak(e), full * 0.5f, full * 0.02f);
}

TEST("mixer: meters report each track's and the master's peak since the last read") {
    Project p = oneTone();
    p.masterGainDb = -6.0206;  // the project's master fader is applied on prepare
    BuiltinPluginHost host;
    PackageMediaProvider media(th::tempDir("meters"));
    Engine e;
    e.prepare(p, media, host, EngineOptions{});
    const float out = blockPeak(e);
    const float track = e.takePeak(0, 0), master = e.takePeak(-1, 0);
    CHECK(track > 0.05f);
    CHECK_NEAR(master, track * 0.5f, track * 0.02f);  // master fader after the track
    CHECK_NEAR(master, out, 1e-6f);
    CHECK_EQ(e.takePeak(0, 0), 0.f);  // reset by reading
    CHECK_EQ(e.takePeak(5, 0), 0.f);  // out of range is harmless
    e.setTrackMix(0, 0.0, 0.0, true);
    blockPeak(e);
    CHECK_EQ(e.takePeak(0, 0), 0.f);  // muted tracks read silent

    auto dir = th::tempDir("master-save");
    p.name = "m";
    saveProjectPackage(p, dir);
    CHECK_NEAR(loadProjectPackage(dir).masterGainDb, -6.0206, 1e-9);
}
