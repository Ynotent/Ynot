#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr double kSr = 48000;
constexpr double kTau = 6.283185307179586;
double hzOf(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

// A "reference song": C - Am - F - G at 100 BPM with a kick on every beat and a melody on top.
AudioBuffer referenceSong() {
    AudioBuffer a;
    a.sampleRate = kSr;
    a.channels = 1;
    const double bpm = 100, beat = 60.0 / bpm;
    const std::vector<std::vector<int>> chords = {{48, 60, 64, 67}, {45, 57, 60, 64}, {41, 57, 60, 65}, {43, 55, 59, 62}};
    const int mel[8] = {76, 74, 72, 74, 76, 76, 76, 79};
    const int bars = 8;
    a.samples.assign(static_cast<size_t>(bars * 4 * beat * kSr), 0.f);
    for (size_t i = 0; i < a.samples.size(); ++i) {
        const double t = i / kSr;
        const int bar = static_cast<int>(t / (4 * beat)) % 4;
        double v = 0;
        for (int n : chords[static_cast<size_t>(bar)]) v += 0.05 * std::sin(kTau * hzOf(n) * t);
        const int b = static_cast<int>(t / beat);
        const double tb = t - b * beat;
        v += 0.5 * std::exp(-tb * 25) * std::sin(kTau * 60 * tb);  // kick
        v += 0.04 * std::sin(kTau * hzOf(mel[b % 8]) * t) * std::min(1.0, tb / 0.01);
        a.samples[i] = static_cast<float>(v);
    }
    return a;
}
}  // namespace

TEST("inspire: the reference's tempo, key and chord movement are read") {
    auto r = analyzeReference(referenceSong());
    CHECK(std::abs(r.bpm - 100) < 1.0);
    CHECK_EQ(r.key.name(), std::string("C major"));
    CHECK(r.chords.size() >= 3);
    if (r.chords.size() >= 3) {
        CHECK_EQ(r.chords[0], std::string("C"));
        CHECK_EQ(r.chords[1], std::string("Am"));
        CHECK_EQ(r.chords[2], std::string("F"));
    }
    // The melody on top was heard: E D C D E E E G repeating.
    CHECK(r.melody.size() >= 8);
    std::vector<MidiNote> sung;
    const int mel[8] = {76, 74, 72, 74, 76, 76, 76, 79};
    for (int b = 0; b < 16; ++b) sung.push_back({b * kTicksPerQuarter, kTicksPerQuarter, mel[b % 8], 100});
    CHECK(melodySimilarity(sung, r.melody) > 0.5);
}

TEST("inspire: ideas are new parts in the reference's key and tempo; seeds give different ideas") {
    ReferenceProfile r;
    r.bpm = 92;
    r.key.tonic = 9;
    r.key.minor = true;
    r.chords = {"Am", "F", "C", "G"};
    auto a = generateIdea(r, 1), b = generateIdea(r, 2), again = generateIdea(r, 1);
    CHECK_EQ(a.bars, 4);
    CHECK(!a.chords.empty() && !a.bass.empty() && !a.melody.empty() && !a.drums.empty());
    CHECK(a.melody.size() == again.melody.size());  // same seed, same idea (every platform)
    bool same = a.melody.size() == b.melody.size();
    for (size_t i = 0; same && i < a.melody.size(); ++i) same = a.melody[i].pitch == b.melody[i].pitch && a.melody[i].tick == b.melody[i].tick;
    CHECK(!same);
    // Everything in the melody is in A minor and within the 4 bars.
    const int aMinor[7] = {9, 11, 0, 2, 4, 5, 7};
    for (const auto& n : a.melody) {
        CHECK(std::find(std::begin(aMinor), std::end(aMinor), n.pitch % 12) != std::end(aMinor));
        CHECK(n.tick >= 0 && n.tick + n.lengthTick <= 4 * 4 * kTicksPerQuarter);
    }
    CHECK(a.description.find("A minor") != std::string::npos);
    CHECK(a.description.find("92 BPM") != std::string::npos);
}

TEST("inspire: the originality check catches a copied melody, and ideas are steered away from it") {
    ReferenceProfile r;
    r.bpm = 120;
    r.key.tonic = 0;
    auto idea = generateIdea(r, 7);
    // Identical, transposed, or at another tempo: still the same melody.
    CHECK_NEAR(melodySimilarity(idea.melody, idea.melody), 1.0, 1e-9);
    auto moved = idea.melody;
    for (auto& n : moved) n.pitch += 5, n.tick *= 2, n.lengthTick *= 2;
    CHECK(melodySimilarity(idea.melody, moved) > 0.9);
    // Other ideas are clearly different.
    for (uint32_t s = 20; s < 30; ++s) CHECK(melodySimilarity(idea.melody, generateIdea(r, s).melody) < 0.6);
    // If the reference's melody *is* what seed 7 would write, a different idea is chosen.
    r.melody = idea.melody;
    auto safe = generateOriginalIdea(r, 7);
    CHECK(safe.seed != 7);
    CHECK(safe.similarity < kMaxSimilarity);
}

TEST("inspire: ideas become editable tracks, or a seamless audio loop for the sampler") {
    ReferenceProfile r;
    r.bpm = 90;
    r.key.tonic = 2;
    r.key.minor = true;
    auto idea = generateIdea(r, 3);
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    auto ids = addIdeaTracks(p, idea, 0, true);
    CHECK_EQ(ids.size(), size_t(4));
    CHECK_NEAR(p.tempoBpm, 90.0, 1e-9);
    const Track* drums = p.findTrack(ids[3]);
    CHECK(drums && drums->midiClips.size() == 1 && drums->midiClips[0].steps.has_value());
    CHECK_EQ(drums->midiClips[0].notes.size(), idea.drums.size());
    auto audio = renderIdea(idea, 48000);
    CHECK_EQ(audio.frames(), int64_t(std::llround(4 * 4 * 60.0 / 90.0 * 48000)));
    CHECK(th::rms(audio) > 0.01);
}
