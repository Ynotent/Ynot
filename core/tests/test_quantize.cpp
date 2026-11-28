#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr int64_t kBeat = kTicksPerQuarter;  // 960
QuantizeSettings grid(int64_t g, double swing = 50, double strength = 1.0) {
    QuantizeSettings q;
    q.gridTicks = g;
    q.swingPercent = swing;
    q.strength = strength;
    return q;
}
}  // namespace

TEST("quantize: straight grids snap to the nearest step") {
    CHECK_EQ(quantizeTick(250, grid(kGrid1_16)), int64_t(240));
    CHECK_EQ(quantizeTick(355, grid(kGrid1_16)), int64_t(240));
    CHECK_EQ(quantizeTick(365, grid(kGrid1_16)), int64_t(480));
    CHECK_EQ(quantizeTick(1000, grid(kGrid1_4)), kBeat);
}

TEST("quantize: swing moves every second step (drum-machine style)") {
    // 1/16 at 66%: a pair of 16ths is 480 ticks, the off-step sits at 66% = 317 ticks.
    CHECK_EQ(quantizeTick(240, grid(kGrid1_16, 66)), int64_t(317));
    CHECK_EQ(quantizeTick(300, grid(kGrid1_16, 66)), int64_t(317));
    CHECK_EQ(quantizeTick(480, grid(kGrid1_16, 66)), int64_t(480));  // on-steps don't move
    CHECK_EQ(quantizeTick(480 + 240, grid(kGrid1_16, 66)), int64_t(480 + 317));
    // 50% is straight; 75% is the maximum (dotted feel).
    CHECK_EQ(quantizeTick(240, grid(kGrid1_16, 50)), int64_t(240));
    CHECK_EQ(quantizeTick(360, grid(kGrid1_16, 90)), int64_t(360));  // clamped to 75%
    // 8th swing at 66% is the classic triplet shuffle: off-8th lands on the 3rd triplet.
    CHECK_EQ(quantizeTick(480, grid(kGrid1_8, 66.6667)), int64_t(640));
}

TEST("quantize: triplet grids, and swing doesn't apply to them") {
    CHECK_EQ(quantizeTick(300, grid(kGrid1_8T)), int64_t(320));
    CHECK_EQ(quantizeTick(650, grid(kGrid1_8T, 70)), int64_t(640));
    CHECK_EQ(quantizeTick(170, grid(kGrid1_16T)), int64_t(160));
    CHECK(isTripletGrid(kGrid1_8T) && !isTripletGrid(kGrid1_8));
    CHECK_EQ(gridName(kGrid1_16T), std::string("1/16T"));
}

TEST("quantize: strength moves notes only part of the way") {
    CHECK_EQ(quantizeTick(300, grid(kGrid1_16, 50, 0.5)), int64_t(270));
    CHECK_EQ(quantizeTick(300, grid(kGrid1_16, 50, 0.0)), int64_t(300));
}

TEST("quantize: clips quantize to the song grid, keep lengths, skip hidden notes") {
    MidiClip c{"c", 100, 4 * kBeat, {}, std::nullopt};
    c.notes = {{150, 300, 60, 100},   // abs 250 -> 240 -> clip tick 140
               {-50, 100, 62, 100},   // hidden by a start trim: untouched
               {965, 200, 64, 90}};   // abs 1065 -> 960 -> clip tick 860
    CHECK_EQ(quantizeClip(c, grid(kGrid1_16)), 2);
    CHECK_EQ(c.notes[0].tick, int64_t(-50));
    CHECK_EQ(c.notes[1].tick, int64_t(140));
    CHECK_EQ(c.notes[1].lengthTick, int64_t(300));
    CHECK_EQ(c.notes[2].tick, int64_t(860));

    QuantizeSettings ends = grid(kGrid1_8);
    ends.quantizeLengths = true;
    MidiClip d{"d", 0, 4 * kBeat, {{10, 470, 60, 100}}, std::nullopt};
    quantizeClip(d, ends);
    CHECK_EQ(d.notes[0].tick, int64_t(0));
    CHECK_EQ(d.notes[0].lengthTick, int64_t(480));
}

