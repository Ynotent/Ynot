#include "ynot/inspire.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "ynot/demo.hpp"
#include "ynot/drums.hpp"
#include "ynot/edit.hpp"
#include "ynot/engine.hpp"
#include "ynot/sampler.hpp"
#include "fft_internal.hpp"

namespace ynot {

namespace {

constexpr int64_t kQ = kTicksPerQuarter;
constexpr int64_t kStep = kQ / 4;  // 1/16
constexpr int64_t kBar = 16 * kStep;
constexpr int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
constexpr int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
const char* const kPcNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

// Same numbers on every platform (std distributions differ between standard libraries).
class Rng {
public:
    explicit Rng(uint32_t seed) : s_(seed * 2654435761u + 0x9E3779B9u) { next(); }
    uint32_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return s_;
    }
    int below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint32_t>(n)); }
    double unit() { return (next() >> 8) / 16777216.0; }
    bool chance(double p) { return unit() < p; }

private:
    uint32_t s_;
};

enum Quality { Major, Minor, Dim };
struct Chord {
    int root = 0;  // pitch class
    Quality q = Major;
    bool seventh = false;
    int seventhPc = -1;
};

std::string nameOf(const Chord& c) {
    std::string n = kPcNames[c.root];
    if (c.q == Minor) n += "m";
    if (c.q == Dim) n += "dim";
    if (c.seventh) {
        const int iv = (c.seventhPc - c.root + 12) % 12;
        n += iv == 11 ? (c.q == Minor ? "(maj7)" : "maj7") : "7";
    }
    return n;
}

std::vector<int> tonesOf(const Chord& c) {
    std::vector<int> t = {c.root, (c.root + (c.q == Major ? 4 : 3)) % 12, (c.root + (c.q == Dim ? 6 : 7)) % 12};
    if (c.seventh && c.seventhPc >= 0) t.push_back(c.seventhPc);
    return t;
}

const int* scaleOf(const KeyEstimate& k) { return k.minor ? kMinor : kMajor; }

// The triad (and optional 7th) built on scale degree `deg` of the key.
Chord diatonic(const KeyEstimate& k, int deg, bool seventh) {
    const int* sc = scaleOf(k);
    auto pc = [&](int d) { return (k.tonic + sc[((d % 7) + 7) % 7]) % 12; };
    Chord c;
    c.root = pc(deg);
    const int third = (pc(deg + 2) - c.root + 12) % 12, fifth = (pc(deg + 4) - c.root + 12) % 12;
    c.q = third == 4 ? Major : fifth == 6 ? Dim : Minor;
    c.seventh = seventh && c.q != Dim;
    c.seventhPc = pc(deg + 6);
    return c;
}

bool parseChord(const std::string& name, Chord& out) {
    if (name.empty() || name == "N") return false;
    static const std::pair<const char*, int> roots[] = {{"C#", 1}, {"Db", 1}, {"D#", 3}, {"Eb", 3}, {"F#", 6}, {"Gb", 6},
                                                        {"G#", 8}, {"Ab", 8}, {"A#", 10}, {"Bb", 10}, {"C", 0}, {"D", 2},
                                                        {"E", 4},  {"F", 5},  {"G", 7},  {"A", 9},  {"B", 11}};
    for (auto [r, pc] : roots)
        if (name.rfind(r, 0) == 0) {
            const std::string rest = name.substr(std::string(r).size());
            out.root = pc;
            out.q = rest.rfind("dim", 0) == 0 ? Dim : (rest.rfind("m", 0) == 0 && rest.rfind("maj", 0) != 0) ? Minor : Major;
            return true;
        }
    return false;
}

// Degree of a pitch class in the key, or -1.
int degreeOf(const KeyEstimate& k, int pc) {
    const int* sc = scaleOf(k);
    for (int d = 0; d < 7; ++d)
        if ((k.tonic + sc[d]) % 12 == pc) return d;
    return -1;
}

int nearestOctave(int pc, int target) {
    int best = pc, dist = 1000;
    for (int n = pc; n < 128; n += 12)
        if (std::abs(n - target) < dist) dist = std::abs(n - target), best = n;
    return best;
}

