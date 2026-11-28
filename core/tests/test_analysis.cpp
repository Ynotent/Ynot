#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr double kSr = 48000;
constexpr double kTau = 6.283185307179586;
constexpr int64_t kQ = kTicksPerQuarter;
constexpr int64_t kBar = 4 * kQ;

double hzOf(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

// Piano-ish tones (harmonics, decay) for the given notes, one chord per second.
AudioBuffer chords(const std::vector<std::vector<int>>& seq, double secondsEach = 2.0) {
    AudioBuffer a;
    a.sampleRate = kSr;
    a.channels = 1;
    for (const auto& ch : seq)
        for (int i = 0; i < static_cast<int>(secondsEach * kSr); ++i) {
            const double t = i / kSr;
            double v = 0;
            for (int n : ch)
                for (int h = 1; h <= 4; ++h) v += std::sin(kTau * hzOf(n) * h * t) / (h * h);
            a.samples.push_back(static_cast<float>(0.15 * v * std::exp(-t * 0.8)));
        }
    return a;
}

Project withAudio(const AudioBuffer& a, const std::string& dir) {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;  // 2 s per bar
    Track t;
    t.id = "a";
    t.kind = TrackKind::Audio;
    p.tracks = {t};
    importAudioClip(p, dir, "a", a, 0);
    return p;
}
}  // namespace

TEST("ai: chord names from pitch classes") {
    auto chroma = [](std::vector<int> pcs) {
        Chroma c{};
        for (int x : pcs) c[static_cast<size_t>(x % 12)] += 1;
        return c;
    };
    CHECK_EQ(chordName(chroma({0, 4, 7})), std::string("C"));
    CHECK_EQ(chordName(chroma({9, 0, 4})), std::string("Am"));
    CHECK_EQ(chordName(chroma({7, 11, 2, 5})), std::string("G7"));
    CHECK_EQ(chordName(chroma({5, 9, 0, 4})), std::string("Fmaj7"));
    CHECK_EQ(chordName(chroma({2, 5, 9, 0})), std::string("Dm7"));
    CHECK_EQ(chordName(Chroma{}), std::string("N"));
}

TEST("ai: key detection from MIDI (major and minor)") {
    // I-IV-V-I in G major.
    std::vector<MidiNote> g;
    const std::vector<std::vector<int>> prog = {{55, 59, 62}, {60, 64, 67}, {62, 66, 69}, {55, 59, 62}};
    for (size_t i = 0; i < prog.size(); ++i)
        for (int n : prog[i]) g.push_back({static_cast<int64_t>(i) * kBar, kBar, n, 100});
    auto k = detectKey(chromaFromNotes(g, 0, 4 * kBar));
    CHECK_EQ(k.name(), std::string("G major"));
    CHECK(k.confidence > 0.2);
    // i-iv-V-i in A minor (harmonic minor: G#).
    std::vector<MidiNote> a;
    const std::vector<std::vector<int>> am = {{57, 60, 64}, {62, 65, 69}, {64, 68, 71}, {57, 60, 64}};
    for (size_t i = 0; i < am.size(); ++i)
        for (int n : am[i]) a.push_back({static_cast<int64_t>(i) * kBar, kBar, n, 100});
    auto m = detectKey(chromaFromNotes(a, 0, 4 * kBar));
    CHECK_EQ(m.name(), std::string("A minor"));
    CHECK_EQ(m.tuneScale(), 2);
}

