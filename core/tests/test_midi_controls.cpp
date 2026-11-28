#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr double kSr = 48000;

// Runs Ynot Synth (sine) through `seconds`, applying events at absolute frames.
std::vector<float> synth(const std::vector<std::pair<int64_t, MidiEvent>>& events, double seconds) {
    BuiltinPluginHost host;
    auto inst = makeBuiltin(kBuiltinSynth, "Synth");
    inst.params["wave"] = 0.0;  // sine, for clean pitch measurement
    inst.params["release"] = 0.05;
    auto s = host.load(inst, kSr, 512);
    std::vector<float> out;
    std::vector<float> l(512), r(512);
    const auto total = static_cast<int64_t>(seconds * kSr);
    for (int64_t done = 0; done < total; done += 512) {
        std::fill(l.begin(), l.end(), 0.f);
        std::fill(r.begin(), r.end(), 0.f);
        std::vector<MidiEvent> block;
        for (const auto& [f, e] : events)
            if (f >= done && f < done + 512) {
                auto ev = e;
                ev.frameOffset = static_cast<int>(f - done);
                block.push_back(ev);
            }
        s->process(l.data(), r.data(), 512, block.data(), static_cast<int>(block.size()));
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

double pitchAt(const std::vector<float>& x, double a, double b) {
    const auto i = static_cast<size_t>(a * kSr), j = static_cast<size_t>(b * kSr);
    return detectPitchHz(x.data() + i, static_cast<int>(j - i), kSr);
}

double rmsAt(const std::vector<float>& x, double a, double b) {
    double s = 0;
    const auto i = static_cast<size_t>(a * kSr), j = static_cast<size_t>(b * kSr);
    for (size_t k = i; k < j; ++k) s += static_cast<double>(x[k]) * x[k];
    return std::sqrt(s / static_cast<double>(j - i));
}
}  // namespace

TEST("midi controls: pitch bend moves the synth up to +-2 semitones and back") {
    auto out = synth({{0, MidiEvent(0, true, 69, 100)},
                      {24000, MidiEvent::bend(0, 16383)},
                      {48000, MidiEvent::bend(0, 0)},
                      {72000, MidiEvent::bend(0, kPitchBendCentre)}},
                     2.0);
    CHECK_NEAR(pitchAt(out, 0.1, 0.45), 440.0, 1.5);
    CHECK_NEAR(pitchAt(out, 0.6, 0.95), 440.0 * std::pow(2.0, 2.0 * 8191.0 / 8192.0 / 12.0), 2.0);
    CHECK_NEAR(pitchAt(out, 1.1, 1.45), 440.0 * std::pow(2.0, -2.0 / 12.0), 1.5);
    CHECK_NEAR(pitchAt(out, 1.6, 1.95), 440.0, 1.5);
}

TEST("midi controls: the sustain pedal holds released notes until it comes up") {
    auto out = synth({{0, MidiEvent::control(0, kCcSustain, 127)},
                      {0, MidiEvent(0, true, 60, 100)},
                      {9600, MidiEvent(0, false, 60, 0)},            // key released at 0.2 s
                      {48000, MidiEvent::control(0, kCcSustain, 0)}},  // pedal up at 1 s
                     1.5);
    CHECK(rmsAt(out, 0.5, 0.9) > 0.05);   // still ringing on the pedal
    CHECK(rmsAt(out, 1.2, 1.5) < 1e-4);   // released after the pedal
    auto plain = synth({{0, MidiEvent(0, true, 60, 100)}, {9600, MidiEvent(0, false, 60, 0)}}, 1.0);
    CHECK(rmsAt(plain, 0.5, 0.9) < 1e-4);  // no pedal: stops with the key
}

TEST("midi controls: the mod wheel adds vibrato") {
    auto still = synth({{0, MidiEvent(0, true, 69, 100)}}, 1.0);
    auto wobbly = synth({{0, MidiEvent(0, true, 69, 100)}, {0, MidiEvent::control(0, kCcModWheel, 127)}}, 1.0);
    auto spread = [](const std::vector<float>& x) {
        double lo = 1e9, hi = 0;
        for (double t = 0.1; t < 0.9; t += 0.05) {
            const double p = pitchAt(x, t, t + 0.04);
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
        return hi - lo;
    };
    CHECK(spread(still) < 1.0);
    CHECK(spread(wobbly) > 4.0);
}

TEST("midi controls: bends and pedal are recorded, saved and played back") {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    MidiTakeRecorder rec;
    rec.start(0);
    const MidiEvent block1[] = {MidiEvent(0, true, 69, 100), MidiEvent::control(10, kCcSustain, 127)};
    rec.capture(block1, 2, 0);
    const MidiEvent block2[] = {MidiEvent::bend(0, 16383)};
    rec.capture(block2, 1, 24000);  // 0.5 s = 1 beat
    const MidiEvent block3[] = {MidiEvent(0, false, 69, 0), MidiEvent::bend(5, kPitchBendCentre)};
    rec.capture(block3, 2, 48000);
    auto clip = buildMidiClip(p, rec.finish(), 0, 96000, kSr);
    CHECK(clip.has_value());
    CHECK_EQ(clip->controls.size(), size_t(3));
    CHECK_EQ(clip->controls[1].tick, kTicksPerQuarter);
    CHECK(clip->controls[1].kind == MidiControl::Kind::PitchBend);
    CHECK_EQ(clip->controls[1].value, 16383);

    Track t;
    t.id = "k";
    t.kind = TrackKind::Instrument;
    t.instrument = makeBuiltin(kBuiltinSynth, "Synth");
    t.instrument->params["wave"] = 0.0;
    t.midiClips = {*clip};
    p.tracks = {t};
    auto dir = th::tempDir("ctl");
    p.name = "c";
    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK(projectToJson(q) == projectToJson(p));
    CHECK_EQ(q.tracks[0].midiClips[0].controls.size(), size_t(3));

    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    auto out = renderProject(q, media, host, kSr, 0.0);
    std::vector<float> left;
    for (int64_t f = 0; f < out.frames(); ++f) left.push_back(out.samples[static_cast<size_t>(f * 2)]);
    CHECK_NEAR(pitchAt(left, 0.1, 0.45), 440.0, 1.5);
    CHECK(pitchAt(left, 0.6, 0.95) > 490.0);  // bent up two semitones during beat 2

    // Splitting keeps controls at the same place in the song.
    CHECK(splitClip(q, "k", q.tracks[0].midiClips[0].id, kTicksPerQuarter / 2).has_value());
    const auto& right = q.tracks[0].midiClips[1];
    CHECK_EQ(right.controls[1].tick, kTicksPerQuarter / 2);
}
