#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr int64_t kQ = kTicksPerQuarter;
constexpr int64_t kBar = 4 * kQ;  // 4/4

// 120 BPM for bar 1, then 60 BPM from bar 2.
Project slowsDownAtBar2() {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    p.tempoChanges = {{kBar, 60}};
    return p;
}

// Frames where the signal starts after at least `gap` frames of silence.
std::vector<int64_t> onsets(const AudioBuffer& b, int64_t gap = 2000) {
    std::vector<int64_t> out;
    int64_t silent = gap;
    for (int64_t f = 0; f < b.frames(); ++f) {
        const bool sound = std::abs(b.samples[static_cast<size_t>(f * b.channels)]) > 1e-4f;
        if (sound && silent >= gap) out.push_back(f);
        silent = sound ? 0 : silent + 1;
    }
    return out;
}
}  // namespace

TEST("tempo map: ticks <-> seconds across a tempo change") {
    Project p = slowsDownAtBar2();
    const TempoMap m(p);
    CHECK_NEAR(m.tickToSeconds(0), 0.0, 1e-12);
    CHECK_NEAR(m.tickToSeconds(static_cast<double>(kBar)), 2.0, 1e-12);       // 4 beats at 120
    CHECK_NEAR(m.tickToSeconds(static_cast<double>(kBar + kQ)), 3.0, 1e-12);  // then 1 s per beat
    CHECK_NEAR(m.secondsToTick(2.5), static_cast<double>(kBar + kQ / 2), 1e-9);
    CHECK_NEAR(m.secondsToTick(1.0), static_cast<double>(2 * kQ), 1e-9);
    CHECK_NEAR(p.tempoAt(0), 120.0, 1e-12);
    CHECK_NEAR(p.tempoAt(kBar - 1), 120.0, 1e-12);
    CHECK_NEAR(p.tempoAt(kBar), 60.0, 1e-12);
    for (double t : {0.0, 123.0, 3840.0, 5000.0, 20000.0})
        CHECK_NEAR(m.secondsToTick(m.tickToSeconds(t)), t, 1e-6);
    CHECK_EQ(m.tickToFrame(kBar + kQ, 48000), int64_t(3 * 48000));
    CHECK_EQ(m.frameToTick(3 * 48000, 48000), kBar + kQ);
    // No changes: plain constant tempo.
    Project c;
    c.tempoBpm = 90;
    CHECK_NEAR(TempoMap(c).tickToSeconds(static_cast<double>(kQ)), 60.0 / 90.0, 1e-12);
}

TEST("tempo: the click follows tempo changes") {
    Project p = slowsDownAtBar2();
    p.tracks.push_back([] {
        Track t;  // a track so the song has length
        t.id = "t";
        t.kind = TrackKind::Instrument;
        t.midiClips = {{"m", 0, 3 * kBar, {}}};
        return t;
    }());
    BuiltinPluginHost host;
    PackageMediaProvider media(th::tempDir("click"));
    Engine e;
    EngineOptions o;
    o.sampleRate = 48000;
    e.prepare(p, media, host, o);
    e.setMetronome(true);
    AudioBuffer out;
    out.sampleRate = 48000;
    out.channels = 2;
    std::vector<float> l(512), r(512);
    for (int b = 0; b < 48000 * 5 / 512; ++b) {
        e.process(l.data(), r.data(), 512);
        for (int i = 0; i < 512; ++i) {
            out.samples.push_back(l[static_cast<size_t>(i)]);
            out.samples.push_back(r[static_cast<size_t>(i)]);
        }
    }
    // Beats every 0.5 s in bar 1, every 1 s after the change.
    const std::vector<int64_t> want = {0, 24000, 48000, 72000, 96000, 144000, 192000};
    auto got = onsets(out);
    CHECK(got.size() >= want.size());
    for (size_t i = 0; i < want.size() && i < got.size(); ++i) CHECK(std::abs(got[i] - want[i]) <= 1);
}

TEST("tempo: notes after a tempo change play at the right time") {
    Project p = slowsDownAtBar2();
    Track t;
    t.id = "k";
    t.kind = TrackKind::Instrument;
    t.instrument = makeBuiltin(kBuiltinSynth, "Synth");
    // Beat 2 of bar 2 = 2 s + 1 s.
    t.midiClips = {{"m", 0, 3 * kBar, {{kBar + kQ, kQ / 4, 69, 100}}}};
    p.tracks = {t};
    BuiltinPluginHost host;
    PackageMediaProvider media(th::tempDir("tempo-note"));
    AudioBuffer out = renderProject(p, media, host, 48000, 0.5);
    auto got = onsets(out);
    CHECK(!got.empty());
    CHECK(std::abs(got.front() - 3 * 48000) <= 2);
    // Song length follows the map: bar 1 = 2 s, bars 2-3 = 8 beats at 60 = 8 s, plus the tail.
    CHECK_EQ(out.frames(), int64_t((10.0 + 0.5) * 48000));
}