TEST("quantize: presets cover straight, triplet, sampler swing and drum-machine shuffle") {
    const auto& ps = quantizePresets();
    CHECK(ps.size() >= 15);
    CHECK(findQuantizePreset("straight-1-16") != nullptr);
    CHECK(findQuantizePreset("triplet-1-8") != nullptr);
    auto* mpc = findQuantizePreset("mpc-16-62");
    CHECK(mpc && mpc->settings.gridTicks == kGrid1_16 && mpc->settings.swingPercent == 62.0);
    CHECK(findQuantizePreset("analog-shuffle-heavy") != nullptr);
    for (auto& p : ps) {
        CHECK(!p.name.empty() && !p.description.empty());
        CHECK(p.settings.swingPercent >= 50 && p.settings.swingPercent <= 75);
        CHECK(kBeat * 4 % p.settings.gridTicks == 0);  // every grid divides a 4/4 bar evenly
    }
    // Ids are unique (they're stored in settings).
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j = i + 1; j < ps.size(); ++j) CHECK(ps[i].id != ps[j].id);
}

TEST("time signature: beats and bars follow the signature") {
    Project p;
    p.timeSigNumerator = 6;
    p.timeSigDenominator = 8;
    CHECK_EQ(p.beatTicks(), kBeat / 2);
    CHECK_EQ(p.barTicks(), 3 * kBeat);
    p.timeSigNumerator = 7;
    CHECK_EQ(p.barTicks(), 7 * kBeat / 2);
    p.timeSigNumerator = 5;
    p.timeSigDenominator = 4;
    CHECK_EQ(p.barTicks(), 5 * kBeat);
    p.timeSigDenominator = 3;  // invalid denominator falls back to quarter notes
    CHECK_EQ(p.beatTicks(), kBeat);
}

TEST("time signature: the click follows 6/8 (accent every 6 eighths)") {
    Project p;
    p.tempoBpm = 120;  // quarter = 0.5 s, eighth = 0.25 s = 12000 frames
    p.timeSigNumerator = 6;
    p.timeSigDenominator = 8;
    PackageMediaProvider media(th::tempDir("click68"));
    BuiltinPluginHost host;
    Engine e;
    EngineOptions o;
    o.maxBlock = 480;
    e.prepare(p, media, host, o);
    e.setMetronome(true);
    std::vector<float> l(84000), r(84000);
    for (int done = 0; done < 84000; done += 480) e.process(l.data() + done, r.data() + done, 480);
    auto energy = [&](int from) {
        double s = 0;
        for (int i = from; i < from + 600; ++i) s += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
        return s;
    };
    for (int beat = 0; beat < 7; ++beat) CHECK(energy(beat * 12000) > 0.05);  // a click every eighth
    CHECK(energy(6000) == 0.0);                                                // nothing between
    // Downbeats (beat 0 and 6) use the higher accent pitch: count zero crossings.
    auto crossings = [&](int from) {
        int n = 0;
        for (int i = from + 1; i < from + 600; ++i) n += (l[static_cast<size_t>(i - 1)] < 0) != (l[static_cast<size_t>(i)] < 0);
        return n;
    };
    CHECK(crossings(0) > crossings(12000));
    CHECK(crossings(72000) > crossings(60000));
}

TEST("midi: recording with swing quantize lands notes on the swung grid") {
    Project p;
    p.tempoBpm = 120;
    Track k;
    k.id = "k";
    k.kind = TrackKind::Instrument;
    p.tracks.push_back(k);
    // Played straight 16ths (every 6000 frames = 240 ticks), slightly early.
    std::vector<TimedMidiEvent> ev;
    for (int i = 0; i < 4; ++i) {
        ev.push_back({i * 6000 - (i ? 300 : 0), true, 42, 100});
        ev.push_back({i * 6000 + 2000, false, 42, 0});
    }
    auto q = grid(kGrid1_16, 62);
    auto clip = buildMidiClip(p, ev, 0, 48000, 48000, &q);
    CHECK(clip && clip->notes.size() == 4);
    CHECK_EQ(clip->notes[0].tick, int64_t(0));
    CHECK_EQ(clip->notes[1].tick, int64_t(298));  // 62% of 480
    CHECK_EQ(clip->notes[2].tick, int64_t(480));
    CHECK_EQ(clip->notes[3].tick, int64_t(778));
}
