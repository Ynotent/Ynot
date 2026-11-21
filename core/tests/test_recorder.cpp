#include <filesystem>
#include <thread>

#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;
namespace fs = std::filesystem;

namespace {
Project emptyProject(Track** vocal) {
    Project p;
    p.id = newId();
    p.name = "Rec";
    p.tempoBpm = 120;  // 1 beat = 0.5 s = 24000 frames @ 48 kHz
    Track t;
    t.id = "vox";
    t.name = "Vocal";
    t.kind = TrackKind::Audio;
    t.armed = true;
    p.tracks.push_back(t);
    if (vocal) *vocal = &p.tracks.back();
    return p;
}

// Feeds `total` frames of input in device-sized blocks, like an audio callback.
void feed(Recorder& r, const std::vector<float>& signal, int64_t engineStart, int block = 256,
          bool drainAsWeGo = true) {
    std::vector<float> silence(static_cast<size_t>(block), 0.f);
    for (size_t off = 0; off < signal.size(); off += static_cast<size_t>(block)) {
        int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), signal.size() - off));
        const float* ins[2] = {signal.data() + off, silence.data()};
        r.capture(ins, 2, n, engineStart + static_cast<int64_t>(off));
        if (drainAsWeGo) r.drain();
    }
}
}  // namespace

