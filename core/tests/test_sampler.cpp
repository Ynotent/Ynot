#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr double kSr = 48000;
constexpr double kTau = 6.283185307179586;

// Four decaying "hits" at 0, 0.5, 1.0 and 1.5 s, each a different pitch (so we can tell them apart).
AudioBuffer fourHits() {
    AudioBuffer a;
    a.sampleRate = kSr;
    a.channels = 2;
    const double hz[4] = {220, 330, 440, 660};
    a.samples.assign(static_cast<size_t>(2 * kSr * 2), 0.f);
    for (int h = 0; h < 4; ++h)
        for (int i = 0; i < static_cast<int>(0.4 * kSr); ++i) {
            const double t = i / kSr;
            const auto v = static_cast<float>(0.6 * std::exp(-t * 6) * std::sin(kTau * hz[h] * t));
            const size_t f = static_cast<size_t>(h * 24000 + i);
            a.samples[f * 2] = v;
            a.samples[f * 2 + 1] = v;
        }
    return a;
}

AudioBuffer tone(double hz, double seconds) {
    AudioBuffer a;
    a.sampleRate = 44100;  // a different rate from the song: must still play in tune
    a.channels = 1;
    for (int i = 0; i < static_cast<int>(seconds * a.sampleRate); ++i)
        a.samples.push_back(static_cast<float>(0.5 * std::sin(kTau * hz * i / a.sampleRate)));
    return a;
}

double pitchIn(const AudioBuffer& out, double fromSec, double toSec) {
    std::vector<float> m;
    for (auto f = static_cast<int64_t>(fromSec * out.sampleRate); f < static_cast<int64_t>(toSec * out.sampleRate); ++f)
        m.push_back(out.samples[static_cast<size_t>(f * out.channels)]);
    return detectPitchHz(m.data(), static_cast<int>(m.size()), out.sampleRate);
}

Project song() {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    return p;
}
}  // namespace

TEST("sampler: auto-chop finds each hit; equal slices divide evenly") {
    auto s = detectSlices(fourHits(), 16);
    CHECK_EQ(s.size(), size_t(4));
    // Each slice starts within 2 ms before its hit (never after: the attack must be whole).
    for (size_t i = 0; i < s.size() && i < 4; ++i) {
        const int64_t d = static_cast<int64_t>(24000 * i) - s[i];
        CHECK(d >= 0 && d <= 96);
    }
    CHECK_EQ(detectSlices(fourHits(), 2).size(), size_t(2));  // capped
    auto e = equalSlices(96000, 4);
    CHECK(e == std::vector<int64_t>({0, 24000, 48000, 72000}));
    AudioBuffer silent;
    silent.samples.assign(96000, 0.f);
    CHECK(detectSlices(silent).size() == size_t(1));
}

TEST("sampler: pads play their slice (one-shot); keys mode plays in tune across the keyboard") {
    auto dir = th::tempDir("sampler");
    Project p = song();
    const auto id = addSamplerTrack(p, dir, fourHits(), "Chops");
    const auto* s = samplerOf(p, id);
    CHECK(s != nullptr);
    CHECK_EQ(samplerSlices(*s).size(), size_t(4));
    // Pad 2 (note 37) at beat 1, pad 4 (note 39) at beat 3: short notes, but slices play out.
    p.findTrack(id)->midiClips.push_back({"m", 0, 4 * kTicksPerQuarter,
                                          {{0, 20, 37, 120}, {2 * kTicksPerQuarter, 20, 39, 120}}, std::nullopt, {}});
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    auto out = renderProject(p, media, host, kSr, 0.5);
    CHECK(std::abs(pitchIn(out, 0.05, 0.25) - 330) < 3);  // slice 2, still sounding after note-off
    CHECK(std::abs(pitchIn(out, 1.05, 1.25) - 660) < 5);
    CHECK(th::rms(out, static_cast<int64_t>(0.6 * kSr), static_cast<int64_t>(0.95 * kSr)) < 1e-4);  // slice ended

    // Keys: A440 sample, root A4; play A5 and E5.
    Project k = song();
    const auto kid = addSamplerTrack(k, dir, tone(440, 1.0), "Keys");
    samplerOf(k, kid)->params["mode"] = "keys";
    samplerOf(k, kid)->params["rootNote"] = 69;
    k.findTrack(kid)->midiClips.push_back({"m", 0, 4 * kTicksPerQuarter,
                                           {{0, kTicksPerQuarter, 81, 100}, {2 * kTicksPerQuarter, kTicksPerQuarter, 76, 100}},
                                           std::nullopt, {}});
    auto ko = renderProject(k, media, host, kSr, 0.5);
    CHECK(std::abs(pitchIn(ko, 0.1, 0.4) - 880) < 6);
    CHECK(std::abs(pitchIn(ko, 1.1, 1.4) - 659.26) < 5);
    CHECK(th::rms(ko, static_cast<int64_t>(0.75 * kSr), static_cast<int64_t>(0.95 * kSr)) < 1e-4);  // released
}

TEST("sampler: slice edits, save/load, and a missing sample is reported, not a crash") {
    auto dir = th::tempDir("sampler-edit");
    Project p = song();
    const auto id = addSamplerTrack(p, dir, fourHits(), "Chops");
    CHECK(moveSlice(p, id, 1, 1000));
    CHECK_EQ(samplerSlices(*samplerOf(p, id))[1], int64_t(1000));
    const int64_t next = samplerSlices(*samplerOf(p, id))[2];
    CHECK(moveSlice(p, id, 1, 60000));  // can't pass its neighbour
    CHECK_EQ(samplerSlices(*samplerOf(p, id))[1], next - 1);
    CHECK(!moveSlice(p, id, 0, 10));  // the first slice always starts at 0
    CHECK(addSlice(p, id, 90000));
    CHECK_EQ(samplerSlices(*samplerOf(p, id)).size(), size_t(5));
    CHECK(removeSlice(p, id, 4));
    CHECK(!removeSlice(p, id, 0));
    CHECK(setSlices(p, id, {500, 0, 500, 200000}));  // sorted, deduped, out-of-range dropped
    CHECK(samplerSlices(*samplerOf(p, id)) == std::vector<int64_t>({0, 500}));

    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK(projectToJson(q) == projectToJson(p));
    const auto mediaId = samplerOf(q, id)->params.getString("mediaId", "");
    CHECK(q.findMedia(mediaId) != nullptr && q.findMedia(mediaId)->origin == "sample");

    // On a device that hasn't synced the sample yet.
    Project r = q;
    for (auto& m : r.media) m.fileName = "missing.wav";
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    Engine e;
    EngineOptions o;
    e.prepare(r, media, host, o);
    bool reported = false;
    for (const auto& i : e.issues()) reported |= i.message.find("sample") != std::string::npos;
    CHECK(reported);
}

TEST("sampler: an audio clip can be sent to a new sampler") {
    auto dir = th::tempDir("sampler-clip");
    Project p = song();
    Track a;
    a.id = "a";
    a.kind = TrackKind::Audio;
    p.tracks = {a};
    const auto clip = importAudioClip(p, dir, "a", fourHits(), 0);
    CHECK(trimClipStart(p, "a", clip, kTicksPerQuarter));  // drop the first hit (0.5 s)
    PackageMediaProvider media(dir);
    const auto audio = clipAudio(p, media, "a", clip);
    CHECK_EQ(audio.frames(), int64_t(72000));
    const auto id = addSamplerTrack(p, dir, audio, "From clip");
    CHECK_EQ(samplerSlices(*samplerOf(p, id)).size(), size_t(3));
}
