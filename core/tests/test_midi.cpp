#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr int64_t kBeat = kTicksPerQuarter;  // 0.5 s = 24000 frames @ 48 kHz, 120 BPM
constexpr double kSr = 48000;

Project synthProject() {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    Track k;
    k.id = "keys";
    k.kind = TrackKind::Instrument;
    k.armed = true;
    k.instrument = makeBuiltin(kBuiltinSynth, "Synth");
    p.tracks.push_back(k);
    return p;
}
}  // namespace

TEST("midi: recorded events become notes at the right musical positions") {
    Project p = synthProject();
    MidiTakeRecorder rec;
    rec.start(24000);  // record point: beat 1 (after a 1-beat count-in)
    // Block size 512; events land mid-block.
    MidiEvent a[] = {{100, true, 60, 90}};
    rec.capture(a, 1, 23040);                       // frame 23140: during the count-in -> ignored
    MidiEvent b[] = {{0, true, 64, 100}};
    rec.capture(b, 1, 24000);                       // frame 24000 = beat 1
    MidiEvent c[] = {{0, false, 64, 0}, {0, true, 67, 80}};
    rec.capture(c, 2, 36000);                       // 1/2 beat later: E off, G on
    MidiEvent d[] = {{0, false, 60, 0}};            // off for the count-in note: ignored
    rec.capture(d, 1, 40000);
    auto events = rec.finish();
    CHECK_EQ(events.size(), size_t(4));

    auto clip = buildMidiClip(p, events, 24000, 72000, kSr);  // stop at beat 3; G still held
    CHECK(clip.has_value());
    CHECK_EQ(clip->startTick, kBeat);
    CHECK_EQ(clip->lengthTick, 2 * kBeat);
    CHECK_EQ(clip->notes.size(), size_t(2));
    CHECK_EQ(clip->notes[0].pitch, 64);
    CHECK_EQ(clip->notes[0].tick, int64_t(0));
    CHECK_EQ(clip->notes[0].lengthTick, kBeat / 2);
    CHECK_EQ(clip->notes[0].velocity, 100);
    CHECK_EQ(clip->notes[1].pitch, 67);
    CHECK_EQ(clip->notes[1].tick, kBeat / 2);
    CHECK_EQ(clip->notes[1].lengthTick, kBeat * 3 / 2);  // held to the stop point
}

TEST("midi: re-struck notes, stray note-offs, quantize and empty takes") {
    Project p = synthProject();
    std::vector<TimedMidiEvent> ev = {
        {1000, false, 50, 0},   // off with no on: ignored
        {2000, true, 60, 100},
        {13000, true, 60, 70},  // re-struck before release: first note ends here
        {20000, false, 60, 0},
    };
    auto clip = buildMidiClip(p, ev, 0, 48000, kSr);
    CHECK(clip && clip->notes.size() == 2);
    CHECK_EQ(clip->notes[1].velocity, 70);

    // 1/4-beat grid: a note played 30 ms late snaps back onto the beat.
    std::vector<TimedMidiEvent> late = {{24000 + 1440, true, 62, 100}, {30000, false, 62, 0}};
    QuantizeSettings sixteenths;
    sixteenths.gridTicks = kBeat / 4;
    auto q = buildMidiClip(p, late, 0, 48000, kSr, &sixteenths);
    CHECK(q && q->notes[0].tick == kBeat);

    CHECK(!buildMidiClip(p, {}, 0, 48000, kSr).has_value());
}

TEST("midi: recorder never allocates past capacity; overflow is counted") {
    MidiTakeRecorder rec;
    rec.start(0, std::numeric_limits<int64_t>::max(), 4);
    MidiEvent burst[6];
    for (int i = 0; i < 6; ++i) burst[i] = {i, true, 60 + i, 100};
    rec.capture(burst, 6, 0);
    CHECK_EQ(rec.droppedEvents(), int64_t(2));
    CHECK_EQ(rec.finish().size(), size_t(4));
}

TEST("engine: live MIDI plays armed instruments, even with the transport stopped") {
    Project p = synthProject();
    Track other = p.tracks[0];
    other.id = "other";
    other.armed = false;
    p.tracks.push_back(other);
    PackageMediaProvider media(th::tempDir("live-midi"));
    BuiltinPluginHost host;
    Engine e;
    EngineOptions o;
    o.maxBlock = 512;
    o.liveMidiTracks = {"keys"};
    e.prepare(p, media, host, o);

    std::vector<float> l(512), r(512);
    MidiEvent on{100, true, 69, 127};
    e.process(l.data(), r.data(), 512, nullptr, 0, /*transportRunning=*/false, &on, 1);
    double before = 0, after = 0;
    for (int i = 0; i < 100; ++i) before += std::fabs(l[static_cast<size_t>(i)]);
    for (int i = 100; i < 512; ++i) after += std::fabs(l[static_cast<size_t>(i)]);
    CHECK(before == 0.0);   // sample-accurate: silent until the note's offset
    CHECK(after > 1.0);
    CHECK_EQ(e.position(), int64_t(0));

    // Only the armed track responds: muting it silences live play.
    p.tracks[0].mute = true;
    e.prepare(p, media, host, o);
    e.process(l.data(), r.data(), 512, nullptr, 0, false, &on, 1);
    double sum = 0;
    for (float s : l) sum += std::fabs(s);
    CHECK(sum == 0.0);
}

TEST("midi: a recorded take plays back like the live performance") {
    Project p = synthProject();
    std::vector<TimedMidiEvent> ev = {{0, true, 57, 100}, {24000, false, 57, 0}};
    auto clip = buildMidiClip(p, ev, 0, 48000, kSr);
    CHECK(clip.has_value());
    p.tracks[0].midiClips.push_back(*clip);
    PackageMediaProvider media(th::tempDir("midi-play"));
    BuiltinPluginHost host;
    AudioBuffer out = renderProject(p, media, host, kSr, 0.0);
    CHECK(th::rms(out, 0, 23000) > 0.01);       // note sounds for its half second
    CHECK(th::rms(out, 36000, 48000) < 1e-4);   // and is released afterwards
}
