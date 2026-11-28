#include <map>

#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr int64_t kQ = kTicksPerQuarter;
constexpr int64_t kBar = 4 * kQ;  // 4/4 at 120 BPM: a bar is 2 s = 96000 frames at 48 kHz
constexpr double kSr = 48000;

Project synthProject(std::vector<MidiNote> notes) {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    Track t;
    t.id = "k";
    t.kind = TrackKind::Instrument;
    t.instrument = makeBuiltin(kBuiltinSynth, "Synth");
    t.midiClips = {{"m", 0, 4 * kBar, std::move(notes)}};
    p.tracks = {t};
    return p;
}

// Renders `seconds` of live playback (block size 512, so wraps land mid-block).
AudioBuffer play(Engine& e, double seconds) {
    AudioBuffer out;
    out.sampleRate = kSr;
    out.channels = 2;
    std::vector<float> l(512), r(512);
    const auto blocks = static_cast<int>(seconds * kSr / 512);
    for (int b = 0; b < blocks; ++b) {
        e.process(l.data(), r.data(), 512);
        for (int i = 0; i < 512; ++i) {
            out.samples.push_back(l[static_cast<size_t>(i)]);
            out.samples.push_back(r[static_cast<size_t>(i)]);
        }
    }
    return out;
}

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

std::string writeTake(const std::string& dir, int64_t frames) {
    const std::string path = dir + "/take-" + newId().substr(0, 6) + ".wav";
    WavFileWriter w;
    w.open(path, kSr, 1);
    std::vector<float> v(static_cast<size_t>(frames));
    for (int64_t i = 0; i < frames; ++i) v[static_cast<size_t>(i)] = static_cast<float>(i % 100) / 100.f;
    w.write(v.data(), frames);
    w.close();
    return path;
}
}  // namespace

TEST("loop: playback repeats the loop seamlessly, even when the jump is mid-block") {
    // A note at the start of bar 2; loop bars 2-3 (1 bar).
    Project p = synthProject({{kBar, kQ / 4, 69, 100}});
    BuiltinPluginHost host;
    PackageMediaProvider media(th::tempDir("loop"));
    Engine e;
    EngineOptions o;
    o.sampleRate = kSr;
    e.prepare(p, media, host, o);
    e.setLoop(kBar, 2 * kBar, true);
    CHECK_EQ(e.loopStartFrame(), int64_t(96000));
    CHECK_EQ(e.loopEndFrame(), int64_t(192000));
    auto out = play(e, 8.0);
    // Heard at 2 s, then again every 2 s (the loop length).
    auto got = onsets(out);
    CHECK(got.size() >= 3);
    for (size_t i = 0; i < 3 && i < got.size(); ++i)
        CHECK(std::abs(got[i] - static_cast<int64_t>(96000 * (i + 1))) <= 2);
    CHECK(e.position() >= 96000 && e.position() < 192000);  // still inside the loop
    // Off: plays straight through.
    e.setLoop(kBar, 2 * kBar, false);
    e.seek(0);
    auto straight = onsets(play(e, 6.0));
    CHECK_EQ(straight.size(), size_t(1));
}

TEST("loop: the click restarts with the loop; a note held over the loop end is released") {
    // Note from beat 4 of bar 2 to beat 2 of bar 3: crosses the loop end at bar 3.
    Project p = synthProject({{kBar + 3 * kQ, 2 * kQ, 60, 100}});
    BuiltinPluginHost host;
    PackageMediaProvider media(th::tempDir("loop-held"));
    Engine e;
    EngineOptions o;
    o.sampleRate = kSr;
    e.prepare(p, media, host, o);
    e.setLoop(kBar, 2 * kBar, true);
    e.seek(96000);
    auto out = play(e, 2.5);  // one pass (2 s) plus 0.5 s of the next
    // Right after the jump back to bar 2 nothing should sound: the held note was released.
    CHECK(th::rms(out, 96000 + 200, 96000 + 20000) < 1e-4);
    CHECK(th::rms(out, 72000 + 500, 95000) > 0.01);  // the note did play before the jump

    Project silent;  // metronome only
    silent.id = "s";
    Engine c;
    c.prepare(silent, media, host, o);
    c.setMetronome(true);
    c.setLoop(kBar, kBar + 2 * kQ, true);  // a 2-beat loop
    c.seek(96000);
    auto clicks = onsets(play(c, 3.0));
    // Beats at 0 and 0.5 s of each 1 s pass: 0, 24000, 48000, 72000, ...
    for (size_t i = 0; i < 5 && i < clicks.size(); ++i) CHECK(std::abs(clicks[i] - static_cast<int64_t>(24000 * i)) <= 1);
}

