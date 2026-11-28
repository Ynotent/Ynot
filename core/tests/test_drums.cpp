#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr int64_t kQ = kTicksPerQuarter;
constexpr double kSr = 48000;

AudioBuffer hit(int note, double seconds, std::vector<MidiEvent> extra = {}) {
    auto kit = makeDrumKit(Json::object(), kSr);
    AudioBuffer out;
    out.sampleRate = kSr;
    out.channels = 2;
    const auto frames = static_cast<int>(seconds * kSr);
    std::vector<float> l(512), r(512);
    for (int done = 0; done < frames; done += 512) {
        std::fill(l.begin(), l.end(), 0.f);
        std::fill(r.begin(), r.end(), 0.f);
        std::vector<MidiEvent> ev;
        if (done == 0) ev.push_back({0, true, note, 110});
        for (auto e : extra)
            if (e.frameOffset >= done && e.frameOffset < done + 512) ev.push_back({e.frameOffset - done, e.noteOn, e.pitch, e.velocity});
        kit->process(l.data(), r.data(), 512, ev.data(), static_cast<int>(ev.size()));
        for (int i = 0; i < 512; ++i) {
            out.samples.push_back(l[static_cast<size_t>(i)]);
            out.samples.push_back(r[static_cast<size_t>(i)]);
        }
    }
    return out;
}

double crossingsPerSecond(const AudioBuffer& b, int64_t from, int64_t to) {
    int64_t n = 0;
    for (int64_t f = from + 1; f < to; ++f)
        n += (b.samples[static_cast<size_t>((f - 1) * 2)] < 0) != (b.samples[static_cast<size_t>(f * 2)] < 0);
    return static_cast<double>(n) * kSr / static_cast<double>(to - from);
}

}  // namespace

TEST("drums: every kit sound plays, stays in range and dies away") {
    CHECK_EQ(drumKitSounds().size(), size_t(10));
    for (const auto& d : drumKitSounds()) {
        auto b = hit(d.note, 2.5);
        float peak = 0;
        for (float v : b.samples) peak = std::max(peak, std::abs(v));
        CHECK(peak > 0.05f);
        CHECK(peak < 1.0f);
        CHECK(th::rms(b, b.frames() - 4800, b.frames()) < 1e-4);  // silent by the end
    }
    CHECK_EQ(std::string(drumName(36)), std::string("Kick"));
    CHECK(drumName(60) == nullptr);
    // Unknown notes and note-offs are ignored.
    auto none = hit(60, 0.2);
    CHECK(th::rms(none) < 1e-9);
}

TEST("drums: kick is low, hats are bright; a closed hat chokes the open hat") {
    auto kick = hit(36, 0.3), hat = hit(42, 0.1);
    CHECK(crossingsPerSecond(kick, 2400, 9600) < 200);   // ~45-60 Hz body
    CHECK(crossingsPerSecond(hat, 0, 2400) > 4000);      // mostly above 7 kHz
    auto open = hit(46, 0.6);
    auto choked = hit(46, 0.6, {{4800, true, 42, 100}});  // closed hat 0.1 s later
    CHECK(th::rms(open, 14400, 19200) > 1e-3);
    CHECK(th::rms(choked, 14400, 19200) < th::rms(open, 14400, 19200) * 0.2);
}

TEST("step patterns: steps become notes that play in time; swing and grid edits keep the beat") {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    const std::string drums = addDrumTrack(p, 0, 1);
    const Track* t = p.findTrack(drums);
    CHECK(t && t->instrument && t->instrument->uid == std::string(kBuiltinDrums));
    CHECK_EQ(t->midiClips.size(), size_t(1));
    const std::string clip = t->midiClips[0].id;
    const auto& pat = *t->midiClips[0].steps;
    CHECK_EQ(stepCount(pat, t->midiClips[0].lengthTick), 16);
    CHECK_EQ(pat.rows.size(), size_t(10));

    for (int s : {0, 4, 8, 12}) CHECK(setStep(p, drums, clip, 36, s, 120));  // four on the floor
    CHECK(setStep(p, drums, clip, 38, 4, 100));
    CHECK(setStep(p, drums, clip, 38, 12, 100));
    CHECK(!setStep(p, drums, clip, 36, 16, 100));  // past the end
    auto clipRef = [&]() -> const MidiClip& { return p.findTrack(drums)->midiClips[0]; };
    CHECK_EQ(clipRef().notes.size(), size_t(6));
    CHECK_EQ(stepVelocity(clipRef(), 36, 8), 120);
    CHECK_EQ(stepVelocity(clipRef(), 38, 8), 0);
    CHECK(setStep(p, drums, clip, 36, 8, 0));  // off
    CHECK_EQ(stepVelocity(clipRef(), 36, 8), 0);
    CHECK(setStep(p, drums, clip, 36, 8, 120));

    // Plays in time through the engine: removing the kick on step 4 (beat 2) changes the sound
    // from exactly 0.5 s on (120 BPM), not a sample earlier.
    BuiltinPluginHost host;
    PackageMediaProvider media(th::tempDir("drums"));
    const auto with = renderProject(p, media, host, kSr, 0.5);
    Project noKick = p;
    CHECK(setStep(noKick, drums, clip, 36, 4, 0));
    const auto without = renderProject(noKick, media, host, kSr, 0.5);
    int64_t firstDiff = -1;
    for (int64_t f = 0; f < with.frames() && firstDiff < 0; ++f)
        if (std::abs(with.samples[static_cast<size_t>(f * 2)] - without.samples[static_cast<size_t>(f * 2)]) > 1e-6f)
            firstDiff = f;
    CHECK(std::abs(firstDiff - 24000) <= 1);

    // Swing moves every second step; the step editor still finds it.
    CHECK(setStep(p, drums, clip, 42, 1, 90));
    CHECK(setPatternSwing(p, drums, clip, 66));
    const int64_t swung = stepTick(*clipRef().steps, 1);
    CHECK_EQ(swung, kQ / 4 + static_cast<int64_t>(std::llround(2.0 * (kQ / 4) * 0.16)));
    bool found = false;
    for (auto& n : clipRef().notes) found |= n.pitch == 42 && n.tick == swung;
    CHECK(found);
    CHECK_EQ(stepVelocity(clipRef(), 42, 1), 90);
    CHECK(setPatternSwing(p, drums, clip, 50));

    // Finer grid: same beat, more steps.
    CHECK(setPatternStepTicks(p, drums, clip, kQ / 8));
    CHECK_EQ(stepCount(*clipRef().steps, clipRef().lengthTick), 32);
    CHECK_EQ(stepVelocity(clipRef(), 36, 8), 120);  // beat 2 is now step 8 of 32
    CHECK_EQ(stepVelocity(clipRef(), 42, 2), 90);
    CHECK(setPatternStepTicks(p, drums, clip, kQ / 4));

    // Two bars: the first bar repeats.
    CHECK(setPatternBars(p, drums, clip, 2));
    CHECK_EQ(clipRef().lengthTick, 2 * p.barTicks());
    CHECK_EQ(stepVelocity(clipRef(), 36, 16), 120);
    CHECK_EQ(stepVelocity(clipRef(), 38, 28), 100);
    CHECK(clearPatternRow(p, drums, clip, 38));
    CHECK_EQ(stepVelocity(clipRef(), 38, 4), 0);

    // Saved and reloaded with its grid.
    auto dir = th::tempDir("drums-save");
    p.name = "beat";
    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK(projectToJson(q) == projectToJson(p));
    CHECK(q.findTrack(drums)->midiClips[0].steps.has_value());
}