TEST("ai: key and chords of a recorded (audio) progression") {
    auto dir = th::tempDir("ai-chords");
    // C - Am - F - G, one bar each, recorded as audio.
    Project p = withAudio(chords({{48, 60, 64, 67}, {45, 57, 60, 64}, {41, 57, 60, 65}, {43, 55, 59, 62}}), dir);
    PackageMediaProvider media(dir);
    auto key = detectSongKey(p, media);
    CHECK_EQ(key.name(), std::string("C major"));
    auto ch = detectChords(p, media, kBar);
    CHECK_EQ(ch.size(), size_t(4));
    const std::vector<std::string> want = {"C", "Am", "F", "G"};
    for (size_t i = 0; i < ch.size() && i < 4; ++i) {
        CHECK_EQ(ch[i].name, want[i]);
        CHECK_EQ(ch[i].tick, static_cast<int64_t>(i) * kBar);
    }
    // Drums are left out of the harmony.
    Track d;
    d.id = "d";
    d.kind = TrackKind::Instrument;
    d.instrument = makeBuiltin(kBuiltinDrums, "Ynot Drums");
    d.midiClips = {{"m", 0, 4 * kBar, {{0, 4 * kBar, 37, 127}, {0, 4 * kBar, 42, 127}}, std::nullopt, {}}};
    p.tracks.push_back(d);
    CHECK(isPercussionTrack(p.tracks.back()));
    CHECK_EQ(detectSongKey(p, media).name(), std::string("C major"));
}

TEST("ai: tempo detection of a beat") {
    for (double bpm : {97.0, 128.0, 140.0}) {
        AudioBuffer a;
        a.sampleRate = 44100;
        a.channels = 1;
        a.samples.assign(static_cast<size_t>(10 * a.sampleRate), 0.f);
        const double beat = 60.0 / bpm;
        uint32_t seed = 3;
        for (double t = 0.1; t < 9.8; t += beat)
            for (int i = 0; i < 2000; ++i) {
                seed = seed * 1664525u + 1013904223u;
                const double noise = static_cast<double>(seed >> 8) / 16777216.0 - 0.5;
                a.samples[static_cast<size_t>(t * a.sampleRate) + static_cast<size_t>(i)] +=
                    static_cast<float>(noise * std::exp(-i / 300.0));
            }
        auto est = detectTempo(a);
        CHECK(std::abs(est.bpm - bpm) < 0.6);
    }
    AudioBuffer shortOne;
    shortOne.samples.assign(1000, 0.f);
    CHECK_EQ(detectTempo(shortOne).bpm, 0.0);
}

TEST("ai: audio to MIDI transcribes a sung melody") {
    auto dir = th::tempDir("ai-melody");
    // C4 E4 G4 E4, half a second each (one beat at 120), with a little vibrato.
    AudioBuffer a;
    a.sampleRate = kSr;
    a.channels = 1;
    const int mel[4] = {60, 64, 67, 64};
    double phase = 0;
    for (int n = 0; n < 4; ++n)
        for (int i = 0; i < static_cast<int>(0.5 * kSr); ++i) {
            const double t = i / kSr;
            const double hz = hzOf(mel[n]) * std::pow(2.0, 0.2 / 12 * std::sin(kTau * 5 * t));
            phase += kTau * hz / kSr;
            const double env = std::min(1.0, t / 0.01) * std::min(1.0, (0.5 - t) / 0.03);
            a.samples.push_back(static_cast<float>(0.3 * env * (std::sin(phase) + 0.4 * std::sin(2 * phase))));
        }
    Project p = withAudio(a, dir);
    auto notes = transcribeMelody(a, p, 0);
    CHECK_EQ(notes.size(), size_t(4));
    for (size_t i = 0; i < notes.size() && i < 4; ++i) {
        CHECK_EQ(notes[i].pitch, mel[i]);
        CHECK(std::llabs(notes[i].tick - static_cast<int64_t>(i) * kQ) < 40);
        CHECK(std::llabs(notes[i].lengthTick - kQ) < 80);
    }
    PackageMediaProvider media(dir);
    auto id = audioClipToMidi(p, media, "a", p.tracks[0].audioClips[0].id);
    CHECK(id.has_value());
    const Track* t = p.findTrack(*id);
    CHECK(t && t->instrument && t->midiClips.size() == 1 && t->midiClips[0].notes.size() == 4);
}