TEST("cycle recording: MIDI from every pass is merged into one clip over the loop") {
    Project p;
    p.tempoBpm = 120;
    const int64_t ls = 48000, le = 96000;  // beat 3 to bar 2 (1 s loop)
    std::vector<TimedMidiEvent> ev = {
        {10000, true, 40, 90},  {20000, false, 40, 0},   // count-in: dropped
        {50000, true, 60, 100}, {60000, false, 60, 0},   // pass 1
        {98000, true, 64, 80},  {100000, false, 64, 0},  // pass 2 -> folds to 50000
        {90000, true, 67, 70},  {110000, false, 67, 0},  // held over the loop end -> cut at 96000
        {150000, true, 72, 60},                          // still held at stop (pass 3)
    };
    auto clip = buildCycleMidiClip(p, ev, ls, le, 155000, kSr);
    CHECK(clip.has_value());
    CHECK_EQ(clip->startTick, 2 * kQ);
    CHECK_EQ(clip->lengthTick, 2 * kQ);
    CHECK_EQ(clip->notes.size(), size_t(4));
    std::map<int, MidiNote> byPitch;
    for (auto& n : clip->notes) byPitch[n.pitch] = n;
    CHECK(byPitch.count(40) == 0);
    CHECK_EQ(byPitch[64].tick, byPitch[60].tick);  // same place in the loop, different passes
    CHECK_EQ(byPitch[64].velocity, 80);
    CHECK_EQ(byPitch[67].tick + byPitch[67].lengthTick, clip->lengthTick);  // cut at the loop end
    CHECK(byPitch.count(72) == 1);
}

TEST("cycle recording: each pass is a take; the last complete one plays; takes switch cleanly") {
    auto dir = th::tempDir("cycle-audio");
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    Track a;
    a.id = "a";
    a.kind = TrackKind::Audio;
    p.tracks = {a};
    const int64_t L = 96000;  // loop = bar 2 (1 bar)
    const int latency = 100;
    RecordedTake t{"a", writeTake(dir, latency + L * 2 + L * 6 / 10), 96000, latency + L * 2 + L * 6 / 10, 1, kSr};
    auto ids = commitCycleTakes(p, dir, {t}, latency, kBar, 2 * kBar);
    CHECK_EQ(ids.size(), size_t(1));
    const AudioClip* c = &p.tracks[0].audioClips.at(0);
    CHECK_EQ(c->startTick, kBar);
    CHECK_EQ(c->lengthTick, kBar);
    CHECK_EQ(c->takeOffsets.size(), size_t(3));  // 2 full passes + 60% of a third
    CHECK_EQ(c->activeTake, 1);                   // the last complete pass
    CHECK_EQ(c->sourceOffsetFrames, latency + L);

    CHECK(selectTake(p, "a", c->id, 0));
    c = &p.tracks[0].audioClips.at(0);
    CHECK_EQ(c->sourceOffsetFrames, int64_t(latency));
    CHECK(trimClipStart(p, "a", c->id, kBar + kQ));  // trim half a second...
    c = &p.tracks[0].audioClips.at(0);
    CHECK(selectTake(p, "a", c->id, 2));             // ...and the trim carries over to the new take
    c = &p.tracks[0].audioClips.at(0);
    CHECK_EQ(c->sourceOffsetFrames, latency + 2 * L + 24000);
    CHECK(!selectTake(p, "a", c->id, 3));

    // Save/load keeps the takes and the loop.
    p.loop = {kBar, 2 * kBar, true};
    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK(projectToJson(q) == projectToJson(p));
    CHECK(q.loop.enabled && q.loop.startTick == kBar);

    // Less than a quarter of a second pass isn't a take; under one pass is an ordinary take.
    Project r = p;
    r.tracks[0].audioClips.clear();
    RecordedTake t2{"a", writeTake(dir, L + L / 10), 96000, L + L / 10, 1, kSr};
    commitCycleTakes(r, dir, {t2}, 0, kBar, 2 * kBar);
    CHECK(r.tracks[0].audioClips.at(0).takeOffsets.empty());
    CHECK_EQ(r.tracks[0].audioClips.at(0).lengthTick, kBar);
    Project s = p;
    s.tracks[0].audioClips.clear();
    RecordedTake t3{"a", writeTake(dir, L / 2), 96000, L / 2, 1, kSr};
    commitCycleTakes(s, dir, {t3}, 0, kBar, 2 * kBar);
    CHECK_EQ(s.tracks[0].audioClips.at(0).lengthTick, kBar / 2);
}