std::vector<int> scalePitches(const KeyEstimate& k, int lo, int hi) {
    std::vector<int> out;
    for (int n = lo; n <= hi; ++n)
        if (degreeOf(k, n % 12) >= 0) out.push_back(n);
    return out;
}

std::vector<float> monoOf(const AudioBuffer& a, int64_t from, int64_t n) {
    std::vector<float> m;
    const int ch = std::max(1, a.channels);
    for (int64_t f = from; f < std::min(a.frames(), from + n); ++f) {
        float s = 0;
        for (int c = 0; c < ch; ++c) s += a.samples[static_cast<size_t>(f * ch + c)];
        m.push_back(s / static_cast<float>(ch));
    }
    return m;
}

AudioBuffer slice(const AudioBuffer& a, int64_t from, int64_t n) {
    AudioBuffer out;
    out.sampleRate = a.sampleRate;
    out.channels = a.channels;
    from = std::clamp<int64_t>(from, 0, a.frames());
    n = std::clamp<int64_t>(n, 0, a.frames() - from);
    out.samples.assign(a.samples.begin() + from * a.channels, a.samples.begin() + (from + n) * a.channels);
    return out;
}

// The highest clear line in a full mix (the lead vocal or melody usually sits on top of the
// chords): per 20 ms, harmonic salience of each note from G3 to C7; the highest note that is
// strong and isn't just an overtone of a stronger lower note wins.
std::vector<MidiNote> topLine(const AudioBuffer& a, double bpm) {
    std::vector<MidiNote> notes;
    const auto x = monoOf(a, 0, a.frames());
    const int N = 4096;
    const auto hop = static_cast<int64_t>(a.sampleRate * 0.02);
    if (static_cast<int64_t>(x.size()) < N || hop <= 0) return notes;
    const detail::Fft fft(N);
    std::vector<std::complex<double>> buf(static_cast<size_t>(N));
    std::vector<double> mag(static_cast<size_t>(N / 2));
    const double binHz = a.sampleRate / N;
    auto magAt = [&](double f) {
        const auto k = static_cast<int>(std::lround(f / binHz));
        double m = 0;
        for (int d = -1; d <= 1; ++d)
            if (k + d > 0 && k + d < N / 2) m = std::max(m, mag[static_cast<size_t>(k + d)]);
        return m;
    };
    std::vector<int> frameNote;
    std::vector<double> frameLevel;
    double loudest = 0;
    for (int64_t start = 0; start + N <= static_cast<int64_t>(x.size()); start += hop) {
        for (int i = 0; i < N; ++i)
            buf[static_cast<size_t>(i)] = x[static_cast<size_t>(start + i)] * (0.5 - 0.5 * std::cos(6.283185307179586 * i / N));
        fft.run(buf, false);
        for (int k = 0; k < N / 2; ++k) mag[static_cast<size_t>(k)] = std::abs(buf[static_cast<size_t>(k)]);
        std::array<double, 128> sal{};
        double smax = 0;
        for (int m = 55; m <= 96; ++m) {
            const double f = 440.0 * std::pow(2.0, (m - 69) / 12.0);
            for (int h = 1; h <= 4; ++h) sal[static_cast<size_t>(m)] += magAt(f * h) / h;
            smax = std::max(smax, sal[static_cast<size_t>(m)]);
        }
        loudest = std::max(loudest, smax);
        int pick = -1;
        for (int m = 96; m >= 55 && smax > 0; --m) {
            const double sm = sal[static_cast<size_t>(m)];
            if (sm < 0.4 * smax) continue;
            const double f = 440.0 * std::pow(2.0, (m - 69) / 12.0);
            // Must be a real peak of its own, not a neighbour's leakage.
            if (magAt(f) < magAt(f * std::pow(2.0, 1.0 / 12)) || magAt(f) < magAt(f / std::pow(2.0, 1.0 / 12))) continue;
            // An overtone is clearly weaker than the note it belongs to; a melody doubling a
            // chord tone an octave up is not.
            bool overtone = false;
            for (int below : {12, 19, 24})
                if (m - below >= 55 && magAt(f / std::pow(2.0, below / 12.0)) > 1.8 * magAt(f)) overtone = true;
            if (overtone) continue;
            pick = m;
            break;
        }
        frameNote.push_back(pick);
        frameLevel.push_back(smax);
    }
    for (size_t i = 0; i < frameNote.size(); ++i)
        if (frameLevel[i] < 0.08 * loudest) frameNote[i] = -1;  // quiet: nothing on top
    // Smooth (median of 5) and keep notes of at least 60 ms.
    std::vector<int> smooth(frameNote.size(), -1);
    for (size_t i = 0; i < frameNote.size(); ++i) {
        std::vector<int> w;
        for (size_t k = i >= 2 ? i - 2 : 0; k <= std::min(frameNote.size() - 1, i + 2); ++k) w.push_back(frameNote[k]);
        std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
        smooth[i] = w[w.size() / 2];
    }
    const double ticksPerSec = bpm / 60.0 * static_cast<double>(kQ);
    const double hopSec = static_cast<double>(hop) / a.sampleRate;
    for (size_t i = 0; i < smooth.size();) {
        size_t j = i;
        while (j < smooth.size() && smooth[j] == smooth[i]) ++j;
        if (smooth[i] >= 0 && j - i >= 3)
            notes.push_back({static_cast<int64_t>(std::llround(static_cast<double>(i) * hopSec * ticksPerSec)),
                             static_cast<int64_t>(std::llround(static_cast<double>(j - i) * hopSec * ticksPerSec)),
                             smooth[i], 100});
        i = j;
    }
    return notes;
}

}  // namespace