TEST("tempo edits: audio keeps its real length, clips stay on their bars, undo-able data") {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    MediaRef m;
    m.id = "m";
    m.sampleRate = 48000;
    m.frames = 2 * 48000;  // 2 s of audio
    p.media = {m};
    Track a;
    a.id = "a";
    a.kind = TrackKind::Audio;
    a.audioClips = {{"c", "m", kBar, kBar, 0, 0.0, {}, 0, 1.0, {}, {}, 0}};  // bar 2, 2 s at 120 = 1 bar
    Track k;
    k.id = "k";
    k.kind = TrackKind::Instrument;
    k.midiClips = {{"mc", kBar, kBar, {{0, kQ, 60, 100}}}};
    p.tracks = {a, k};

    setTempo(p, 60);  // half speed: 2 s of audio is now half a bar
    CHECK_NEAR(p.tempoBpm, 60.0, 1e-12);
    CHECK_EQ(p.tracks[0].audioClips[0].startTick, kBar);
    CHECK_EQ(p.tracks[0].audioClips[0].lengthTick, kBar / 2);
    CHECK_EQ(p.tracks[1].midiClips[0].lengthTick, kBar);  // MIDI is musical: unchanged

    // A change in the middle of the clip: first beat at 60, the rest at 240.
    setTempo(p, 120);
    CHECK(setTempoChange(p, kBar + kQ, 240));
    // The clip's first beat runs at 120 (0.5 s); the remaining 1.5 s at 240 BPM is 6 beats.
    CHECK_EQ(p.tracks[0].audioClips[0].lengthTick, kQ + 6 * kQ);
    CHECK(setTempoChange(p, kBar + kQ, 120));  // replace, not add
    CHECK_EQ(p.tempoChanges.size(), size_t(1));
    CHECK_EQ(p.tracks[0].audioClips[0].lengthTick, kBar);
    CHECK(removeTempoChange(p, kBar + kQ));
    CHECK(p.tempoChanges.empty());
    CHECK(!removeTempoChange(p, 123));
    setTempo(p, 5000);
    CHECK_NEAR(p.tempoBpm, kMaxBpm, 1e-12);
}

TEST("tempo: tempo changes are saved and reloaded; old projects load unchanged") {
    auto dir = th::tempDir("tempo-save");
    Project p = slowsDownAtBar2();
    p.name = "Tempo";
    p.tempoChanges.push_back({3 * kBar, 150});
    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK_EQ(q.tempoChanges.size(), size_t(2));
    CHECK_EQ(q.tempoChanges[0].tick, kBar);
    CHECK_NEAR(q.tempoChanges[1].bpm, 150.0, 1e-12);
    CHECK(projectToJson(q) == projectToJson(p));
    CHECK_EQ(p.tempoSignature(), q.tempoSignature());
    Project plain;
    plain.id = "x";
    CHECK(projectToJson(plain).find("tempoChanges") == nullptr);  // nothing new written for constant tempo
}

TEST("tempo: a tempo change makes a track's freeze stale") {
    auto dir = th::tempDir("tempo-freeze");
    Project p = makeDemoProject(dir);
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    std::string vocals;
    for (auto& t : p.tracks)
        if (t.name == "Vocals") vocals = t.id;
    freezeTrack(p, dir, vocals, media, host, Platform::Linux, 48000);
    CHECK(hasFreshFreeze(p, *p.findTrack(vocals)));
    setTempoChange(p, kBar, 100);
    CHECK(!hasFreshFreeze(p, *p.findTrack(vocals)));
}

TEST("tempo: recording and stretch helpers measure from the clip's position") {
    Project p = slowsDownAtBar2();
    // 1 s of audio starting in bar 1 crosses no change; starting in bar 2 it is 1 beat.
    CHECK_EQ(ticksSpanning(p, 0, 48000, 48000), 2 * kQ);
    CHECK_EQ(ticksSpanning(p, kBar, 48000, 48000), kQ);
    // Crossing the change: from beat 4 of bar 1, 1 s = half a beat at 120 + half a beat at 60.
    CHECK_EQ(ticksSpanning(p, 3 * kQ, 48000, 48000), kQ + kQ / 2);
    CHECK_EQ(framesSpanning(p, 3 * kQ, kQ + kQ / 2, 48000), int64_t(48000));
    CHECK_EQ(framesSpanning(p, kBar, -kQ, 48000), int64_t(-24000));
    CHECK_NEAR(ratioForTempo(p, 90, 0), 0.75, 1e-12);
    CHECK_NEAR(ratioForTempo(p, 90, kBar), 1.5, 1e-12);
}