TEST("media: incremental SHA-256 matches known vectors and one-shot hashing") {
    CHECK_EQ(sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    std::string big(200001, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31);
    auto dir = th::tempDir("sha");
    auto path = (fs::path(dir) / "f.bin").string();
    writeFileBytes(path, std::vector<uint8_t>(big.begin(), big.end()));
    CHECK_EQ(sha256File(path), sha256Hex(big));
}

TEST("media: streaming WAV writer produces a valid file") {
    auto dir = th::tempDir("wavw");
    auto path = (fs::path(dir) / "take.wav").string();
    WavFileWriter w;
    CHECK(w.open(path, 44100, 2));
    std::vector<float> a{0.1f, -0.1f, 0.2f, -0.2f}, b{0.3f, -0.3f};
    CHECK(w.write(a.data(), 2));
    CHECK(w.write(b.data(), 1));
    w.close();
    AudioBuffer got = decodeWav(readFileBytes(path));
    CHECK_EQ(got.frames(), int64_t(3));
    CHECK_EQ(got.channels, 2);
    CHECK_EQ(got.sampleRate, 44100.0);
    CHECK_EQ(got.samples[4], 0.3f);
}

TEST("recorder: lock-free ring keeps order across threads") {
    SpscFloatRing ring(1000);
    const int total = 200000;
    std::thread producer([&] {
        for (int i = 0; i < total;) {
            float v = static_cast<float>(i);
            if (ring.write(&v, 1) == 1) ++i;
            else std::this_thread::yield();
        }
    });
    int expected = 0;
    bool inOrder = true;
    float buf[64];
    while (expected < total) {
        size_t n = ring.read(buf, 64);
        for (size_t k = 0; k < n; ++k) inOrder &= buf[k] == static_cast<float>(expected++);
        if (n == 0) std::this_thread::yield();
    }
    producer.join();
    CHECK(inOrder);
    CHECK_EQ(ring.available(), size_t(0));
}

TEST("recorder: take lands at the right time with latency compensation") {
    auto dir = th::tempDir("rec");
    Track* vox = nullptr;
    Project p = emptyProject(&vox);
    const double sr = 48000;
    const int latency = 200;
    // Performer plays an impulse; the interface delivers it 1000 samples into the take.
    std::vector<float> input(24000, 0.f);
    input[1000] = 0.9f;

    Recorder rec;
    Recorder::Config cfg;
    cfg.sampleRate = sr;
    cfg.maxBlock = 256;
    cfg.targets = {{"vox", {0}}};
    cfg.packageDir = dir;
    CHECK(rec.start(cfg));
    feed(rec, input, 48000);  // recording started at 1 s (beat 2)
    auto takes = rec.finish();
    CHECK_EQ(takes.size(), size_t(1));
    CHECK_EQ(takes[0].frames, int64_t(24000));
    CHECK_EQ(rec.droppedFrames(), int64_t(0));

    auto ids = commitTakes(p, dir, takes, latency);
    CHECK_EQ(ids.size(), size_t(1));
    const AudioClip& clip = p.findTrack("vox")->audioClips.at(0);
    CHECK_EQ(clip.startTick, int64_t(2 * kTicksPerQuarter));
    CHECK_EQ(clip.sourceOffsetFrames, int64_t(latency));
    // Media is content-addressed and the scratch folder is cleaned up.
    const MediaRef* m = p.findMedia(clip.mediaId);
    CHECK(m && m->origin == "recording" && m->frames == 24000);
    CHECK(fs::exists(fs::path(dir) / m->fileName));
    CHECK(!fs::exists(fs::path(dir) / ".recording"));

    // Playback puts the impulse where the performer heard it: 48000 + 1000 - 200.
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    AudioBuffer out = renderProject(p, media, host, sr, 0.0);
    int64_t peakAt = 0;
    float peak = 0;
    for (int64_t f = 0; f < out.frames(); ++f)
        if (std::fabs(out.samples[static_cast<size_t>(f * 2)]) > peak) {
            peak = std::fabs(out.samples[static_cast<size_t>(f * 2)]);
            peakAt = f;
        }
    CHECK_EQ(peakAt, int64_t(48800));
    CHECK_NEAR(peak, 0.9, 1e-4);
}

TEST("recorder: saved project with a recording reloads losslessly") {
    auto dir = th::tempDir("rec-save");
    Project p = emptyProject(nullptr);
    Recorder rec;
    Recorder::Config cfg;
    cfg.targets = {{"vox", {0, 1}}};  // stereo take
    cfg.packageDir = dir;
    CHECK(rec.start(cfg));
    std::vector<float> sig(4800, 0.25f);
    feed(rec, sig, 0);
    commitTakes(p, dir, rec.finish(), 0);
    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK(projectToJson(p) == projectToJson(q));
    AudioBuffer take = loadMedia(q.media.at(0), dir);  // verifies hash
    CHECK_EQ(take.channels, 2);
    CHECK_EQ(take.samples[0], 0.25f);
    CHECK_EQ(take.samples[1], 0.0f);  // input 1 was silent
}

TEST("recorder: a stalled disk thread reports dropped audio instead of hiding it") {
    auto dir = th::tempDir("rec-drop");
    Recorder rec;
    Recorder::Config cfg;
    cfg.targets = {{"vox", {0}}};
    cfg.packageDir = dir;
    cfg.ringSeconds = 0.1;  // 4800 frames of headroom
    CHECK(rec.start(cfg));
    std::vector<float> sig(48000, 0.1f);
    feed(rec, sig, 0, 256, /*drainAsWeGo=*/false);
    CHECK(rec.droppedFrames() > 40000);
    rec.cancel();
}

TEST("recorder: refuses to start with nothing armed") {
    Recorder rec;
    Recorder::Config cfg;
    cfg.packageDir = th::tempDir("rec-none");
    CHECK(!rec.start(cfg));
    CHECK(!rec.error().empty());
}

TEST("recorder: audio takes are not placed on instrument tracks") {
    auto dir = th::tempDir("rec-inst");
    Project p = emptyProject(nullptr);
    p.tracks[0].kind = TrackKind::Instrument;
    Recorder rec;
    Recorder::Config cfg;
    cfg.targets = {{"vox", {0}}};
    cfg.packageDir = dir;
    CHECK(rec.start(cfg));
    feed(rec, std::vector<float>(4800, 0.1f), 0);
    CHECK(commitTakes(p, dir, rec.finish(), 0).empty());
    CHECK(p.media.empty());
}

TEST("recorder: punch-in replaces only the overlapped part of existing clips") {
    Project p = emptyProject(nullptr);
    MediaRef m;
    m.id = "m";
    m.sampleRate = 48000;
    p.media.push_back(m);
    Track& t = p.tracks[0];
    const int64_t beat = kTicksPerQuarter;
    t.audioClips = {{"old", "m", 0, 4 * beat, 0, 0.0}};

    placeClipReplacing(p, t, {"new", "m", 1 * beat, 1 * beat, 0, 0.0});  // inside: split
    CHECK_EQ(t.audioClips.size(), size_t(3));
    CHECK_EQ(t.audioClips[0].lengthTick, beat);
    CHECK_EQ(t.audioClips[1].id, std::string("new"));
    CHECK_EQ(t.audioClips[2].startTick, 2 * beat);
    CHECK_EQ(t.audioClips[2].lengthTick, 2 * beat);
    CHECK_EQ(t.audioClips[2].sourceOffsetFrames, int64_t(48000));  // 2 beats in = 1 s

    placeClipReplacing(p, t, {"cover", "m", 0, 2 * beat, 0, 0.0});  // covers first two
    CHECK_EQ(t.audioClips.size(), size_t(2));
    CHECK_EQ(t.audioClips[0].id, std::string("cover"));

    placeClipReplacing(p, t, {"tail", "m", 3 * beat, 2 * beat, 0, 0.0});  // overlaps end
    CHECK_EQ(t.audioClips[1].lengthTick, beat);  // old right part trimmed to [2,3)
    CHECK_EQ(t.audioClips[2].id, std::string("tail"));
}

TEST("engine: metronome clicks on beats in live output only") {
    Project p = emptyProject(nullptr);
    PackageMediaProvider media(th::tempDir("click"));
    BuiltinPluginHost host;
    Engine e;
    EngineOptions o;
    o.sampleRate = 48000;
    o.maxBlock = 512;
    e.prepare(p, media, host, o);
    e.setMetronome(true);
    std::vector<float> l(48000), r(48000);
    for (int done = 0; done < 48000; done += 480) e.process(l.data() + done, r.data() + done, 480);
    auto energy = [&](int from, int to) {
        double s = 0;
        for (int i = from; i < to; ++i) s += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
        return s;
    };
    CHECK(energy(0, 1440) > 0.1);         // beat 1 (downbeat)
    CHECK(energy(24000, 25440) > 0.1);    // beat 2
    CHECK(energy(3000, 23000) == 0.0);    // silence between beats
    AudioBuffer bounce = renderProject(p, media, host, 48000, 1.0);
    CHECK(th::rms(bounce) == 0.0);        // never in bounces
}

TEST("engine: input monitoring runs through inserts, fader and pan, even when stopped") {
    Track* vox = nullptr;
    Project p = emptyProject(&vox);
    auto gain = makeBuiltin(kBuiltinGain, "Gain");
    gain.params["gainDb"] = -6.0;
    p.tracks[0].inserts.push_back(gain);
    p.tracks[0].pan = -1.0;
    Track other;
    other.id = "gtr";
    other.name = "Guitar";
    p.tracks.push_back(other);

    PackageMediaProvider media(th::tempDir("mon"));
    BuiltinPluginHost host;
    Engine e;
    EngineOptions o;
    o.maxBlock = 256;
    o.monitorInputs = {{"vox", {0}}};  // only the vocal track monitors input 1
    e.prepare(p, media, host, o);
    std::vector<float> in(256, 0.5f), l(256), r(256);
    const float* ins[1] = {in.data()};
    e.process(l.data(), r.data(), 256, ins, 1, /*transportRunning=*/false);
    const double expected = 0.5 * dbToGain(-6.0) * std::sqrt(2.0);  // hard-left pan law
    CHECK_NEAR(l[100], expected, 1e-4);
    CHECK_NEAR(r[100], 0.0, 1e-6);
    CHECK_EQ(e.position(), int64_t(0));  // stopped transport doesn't move

    p.tracks[0].mute = true;  // muting the track also mutes its monitoring
    e.prepare(p, media, host, o);
    e.process(l.data(), r.data(), 256, ins, 1, false);
    CHECK_EQ(l[100], 0.0f);
}

TEST("engine: count-in clicks before the record point even with the metronome off") {
    Project p = emptyProject(nullptr);
    PackageMediaProvider media(th::tempDir("countin"));
    BuiltinPluginHost host;
    Engine e;
    EngineOptions o;
    o.maxBlock = 480;
    e.prepare(p, media, host, o);
    e.seek(-48000);  // one 2-beat pre-roll at 120 BPM
    e.setForcedClickUntil(0);
    std::vector<float> l(96000), r(96000);
    for (int done = 0; done < 96000; done += 480) e.process(l.data() + done, r.data() + done, 480);
    auto energy = [&](int from, int to) {
        double s = 0;
        for (int i = from; i < to; ++i) s += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
        return s;
    };
    CHECK(energy(0, 1440) > 0.1);          // engine frame -48000
    CHECK(energy(24000, 25440) > 0.1);     // engine frame -24000
    CHECK(energy(48000, 96000) == 0.0);    // from the record point on: no click
    CHECK_EQ(e.position(), int64_t(48000));
}

TEST("recorder: input during the count-in is not recorded") {
    auto dir = th::tempDir("rec-countin");
    Recorder rec;
    Recorder::Config cfg;
    cfg.targets = {{"vox", {0}}};
    cfg.packageDir = dir;
    cfg.startAtFrame = 0;
    CHECK(rec.start(cfg));
    std::vector<float> sig(5000);
    for (size_t i = 0; i < sig.size(); ++i) sig[i] = static_cast<float>(i) / 10000.0f;
    feed(rec, sig, -1000);  // capture starts 1000 frames before the record point
    auto takes = rec.finish();
    CHECK_EQ(takes.size(), size_t(1));
    CHECK_EQ(takes[0].startFrame, int64_t(0));
    CHECK_EQ(takes[0].frames, int64_t(4000));
    AudioBuffer got = decodeWav(readFileBytes(takes[0].tempPath));
    CHECK_NEAR(got.samples[0], 0.1, 1e-6);  // first recorded sample is input index 1000
}

TEST("punch: audio replaces only the punch range of the existing take") {
    auto dir = th::tempDir("punch-audio");
    Project p = emptyProject(nullptr);
    // Existing 2 s take at a constant level of 0.2 on the vocal track.
    AudioBuffer old;
    old.sampleRate = 48000;
    old.channels = 1;
    old.samples.assign(96000, 0.2f);
    std::string oldId = addMedia(p, dir, old, "recording").id;
    p.findTrack("vox")->audioClips = {{"old", oldId, 0, 4 * kTicksPerQuarter, 0, 0.0}};

    // Punch in at beat 1 (24000), out at beat 2 (48000). Transport runs the whole 2 s;
    // the performer sings a constant 0.7 throughout, but only the range is kept.
    Recorder rec;
    Recorder::Config cfg;
    cfg.targets = {{"vox", {0}}};
    cfg.packageDir = dir;
    cfg.startAtFrame = 24000;
    cfg.stopAtFrame = 48000;
    CHECK(rec.start(cfg));
    feed(rec, std::vector<float>(96000, 0.7f), 0);
    auto takes = rec.finish();
    CHECK_EQ(takes.size(), size_t(1));
    CHECK_EQ(takes[0].startFrame, int64_t(24000));
    CHECK_EQ(takes[0].frames, int64_t(24000));
    commitTakes(p, dir, takes, 0);
    CHECK_EQ(p.findTrack("vox")->audioClips.size(), size_t(3));  // old | new | old

    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    AudioBuffer out = renderProject(p, media, host, 48000, 0.0);
    auto at = [&](int64_t f) { return out.samples[static_cast<size_t>(f * 2)]; };
    CHECK_NEAR(at(12000), 0.2, 1e-4);  // before punch-in: old take
    CHECK_NEAR(at(36000), 0.7, 1e-4);  // inside the punch: new take
    CHECK_NEAR(at(60000), 0.2, 1e-4);  // after punch-out: old take continues
}

TEST("punch: MIDI replaces notes only inside the range") {
    Track t;
    t.id = "k";
    t.kind = TrackKind::Instrument;
    const int64_t b = kTicksPerQuarter;
    t.midiClips = {{"c", 0, 4 * b, {{0, b / 2, 36, 100}, {b, b / 2, 38, 100}, {2 * b, b / 2, 36, 100}, {3 * b, b / 2, 38, 100}}}};
    CHECK_EQ(eraseNotesInRange(t, b, 3 * b), 2);
    CHECK_EQ(t.midiClips[0].notes.size(), size_t(2));
    CHECK_EQ(t.midiClips[0].notes[1].tick, 3 * b);

    // The MIDI recorder ignores new notes after punch-out but keeps their note-offs.
    MidiTakeRecorder rec;
    rec.start(24000, 72000);
    MidiEvent on1{0, true, 40, 100}, off1{0, false, 40, 0}, late{0, true, 41, 100};
    rec.capture(&on1, 1, 60000);
    rec.capture(&off1, 1, 80000);  // after punch-out: kept, so the note gets its end
    rec.capture(&late, 1, 80000);  // new note after punch-out: ignored
    auto ev = rec.finish();
    CHECK_EQ(ev.size(), size_t(2));
}