ReferenceProfile analyzeReference(const AudioBuffer& a) {
    ReferenceProfile r;
    if (a.frames() <= 0 || a.sampleRate <= 0) return r;
    const auto mono = monoOf(a, 0, a.frames());
    // Where the music starts.
    float peak = 0;
    for (float v : mono) peak = std::max(peak, std::abs(v));
    int64_t start = 0;
    while (start < static_cast<int64_t>(mono.size()) && std::abs(mono[static_cast<size_t>(start)]) < 0.05f * peak) ++start;
    // A 30 s window from the start is plenty and keeps analysis quick on long songs.
    const int64_t win = std::min<int64_t>(a.frames() - start, static_cast<int64_t>(30 * a.sampleRate));
    const AudioBuffer part = slice(a, start, win);

    const auto tempo = detectTempo(part);
    if (tempo.bpm > 0) r.bpm = tempo.bpm;
    r.key = detectKey(chromaFromAudio(part));

    const auto barFrames = static_cast<int64_t>(240.0 / r.bpm * a.sampleRate);
    for (int64_t b = 0; b * barFrames < part.frames() && r.chords.size() < 4; ++b) {
        const auto name = chordName(chromaFromAudio(part, b * barFrames, barFrames));
        if (name != "N") r.chords.push_back(name);
    }

    double e = 0;
    int64_t crossings = 0;
    const auto pm = monoOf(part, 0, part.frames());
    for (size_t i = 0; i < pm.size(); ++i) {
        e += static_cast<double>(pm[i]) * pm[i];
        if (i > 0 && (pm[i] >= 0) != (pm[i - 1] >= 0)) ++crossings;
    }
    const double rms = pm.empty() ? 0 : std::sqrt(e / static_cast<double>(pm.size()));
    r.energy = std::clamp(rms / 0.2, 0.0, 1.0);
    const double zcrHz = pm.empty() ? 0 : static_cast<double>(crossings) / 2.0 / (static_cast<double>(pm.size()) / a.sampleRate);
    r.brightness = std::clamp(std::log2(std::max(1.0, zcrHz) / 300.0) / 4.0, 0.0, 1.0);
    const double beats = static_cast<double>(part.frames()) / a.sampleRate * r.bpm / 60.0;
    const auto hits = static_cast<double>(detectSlices(part, kMaxSlices).size());
    r.busyness = std::clamp(hits / std::max(1.0, beats) / 2.0, 0.0, 1.0);

    r.melody = topLine(part, r.bpm);
    if (r.melody.size() < 4) {  // a solo line (a cappella, one instrument)
        Project p;
        p.tempoBpm = r.bpm;
        r.melody = transcribeMelody(part, p, 0);
    }
    return r;
}

