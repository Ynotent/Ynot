#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr int64_t kBeat = kTicksPerQuarter;  // 0.5 s at 120 BPM

// Two audio tracks sharing a 4 s, 48 kHz take, plus an instrument track.
Project editProject() {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    MediaRef m;
    m.id = "m";
    m.sampleRate = 48000;
    m.channels = 1;
    m.frames = 4 * 48000;  // 8 beats
    p.media.push_back(m);
    Track a;
    a.id = "a";
    a.kind = TrackKind::Audio;
    a.audioClips = {{"c1", "m", 0, 4 * kBeat, 0, 0.0}};
    Track b;
    b.id = "b";
    b.kind = TrackKind::Audio;
    Track k;
    k.id = "k";
    k.kind = TrackKind::Instrument;
    k.midiClips = {{"mc", 0, 4 * kBeat, {{0, kBeat, 60, 100}, {2 * kBeat, kBeat, 64, 100}}}};
    p.tracks = {a, b, k};
    return p;
}
}  // namespace

TEST("edit: snap to grid") {
    CHECK_EQ(snapTick(1000, kBeat), kBeat);
    CHECK_EQ(snapTick(1500, kBeat), 2 * kBeat);
    CHECK_EQ(snapTick(1234, 0), int64_t(1234));
}

TEST("edit: move a clip in time and onto another track of the same kind") {
    Project p = editProject();
    CHECK(moveClip(p, "a", "c1", 2 * kBeat));
    CHECK_EQ(p.findTrack("a")->audioClips[0].startTick, 2 * kBeat);
    CHECK(moveClip(p, "a", "c1", kBeat, "b"));
    CHECK(p.findTrack("a")->audioClips.empty());
    CHECK_EQ(p.findTrack("b")->audioClips[0].startTick, kBeat);
    CHECK(!moveClip(p, "b", "c1", 0, "k"));  // audio can't go on an instrument track
    CHECK(!moveClip(p, "k", "mc", 0, "a"));  // MIDI can't go on an audio track
    CHECK(moveClip(p, "b", "c1", -500));      // clamps at the song start
    CHECK_EQ(p.findTrack("b")->audioClips[0].startTick, int64_t(0));
}

TEST("edit: moving an audio clip onto another replaces the overlapped part") {
    Project p = editProject();
    p.findTrack("b")->audioClips = {{"c2", "m", 0, 4 * kBeat, 0, 0.0}};
    CHECK(moveClip(p, "a", "c1", 2 * kBeat, "b"));
    auto& clips = p.findTrack("b")->audioClips;
    CHECK_EQ(clips.size(), size_t(2));
    CHECK_EQ(clips[0].id, std::string("c2"));
    CHECK_EQ(clips[0].lengthTick, 2 * kBeat);
}

TEST("edit: trimming audio moves the source offset and respects the recording's length") {
    Project p = editProject();
    CHECK(trimClipStart(p, "a", "c1", kBeat));
    auto* c = &p.findTrack("a")->audioClips[0];
    CHECK_EQ(c->startTick, kBeat);
    CHECK_EQ(c->lengthTick, 3 * kBeat);
    CHECK_EQ(c->sourceOffsetFrames, int64_t(24000));
    CHECK(trimClipStart(p, "a", "c1", -10 * kBeat));  // can't reveal audio before the take began
    c = &p.findTrack("a")->audioClips[0];
    CHECK_EQ(c->startTick, int64_t(0));
    CHECK_EQ(c->sourceOffsetFrames, int64_t(0));
    CHECK(trimClipEnd(p, "a", "c1", 100 * kBeat));    // can't extend past the end of the take
    CHECK_EQ(p.findTrack("a")->audioClips[0].lengthTick, 8 * kBeat);
    CHECK(trimClipEnd(p, "a", "c1", 0));              // never shorter than the minimum
    CHECK_EQ(p.findTrack("a")->audioClips[0].lengthTick, kMinClipTicks);
}

