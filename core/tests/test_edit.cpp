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

TEST("tracks: add, rename, remove; instrument tracks start with a working synth") {
    Project p = editProject();
    const auto before = p.tracks.size();
    std::string inst = addTrack(p, TrackKind::Instrument);
    std::string aud = addTrack(p, TrackKind::Audio, "Guitar");
    CHECK_EQ(p.tracks.size(), before + 2);
    CHECK(inst != aud && !inst.empty());
    CHECK_EQ(p.findTrack(aud)->name, std::string("Guitar"));
    CHECK_EQ(p.findTrack(inst)->name, std::string("Instrument 2"));  // one instrument track existed
    CHECK(p.findTrack(inst)->instrument && p.findTrack(inst)->instrument->uid == kBuiltinSynth);
    CHECK(!p.findTrack(aud)->instrument);
    CHECK(p.orderedTracks().back()->id == aud);  // appended at the end
    CHECK(p.findTrack(inst)->color != p.findTrack(aud)->color);

    // The new instrument track plays notes straight away.
    p.findTrack(inst)->midiClips = {{"m", 0, kBeat, {{0, kBeat / 2, 60, 100}}}};
    for (auto& t : p.tracks) t.mute = t.id != inst;
    PackageMediaProvider media(th::tempDir("addtrack"));
    BuiltinPluginHost host;
    CHECK(th::rms(renderProject(p, media, host, 48000, 0.0)) > 0.01);

    CHECK(renameTrack(p, inst, "Bass"));
    CHECK(!renameTrack(p, inst, ""));
    CHECK(removeTrack(p, inst));
    CHECK(p.findTrack(inst) == nullptr);
    CHECK(!removeTrack(p, inst));
}

TEST("plugins: add, reorder, bypass and remove effects; swap instruments") {
    Project p = editProject();
    auto gain = makeBuiltin(kBuiltinGain, "Gain");
    gain.params["gainDb"] = -60.0;  // nearly silent: easy to hear whether it's active
    auto lp = makeBuiltin(kBuiltinFilter, "Lowpass");
    const std::string g = addInsert(p, "a", gain);
    const std::string f = addInsert(p, "a", lp, 0);  // at the front
    CHECK_EQ(p.findTrack("a")->inserts.size(), size_t(2));
    CHECK_EQ(p.findTrack("a")->inserts[0].id, f);
    CHECK(moveInsert(p, "a", f, 5));                 // clamps to the end
    CHECK_EQ(p.findTrack("a")->inserts[1].id, f);

    auto dir = th::tempDir("plugins");
    AudioBuffer take;
    take.sampleRate = 48000;
    take.channels = 1;
    take.samples.assign(48000, 0.5f);
    std::string mid = addMedia(p, dir, take, "recording").id;
    p.findTrack("a")->audioClips = {{"c1", mid, 0, 2 * kBeat, 0, 0.0}};
    p.findTrack("k")->mute = true;
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    const double quiet = th::rms(renderProject(p, media, host, 48000, 0.0), 1000, 20000);
    CHECK(setPluginBypassed(p, "a", g, true));
    const double loud = th::rms(renderProject(p, media, host, 48000, 0.0), 1000, 20000);
    CHECK(loud > quiet * 100);
    CHECK(setPluginBypassed(p, "a", g, false));
    CHECK(removePlugin(p, "a", g));
    CHECK_EQ(p.findTrack("a")->inserts.size(), size_t(1));
    CHECK(!removePlugin(p, "a", g));

    // Instruments: only on instrument tracks; swapping unfreezes; removing silences.
    auto other = makeBuiltin(kBuiltinSynth, "Synth 2");
    CHECK(!setInstrument(p, "a", other));
    p.findTrack("k")->frozen = true;
    CHECK(setInstrument(p, "k", other));
    CHECK(!p.findTrack("k")->frozen);
    CHECK_EQ(p.findTrack("k")->instrument->name, std::string("Synth 2"));
    CHECK(removePlugin(p, "k", p.findTrack("k")->instrument->id));
    CHECK(!p.findTrack("k")->instrument);
}