namespace {

std::vector<Chord> progression(const ReferenceProfile& ref, Rng& rng, int bars, bool sevenths) {
    const KeyEstimate& k = ref.key;
    std::vector<Chord> out;
    // The reference's chord movement, as degrees of its key (chord progressions aren't owned),
    // or a common progression; then one or two chords are swapped for others in the key.
    std::vector<Chord> fromRef;
    for (const auto& n : ref.chords) {
        Chord c;
        if (!parseChord(n, c)) continue;
        const int d = degreeOf(k, c.root);
        fromRef.push_back(d >= 0 ? diatonic(k, d, sevenths) : c);
    }
    if (fromRef.size() >= 2 && rng.chance(0.5)) {
        out = fromRef;
    } else {
        static const int major[][4] = {{0, 4, 5, 3}, {5, 3, 0, 4}, {0, 5, 3, 4}, {1, 4, 0, 5}, {0, 3, 5, 4}, {3, 0, 4, 5}};
        static const int minor[][4] = {{0, 5, 2, 6}, {0, 3, 6, 2}, {0, 6, 5, 6}, {0, 3, 4, 0}, {0, 5, 3, 4}, {5, 6, 0, 0}};
        const auto& pick = k.minor ? minor[rng.below(6)] : major[rng.below(6)];
        for (int d : pick) out.push_back(diatonic(k, d, sevenths));
    }
    const int swaps = 1 + rng.below(2);
    for (int i = 0; i < swaps && out.size() > 1; ++i) {
        const int at = 1 + rng.below(static_cast<int>(out.size()) - 1);  // keep the home chord first
        out[static_cast<size_t>(at)] = diatonic(k, 1 + rng.below(6), sevenths && rng.chance(0.7));
    }
    std::vector<Chord> full;
    for (int b = 0; b < bars; ++b) full.push_back(out[static_cast<size_t>(b) % out.size()]);
    return full;
}

enum class Style { LoFi, Pop, House, Trap };

const char* styleName(Style s) {
    switch (s) {
        case Style::LoFi: return "laid-back";
        case Style::Pop: return "pop";
        case Style::House: return "four-on-the-floor";
        case Style::Trap: return "half-time trap";
    }
    return "";
}

}  // namespace