TEST("edit: MIDI trims hide notes without deleting them") {
    Project p = editProject();
    CHECK(trimClipStart(p, "k", "mc", 2 * kBeat));  // hides the first note
    PackageMediaProvider media(th::tempDir("edit-midi"));
    BuiltinPluginHost host;
    p.findTrack("k")->instrument = makeBuiltin(kBuiltinSynth, "Synth");
    p.findTrack("a")->mute = true;
    // Only the second note (beat 2..3) sounds: silence in the first second.
    AudioBuffer out = renderProject(p, media, host, 48000, 0.0);
    CHECK(th::rms(out, 0, 47000) < 1e-6);
    CHECK(th::rms(out, 49000, 70000) > 0.01);
    CHECK(trimClipStart(p, "k", "mc", 0));  // extend back: the note returns
    CHECK_EQ(p.findTrack("k")->midiClips[0].notes[0].tick, int64_t(0));
    CHECK_EQ(p.findTrack("k")->midiClips[0].notes.size(), size_t(2));
}

TEST("edit: split audio and MIDI clips") {
    Project p = editProject();
    auto right = splitClip(p, "a", "c1", kBeat);
    CHECK(right.has_value());
    auto& clips = p.findTrack("a")->audioClips;
    CHECK_EQ(clips.size(), size_t(2));
    CHECK_EQ(clips[0].lengthTick, kBeat);
    CHECK_EQ(clips[1].startTick, kBeat);
    CHECK_EQ(clips[1].sourceOffsetFrames, int64_t(24000));
    CHECK(!splitClip(p, "a", "c1", 0).has_value());  // at the very edge: no-op

    auto midiRight = splitClip(p, "k", "mc", 2 * kBeat);
    CHECK(midiRight.has_value());
    auto& mclips = p.findTrack("k")->midiClips;
    CHECK_EQ(mclips[1].notes[1].tick, int64_t(0));  // second note now starts the right clip
}

TEST("edit: split playback sounds identical to the unsplit clip") {
    auto dir = th::tempDir("split-same");
    Project p = makeDemoProject(dir);
    p.tracks[1].mute = p.tracks[2].mute = true;
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    AudioBuffer before = renderProject(p, media, host, 48000, 0.0);
    auto maxDiffAfterSplit = [&](int64_t atTick) {
        Project q = p;
        CHECK(splitClip(q, q.tracks[0].id, q.tracks[0].audioClips[0].id, atTick).has_value());
        AudioBuffer after = renderProject(q, media, host, 48000, 0.0);
        CHECK_EQ(before.samples.size(), after.samples.size());
        // Compare the clip body; the final 2 samples where the 2 s take runs out can differ by
        // one interpolated sample (end-of-media edge, not a discontinuity at the split).
        double d = 0;
        for (size_t i = 0; i < static_cast<size_t>(95990 * 2); ++i)
            d = std::max(d, static_cast<double>(std::fabs(before.samples[i] - after.samples[i])));
        return d;
    };
    // The demo take is 44.1 kHz; at 120 BPM every 32 ticks is a whole number of its samples.
    CHECK(maxDiffAfterSplit(kBeat + 128) < 1e-5);
    // Elsewhere the stored source offset rounds to the nearest source sample (at most half a
    // sample, ~11 microseconds at 44.1 kHz): bounded and inaudible. For this 220 Hz take at
    // peak level 0.3 * -3 dB, half a sample of shift changes a sample by at most
    // 0.212 * 2*pi*220/44100 * 0.5 = 0.0033. Sample-exact offsets are planned for format v2.
    CHECK(maxDiffAfterSplit(kBeat + 123) < 0.0034);
}

TEST("edit: delete and undo/redo through snapshots") {
    Project p = editProject();
    History h;
    h.record(p);
    CHECK(deleteClip(p, "a", "c1"));
    CHECK(p.findTrack("a")->audioClips.empty());
    h.record(p);
    CHECK(moveClip(p, "k", "mc", 4 * kBeat));
    CHECK(h.undo(p));
    CHECK_EQ(p.findTrack("k")->midiClips[0].startTick, int64_t(0));
    CHECK(h.undo(p));
    CHECK_EQ(p.findTrack("a")->audioClips.size(), size_t(1));
    CHECK(!h.canUndo());
    CHECK(h.redo(p));
    CHECK(p.findTrack("a")->audioClips.empty());
    h.record(p);  // a new edit clears the redo stack
    CHECK(!h.canRedo());
    CHECK(!deleteClip(p, "a", "nope"));
}