InspiredIdea generateIdea(const ReferenceProfile& ref, uint32_t seed, int bars) {
    Rng rng(seed);
    InspiredIdea idea;
    idea.seed = seed;
    idea.bpm = std::clamp(ref.bpm, 60.0, 180.0);
    idea.key = ref.key;
    idea.bars = bars = std::clamp(bars, 1, 16);
    const Style style = idea.bpm < 100 ? Style::LoFi : idea.bpm < 120 ? Style::Pop : idea.bpm < 136 ? Style::House : Style::Trap;
    const bool sevenths = style == Style::LoFi || (ref.brightness > 0.45 && rng.chance(0.5));
    const auto chords = progression(ref, rng, bars, sevenths);
    for (const auto& c : chords) idea.chordNames.push_back(nameOf(c));

    // ---- chords: voice-led around middle C, rhythm from how busy the reference is
    const double busy = std::clamp(ref.busyness + (rng.unit() - 0.5) * 0.3, 0.0, 1.0);
    std::vector<std::pair<int, int>> chordHits;  // (step, length in steps)
    if (busy < 0.35) chordHits = {{0, 16}};
    else if (busy < 0.65) chordHits = rng.chance(0.5) ? std::vector<std::pair<int, int>>{{0, 6}, {6, 10}}
                                                      : std::vector<std::pair<int, int>>{{0, 8}, {8, 8}};
    else chordHits = rng.chance(0.5) ? std::vector<std::pair<int, int>>{{0, 3}, {3, 3}, {6, 2}, {10, 3}, {14, 2}}
                                     : std::vector<std::pair<int, int>>{{2, 2}, {6, 2}, {10, 2}, {14, 2}};
    int centre = 62;
    for (int b = 0; b < bars; ++b) {
        const auto tones = tonesOf(chords[static_cast<size_t>(b)]);
        std::vector<int> voicing;
        for (int pc : tones) voicing.push_back(nearestOctave(pc, centre));
        std::sort(voicing.begin(), voicing.end());
        for (auto [st, len] : chordHits)
            for (int n : voicing)
                idea.chords.push_back({b * kBar + st * kStep, len * kStep - kStep / 4, n, 70 + rng.below(16)});
    }

    // ---- drums (1/16 grid)
    std::array<int, 16> kick{}, snare{}, hat{}, open{};
    switch (style) {
        case Style::LoFi:
            kick[0] = 110, kick[10] = 95;
            if (rng.chance(0.5)) kick[7] = 80;
            if (rng.chance(0.4)) kick[13] = 70;
            snare[4] = snare[12] = 105;
            for (int i = 0; i < 16; i += 2) hat[static_cast<size_t>(i)] = 60 + rng.below(25);
            idea.swing = 56 + rng.below(6);
            break;
        case Style::Pop:
            kick[0] = kick[8] = 110;
            if (rng.chance(0.6)) kick[6] = 90;
            if (rng.chance(0.4)) kick[14] = 80;
            snare[4] = snare[12] = 110;
            for (int i = 0; i < 16; i += 2) hat[static_cast<size_t>(i)] = 55 + rng.below(30);
            break;
        case Style::House:
            kick[0] = kick[4] = kick[8] = kick[12] = 115;
            snare[4] = snare[12] = 95;  // clap-like backbeat
            for (int i = 2; i < 16; i += 4) open[static_cast<size_t>(i)] = 70 + rng.below(20);
            for (int i = 0; i < 16; ++i)
                if (!open[static_cast<size_t>(i)] && rng.chance(0.6)) hat[static_cast<size_t>(i)] = 40 + rng.below(25);
            break;
        case Style::Trap:
            kick[0] = 120;
            kick[static_cast<size_t>(rng.chance(0.5) ? 10 : 11)] = 100;
            if (rng.chance(0.5)) kick[3] = 90;
            snare[8] = 115;  // half-time backbeat
            for (int i = 0; i < 16; ++i) hat[static_cast<size_t>(i)] = i % 2 ? 45 + rng.below(20) : 70 + rng.below(20);
            break;
    }
    const int snareNote = style == Style::House ? 39 : 38;
    for (int b = 0; b < bars; ++b)
        for (int i = 0; i < 16; ++i) {
            const int64_t t = b * kBar + i * kStep;
            const bool fill = b == bars - 1 && i >= 12 && rng.chance(0.35);  // a little fill into the loop
            if (kick[static_cast<size_t>(i)]) idea.drums.push_back({t, kStep, 36, kick[static_cast<size_t>(i)]});
            if (snare[static_cast<size_t>(i)] || fill) idea.drums.push_back({t, kStep, snareNote, fill ? 70 + rng.below(30) : snare[static_cast<size_t>(i)]});
            if (hat[static_cast<size_t>(i)]) idea.drums.push_back({t, kStep, 42, hat[static_cast<size_t>(i)]});
            if (open[static_cast<size_t>(i)]) idea.drums.push_back({t, kStep, 46, open[static_cast<size_t>(i)]});
        }

    // ---- bass: chord roots, following the kick
    for (int b = 0; b < bars; ++b) {
        const auto& c = chords[static_cast<size_t>(b)];
        const int root = nearestOctave(c.root, 40);
        std::vector<int> hits;
        for (int i = 0; i < 16; ++i)
            if (kick[static_cast<size_t>(i)]) hits.push_back(i);
        if (hits.empty() || hits.front() != 0) hits.insert(hits.begin(), 0);
        for (size_t h = 0; h < hits.size(); ++h) {
            const int st = hits[h], end = h + 1 < hits.size() ? hits[h + 1] : 16;
            int note = root;
            if (h > 0 && rng.chance(0.3)) note = root + (rng.chance(0.5) ? 7 : 12);  // fifth or octave
            idea.bass.push_back({b * kBar + st * kStep, (end - st) * kStep - kStep / 4, note, 95 + rng.below(20)});
        }
    }

    // ---- melody: a motif (rhythm reused A A' A B), notes on the scale, chord tones on strong beats
    const auto scale = scalePitches(idea.key, 64, 86);
    const int notesPerBar = 3 + static_cast<int>(std::lround(busy * 4)) + rng.below(2);
    auto makeRhythm = [&]() {
        std::vector<int> steps;
        std::vector<int> pool;
        for (int i = 0; i < 16; ++i) pool.push_back(i);
        steps.push_back(rng.chance(0.7) ? 0 : 2);
        while (static_cast<int>(steps.size()) < notesPerBar) {
            const int i = pool[static_cast<size_t>(rng.below(16))];
            // Prefer beats and 8ths (fewer 16th pickups).
            if (i % 2 == 1 && rng.chance(0.7)) continue;
            if (std::find(steps.begin(), steps.end(), i) == steps.end()) steps.push_back(i);
        }
        std::sort(steps.begin(), steps.end());
        return steps;
    };
    const auto rhythmA = makeRhythm(), rhythmB = makeRhythm();
    size_t idx = scale.size() / 2;
    for (int b = 0; b < bars; ++b) {
        const auto& rhythm = (b % 4 == 3) ? rhythmB : rhythmA;
        const auto tones = tonesOf(chords[static_cast<size_t>(b)]);
        for (size_t k = 0; k < rhythm.size(); ++k) {
            const int st = rhythm[k], next = k + 1 < rhythm.size() ? rhythm[k + 1] : 16;
            // Move by step mostly, sometimes leap; snap to a chord tone on strong beats.
            const int move = rng.chance(0.65) ? (rng.chance(0.5) ? 1 : -1) * (1 + rng.below(2)) : (rng.chance(0.5) ? 3 : -3);
            idx = static_cast<size_t>(std::clamp<int>(static_cast<int>(idx) + move, 0, static_cast<int>(scale.size()) - 1));
            if (st % 4 == 0) {
                size_t best = idx;
                int dist = 100;
                for (size_t s = 0; s < scale.size(); ++s)
                    if (std::find(tones.begin(), tones.end(), scale[s] % 12) != tones.end() &&
                        std::abs(static_cast<int>(s) - static_cast<int>(idx)) < dist)
                        dist = std::abs(static_cast<int>(s) - static_cast<int>(idx)), best = s;
                idx = best;
            }
            const int len = std::max(1, std::min(next - st, rng.chance(0.3) ? 1 : 4));
            idea.melody.push_back({b * kBar + st * kStep, len * kStep - kStep / 8, scale[idx], 80 + rng.below(30)});
        }
    }

    idea.similarity = melodySimilarity(idea.melody, ref.melody);
    std::string prog;
    for (size_t i = 0; i < idea.chordNames.size() && i < 4; ++i) prog += (i ? " - " : "") + idea.chordNames[i];
    idea.description = idea.key.name() + ", " + std::to_string(static_cast<int>(std::lround(idea.bpm))) + " BPM, " +
                       styleName(style) + ": " + prog;
    return idea;
}

InspiredIdea generateOriginalIdea(const ReferenceProfile& ref, uint32_t seed, int bars) {
    InspiredIdea best;
    bool have = false;
    for (uint32_t s = seed; s < seed + 24; ++s) {
        auto idea = generateIdea(ref, s, bars);
        if (!have || idea.similarity < best.similarity) {
            best = std::move(idea);
            have = true;
        }
        if (best.similarity < kMaxSimilarity) break;
    }
    return best;
}

double melodySimilarity(const std::vector<MidiNote>& a, const std::vector<MidiNote>& b) {
    // Top line as interval steps (transposition doesn't matter), each with its rhythm relative
    // to the previous note (tempo doesn't matter).
    struct Tok { int interval; int rhythm; };
    auto tokens = [](std::vector<MidiNote> n) {
        std::sort(n.begin(), n.end(), [](const MidiNote& x, const MidiNote& y) {
            return x.tick != y.tick ? x.tick < y.tick : x.pitch > y.pitch;
        });
        // Top note at each onset; a repeated note counts once (sung or held, it's the same tune).
        std::vector<MidiNote> top;
        for (const auto& x : n)
            if (top.empty() || (x.tick != top.back().tick && x.pitch != top.back().pitch)) top.push_back(x);
        std::vector<Tok> t;
        for (size_t i = 1; i < top.size(); ++i) {
            const double ioi = static_cast<double>(std::max<int64_t>(1, top[i].tick - top[i - 1].tick));
            const double prev = i >= 2 ? static_cast<double>(std::max<int64_t>(1, top[i - 1].tick - top[i - 2].tick)) : ioi;
            t.push_back({std::clamp(top[i].pitch - top[i - 1].pitch, -12, 12),
                         static_cast<int>(std::lround(std::clamp(std::log2(ioi / prev), -2.0, 2.0)))});
        }
        return t;
    };
    const auto x = tokens(a), y = tokens(b);
    if (x.size() < 3 || y.size() < 3) return 0.0;
    // Longest shared run of intervals (matching rhythm counts a little more), relative to the
    // shorter melody.
    std::vector<double> prev(y.size() + 1, 0.0), cur(y.size() + 1, 0.0);
    double best = 0;
    for (size_t i = 1; i <= x.size(); ++i) {
        for (size_t j = 1; j <= y.size(); ++j) {
            const auto& p = x[i - 1];
            const auto& q = y[j - 1];
            if (p.interval == q.interval) {
                const double w = p.rhythm == q.rhythm ? 1.0 : 0.8;
                cur[j] = prev[j - 1] + w;
                best = std::max(best, cur[j]);
            } else {
                cur[j] = 0;
            }
        }
        std::swap(prev, cur);
    }
    // Relative to the shorter melody matched against itself (so identical = 1).
    const auto& shorter = x.size() <= y.size() ? x : y;
    return std::clamp(best / static_cast<double>(shorter.size()), 0.0, 1.0);
}

std::vector<std::string> addIdeaTracks(Project& p, const InspiredIdea& idea, int64_t atTick, bool setTempoToIdea) {
    if (setTempoToIdea) setTempo(p, idea.bpm);
    std::vector<std::string> ids;
    const int64_t len = idea.bars * kBar;
    auto synthTrack = [&](const std::string& name, double wave, double attack, double release, double level,
                          const std::vector<MidiNote>& notes) {
        const std::string id = addTrack(p, TrackKind::Instrument, name);
        auto synth = makeBuiltin(kBuiltinSynth, "Ynot Synth");
        synth.params["wave"] = wave;
        synth.params["attack"] = attack;
        synth.params["release"] = release;
        synth.params["level"] = level;
        setInstrument(p, id, synth);
        p.findTrack(id)->midiClips.push_back({newId(), atTick, len, notes, std::nullopt, {}});
        ids.push_back(id);
    };
    synthTrack("Idea chords", 1.0, 0.04, 0.35, 0.07, idea.chords);
    synthTrack("Idea bass", 0.0, 0.005, 0.08, 0.35, idea.bass);
    synthTrack("Idea melody", 0.0, 0.01, 0.2, 0.2, idea.melody);
    if (!idea.drums.empty()) {
        const std::string id = addDrumTrack(p, atTick, idea.bars);
        renameTrack(p, id, "Idea drums");
        const std::string clip = p.findTrack(id)->midiClips.at(0).id;
        setPatternSwing(p, id, clip, idea.swing);
        for (const auto& n : idea.drums) setStep(p, id, clip, n.pitch, static_cast<int>(n.tick / kStep), n.velocity);
        ids.push_back(id);
    }
    return ids;
}

AudioBuffer renderIdea(const InspiredIdea& idea, double sampleRate, bool withDrums) {
    Project q;
    q.id = "idea";
    q.tempoBpm = idea.bpm;
    InspiredIdea parts = idea;
    if (!withDrums) parts.drums.clear();
    addIdeaTracks(q, parts, 0, false);
    PackageMediaProvider media("");
    BuiltinPluginHost host;
    AudioBuffer out = renderProject(q, media, host, sampleRate, 2.0);
    // Exactly the loop's length; the tail (release, reverb-like decay) wraps to the start, so
    // the loop repeats seamlessly in a sampler or as a clip.
    const auto loopFrames = static_cast<int64_t>(std::llround(idea.bars * 4 * 60.0 / idea.bpm * sampleRate));
    if (out.frames() > loopFrames) {
        const int ch = out.channels;
        for (int64_t f = loopFrames; f < out.frames() && f - loopFrames < loopFrames; ++f)
            for (int c = 0; c < ch; ++c)
                out.samples[static_cast<size_t>((f - loopFrames) * ch + c)] += out.samples[static_cast<size_t>(f * ch + c)];
        out.samples.resize(static_cast<size_t>(loopFrames * ch));
    }
    return out;
}

}  // namespace ynot
