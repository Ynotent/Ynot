#include "ynot/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "fft_internal.hpp"
#include "ynot/demo.hpp"
#include "ynot/drums.hpp"
#include "ynot/edit.hpp"
#include "ynot/pitch_correct.hpp"
#include "ynot/sampler.hpp"

namespace ynot {

namespace {

constexpr double kPi = 3.14159265358979323846;
const char* const kNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

double sum(const Chroma& c) { return std::accumulate(c.begin(), c.end(), 0.0); }

void addScaled(Chroma& into, const Chroma& c, double w) {
    const double s = sum(c);
    if (s <= 0) return;
    for (int i = 0; i < 12; ++i) into[static_cast<size_t>(i)] += c[static_cast<size_t>(i)] / s * w;
}

double correlation(const Chroma& a, const double* b) {
    double ma = 0, mb = 0;
    for (int i = 0; i < 12; ++i) ma += a[static_cast<size_t>(i)], mb += b[i];
    ma /= 12, mb /= 12;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < 12; ++i) {
        const double x = a[static_cast<size_t>(i)] - ma, y = b[i] - mb;
        num += x * y, da += x * x, db += y * y;
    }
    return da > 0 && db > 0 ? num / std::sqrt(da * db) : 0.0;
}

std::vector<float> mono(const AudioBuffer& a, int64_t from, int64_t frames) {
    from = std::clamp<int64_t>(from, 0, a.frames());
    frames = std::clamp<int64_t>(frames < 0 ? a.frames() - from : frames, 0, a.frames() - from);
    std::vector<float> m(static_cast<size_t>(frames));
    const int ch = std::max(1, a.channels);
    for (int64_t f = 0; f < frames; ++f) {
        float s = 0;
        for (int c = 0; c < ch; ++c) s += a.samples[static_cast<size_t>((from + f) * ch + c)];
        m[static_cast<size_t>(f)] = s / static_cast<float>(ch);
    }
    return m;
}

double rmsOf(const float* x, int64_t n) {
    double e = 0;
    for (int64_t i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i];
    return n > 0 ? std::sqrt(e / static_cast<double>(n)) : 0.0;
}

int64_t songEnd(const Project& p) {
    int64_t end = 0;
    for (const auto& t : p.tracks) {
        for (const auto& c : t.audioClips) end = std::max(end, c.startTick + c.lengthTick);
        for (const auto& c : t.midiClips) end = std::max(end, c.startTick + c.lengthTick);
    }
    return end;
}

}  // namespace

std::string KeyEstimate::name() const { return std::string(kNames[tonic % 12]) + (minor ? " minor" : " major"); }

Chroma chromaFromAudio(const AudioBuffer& a, int64_t fromFrame, int64_t frames) {
    Chroma out{};
    const auto x = mono(a, fromFrame, frames);
    if (x.empty() || a.sampleRate <= 0) return out;
    const int N = a.sampleRate > 30000 ? 16384 : 8192;  // ~3 Hz bins: semitones apart from ~80 Hz
    const int hop = N / 2;
    // Bin -> pitch class, 80 Hz .. 4 kHz (bass to the upper harmonics of a voice).
    std::vector<int> pc(static_cast<size_t>(N / 2), -1);
    for (int k = 1; k < N / 2; ++k) {
        const double f = k * a.sampleRate / N;
        if (f < 80 || f > 4000) continue;
        const int midi = static_cast<int>(std::lround(69 + 12 * std::log2(f / 440.0)));
        pc[static_cast<size_t>(k)] = ((midi % 12) + 12) % 12;
    }
    std::vector<double> window(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) window[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / N);
    const detail::Fft fft(N);
    std::vector<std::complex<double>> buf(static_cast<size_t>(N));
    const int64_t n = static_cast<int64_t>(x.size());
    for (int64_t start = 0; start == 0 || start + N / 4 < n; start += hop) {
        for (int i = 0; i < N; ++i) {
            const int64_t j = start + i;
            buf[static_cast<size_t>(i)] = j < n ? x[static_cast<size_t>(j)] * window[static_cast<size_t>(i)] : 0.0;
        }
        fft.run(buf, false);
        for (int k = 1; k < N / 2; ++k)
            if (pc[static_cast<size_t>(k)] >= 0) out[static_cast<size_t>(pc[static_cast<size_t>(k)])] += std::abs(buf[static_cast<size_t>(k)]);
    }
    return out;
}

Chroma chromaFromNotes(const std::vector<MidiNote>& notes, int64_t fromTick, int64_t toTick) {
    Chroma out{};
    for (const auto& n : notes) {
        const int64_t a = std::max(n.tick, fromTick), b = std::min(n.tick + n.lengthTick, toTick);
        if (b <= a) continue;
        out[static_cast<size_t>(((n.pitch % 12) + 12) % 12)] += static_cast<double>(b - a) * n.velocity / 127.0;
    }
    return out;
}

KeyEstimate detectKey(const Chroma& c) {
    // Krumhansl-Kessler key profiles (listening experiments on how well each note fits a key).
    static const double major[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double minor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    KeyEstimate best;
    if (sum(c) <= 0) return best;
    double bestR = -2, second = -2;
    for (int mode = 0; mode < 2; ++mode)
        for (int tonic = 0; tonic < 12; ++tonic) {
            Chroma rotated{};
            for (int i = 0; i < 12; ++i) rotated[static_cast<size_t>(i)] = c[static_cast<size_t>((i + tonic) % 12)];
            const double r = correlation(rotated, mode ? minor : major);
            if (r > bestR) {
                second = bestR;
                bestR = r;
                best.tonic = tonic;
                best.minor = mode == 1;
            } else if (r > second) {
                second = r;
            }
        }
    best.confidence = std::clamp((bestR - second) * 4.0 + std::max(0.0, bestR - 0.5), 0.0, 1.0);
    return best;
}

std::string chordName(const Chroma& c) {
    const double total = sum(c);
    if (total <= 1e-12) return "N";
    const double mx = *std::max_element(c.begin(), c.end());
    struct Shape { const char* suffix; std::vector<int> notes; double bias; };
    static const std::vector<Shape> shapes = {
        {"", {0, 4, 7}, 0.0},        {"m", {0, 3, 7}, 0.0},      {"dim", {0, 3, 6}, -0.06},
        {"7", {0, 4, 7, 10}, -0.04}, {"maj7", {0, 4, 7, 11}, -0.04}, {"m7", {0, 3, 7, 10}, -0.04},
        {"sus4", {0, 5, 7}, -0.08},
    };
    double best = -1;
    std::string name = "N";
    for (int root = 0; root < 12; ++root)
        for (const auto& s : shapes) {
            double in = 0, out = 0;
            bool member[12] = {};
            for (int n : s.notes) member[(root + n) % 12] = true;
            for (int i = 0; i < 12; ++i) (member[i] ? in : out) += c[static_cast<size_t>(i)] / mx;
            const double score = in / static_cast<double>(s.notes.size()) -
                                 out / static_cast<double>(12 - s.notes.size()) + s.bias +
                                 0.05 * c[static_cast<size_t>(root)] / mx;  // the root tends to be strong
            if (score > best) {
                best = score;
                name = std::string(kNames[root]) + s.suffix;
            }
        }
    return best >= 0.25 ? name : "N";
}

bool isPercussionTrack(const Track& t) {
    if (t.instrument && t.instrument->format == PluginFormat::Builtin) {
        if (t.instrument->uid == kBuiltinDrums) return true;
        if (t.instrument->uid == kBuiltinSampler && t.instrument->params.getString("mode", "chop") == "chop") return true;
    }
    for (const auto& c : t.midiClips)
        if (c.steps) return true;
    return false;
}

Chroma songChroma(const Project& p, MediaProvider& media, int64_t fromTick, int64_t toTick) {
    Chroma out{};
    if (toTick <= fromTick) return out;
    const double winSec = p.tickToSeconds(static_cast<double>(toTick)) - p.tickToSeconds(static_cast<double>(fromTick));
    for (const auto& t : p.tracks) {
        if (t.mute || isPercussionTrack(t)) continue;
        const double trackGain = dbToGain(t.gainDb);
        for (const auto& c : t.audioClips) {
            const int64_t a = std::max(fromTick, c.startTick), b = std::min(toTick, c.startTick + c.lengthTick);
            if (b <= a) continue;
            const MediaRef* m = p.findMedia(c.mediaId);
            const AudioBuffer* buf = m ? media.get(*m) : nullptr;
            if (!buf) continue;
            const double s0 = p.tickToSeconds(static_cast<double>(a)) - p.tickToSeconds(static_cast<double>(c.startTick));
            const double s1 = p.tickToSeconds(static_cast<double>(b)) - p.tickToSeconds(static_cast<double>(c.startTick));
            const auto from = c.sourceOffsetFrames + static_cast<int64_t>(s0 * buf->sampleRate);
            const auto n = static_cast<int64_t>((s1 - s0) * buf->sampleRate);
            const auto seg = mono(*buf, from, n);
            const double level = std::min(1.0, rmsOf(seg.data(), static_cast<int64_t>(seg.size())) / 0.05);
            if (level < 0.02) continue;  // silence
            addScaled(out, chromaFromAudio(*buf, from, n), trackGain * dbToGain(c.gainDb) * level * (s1 - s0) / winSec);
        }
        for (const auto& c : t.midiClips) {
            const int64_t a = std::max(fromTick, c.startTick), b = std::min(toTick, c.startTick + c.lengthTick);
            if (b <= a) continue;
            const Chroma ch = chromaFromNotes(c.notes, a - c.startTick, b - c.startTick);
            const double covered = std::min(1.0, sum(ch) / static_cast<double>(toTick - fromTick));
            addScaled(out, ch, trackGain * covered);
        }
    }
    return out;
}

KeyEstimate detectSongKey(const Project& p, MediaProvider& media) {
    return detectKey(songChroma(p, media, 0, songEnd(p)));
}

std::vector<ChordSpan> detectChords(const Project& p, MediaProvider& media, int64_t stepTicks) {
    if (stepTicks <= 0) stepTicks = std::max<int64_t>(1, p.barTicks() / 2);
    std::vector<ChordSpan> out;
    const int64_t end = songEnd(p);
    std::vector<std::pair<int64_t, Chroma>> windows;
    std::vector<double> sums;
    for (int64_t t = 0; t < end; t += stepTicks) {
        windows.emplace_back(t, songChroma(p, media, t, std::min(end, t + stepTicks)));
        sums.push_back(sum(windows.back().second));
    }
    if (windows.empty()) return out;
    auto sorted = sums;
    std::sort(sorted.begin(), sorted.end());
    const double typical = sorted[sorted.size() / 2];
    for (const auto& [tick, c] : windows) {
        const std::string name = sum(c) < 0.1 * typical || sum(c) <= 0 ? "N" : chordName(c);
        const int64_t len = std::min(end, tick + stepTicks) - tick;
        if (!out.empty() && out.back().name == name) out.back().lengthTick += len;
        else out.push_back({tick, len, name});
    }
    return out;
}

TempoEstimate detectTempo(const AudioBuffer& a) {
    TempoEstimate est;
    const auto x = mono(a, 0, -1);
    const double fps = 200.0;  // onset envelope at 5 ms
    const int hop = std::max(1, static_cast<int>(a.sampleRate / fps));
    const int64_t frames = static_cast<int64_t>(x.size()) / hop;
    if (frames < static_cast<int64_t>(fps * 2)) return est;  // under 2 s: too short to tell
    std::vector<double> env(static_cast<size_t>(frames)), onset(static_cast<size_t>(frames), 0.0);
    for (int64_t f = 0; f < frames; ++f)
        env[static_cast<size_t>(f)] = std::log10(rmsOf(x.data() + f * hop, hop) + 1e-5);
    for (int64_t f = 1; f < frames; ++f) onset[static_cast<size_t>(f)] = std::max(0.0, env[static_cast<size_t>(f)] - env[static_cast<size_t>(f - 1)]);
    const double mean = std::accumulate(onset.begin(), onset.end(), 0.0) / static_cast<double>(frames);
    for (auto& v : onset) v -= mean;
    auto ac = [&](double lag) {  // autocorrelation at a fractional lag
        const auto l0 = static_cast<int64_t>(lag);
        const double fr = lag - static_cast<double>(l0);
        double s = 0;
        for (int64_t i = 0; i + l0 + 1 < frames; ++i)
            s += onset[static_cast<size_t>(i)] *
                 ((1 - fr) * onset[static_cast<size_t>(i + l0)] + fr * onset[static_cast<size_t>(i + l0 + 1)]);
        return s;
    };
    const double zero = ac(0);
    if (zero <= 0) return est;
    double bestScore = -1e300, bestBpm = 0;
    for (double bpm = 60; bpm <= 200; bpm += 0.05) {
        const double lag = 60.0 / bpm * fps;
        if (lag * 2 >= static_cast<double>(frames)) continue;
        // Beats repeat: reward the 2- and 4-beat lags too, and prefer common song tempos.
        const double score = (ac(lag) + 0.5 * ac(2 * lag) + 0.25 * ac(4 * lag < static_cast<double>(frames) ? 4 * lag : 2 * lag)) *
                             std::exp(-0.5 * std::pow(std::log2(bpm / 115.0) / 1.0, 2));
        if (score > bestScore) bestScore = score, bestBpm = bpm;
    }
    est.bpm = std::round(bestBpm * 10) / 10;
    est.confidence = std::clamp(ac(60.0 / bestBpm * fps) / zero, 0.0, 1.0);
    if (est.confidence < 0.05) est.bpm = 0;
    return est;
}

std::vector<MidiNote> transcribeMelody(const AudioBuffer& a, const Project& p, int64_t startTick) {
    std::vector<MidiNote> notes;
    const auto x = mono(a, 0, -1);
    const int hop = std::max(1, static_cast<int>(a.sampleRate * 0.01));  // 10 ms
    const int win = static_cast<int>(a.sampleRate * 0.046);
    const int64_t frames = (static_cast<int64_t>(x.size()) - win) / hop;
    if (frames <= 0) return notes;
    std::vector<double> level(static_cast<size_t>(frames)), midi(static_cast<size_t>(frames), 0.0);
    double peak = 0;
    for (int64_t f = 0; f < frames; ++f) {
        level[static_cast<size_t>(f)] = rmsOf(x.data() + f * hop, win);
        peak = std::max(peak, level[static_cast<size_t>(f)]);
    }
    const double gate = std::max(0.003, peak * 0.06);
    for (int64_t f = 0; f < frames; ++f) {
        if (level[static_cast<size_t>(f)] < gate) continue;
        const double hz = detectPitchHz(x.data() + f * hop, win, a.sampleRate);
        if (hz > 0) midi[static_cast<size_t>(f)] = 69 + 12 * std::log2(hz / 440.0);
    }
    // Smooth vibrato and glitches: median of 5 voiced frames.
    std::vector<int> note(static_cast<size_t>(frames), -1);
    for (int64_t f = 0; f < frames; ++f) {
        if (midi[static_cast<size_t>(f)] <= 0) continue;
        std::vector<double> w;
        for (int64_t k = std::max<int64_t>(0, f - 2); k <= std::min(frames - 1, f + 2); ++k)
            if (midi[static_cast<size_t>(k)] > 0) w.push_back(midi[static_cast<size_t>(k)]);
        std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
        note[static_cast<size_t>(f)] = static_cast<int>(std::lround(w[w.size() / 2]));
    }
    // Runs of the same note; a new note also starts where the level jumps (a re-sung note).
    struct Run { int note; int64_t from, to; double level; };
    std::vector<Run> runs;
    for (int64_t f = 0; f < frames; ++f) {
        const int n = note[static_cast<size_t>(f)];
        double recentMin = level[static_cast<size_t>(f)];
        for (int64_t k = std::max<int64_t>(0, f - 5); k < f; ++k) recentMin = std::min(recentMin, level[static_cast<size_t>(k)]);
        const bool reattack = level[static_cast<size_t>(f)] > 2.0 * recentMin && f > 0 &&
                              level[static_cast<size_t>(f)] > level[static_cast<size_t>(f - 1)] * 1.4;
        if (!runs.empty() && runs.back().note == n && runs.back().to == f && !(n >= 0 && reattack && f - runs.back().from >= 6)) {
            runs.back().to = f + 1;
            runs.back().level = std::max(runs.back().level, level[static_cast<size_t>(f)]);
        } else {
            runs.push_back({n, f, f + 1, level[static_cast<size_t>(f)]});
        }
    }
    // Short blips (< 60 ms) join the note before them (a scoop or glitch), or are dropped.
    std::vector<Run> clean;
    for (const auto& r : runs) {
        if (r.note < 0) continue;
        if (r.to - r.from < 6) {
            if (!clean.empty() && clean.back().to >= r.from - 1) clean.back().to = r.to;
            continue;
        }
        if (!clean.empty() && clean.back().note == r.note && r.from - clean.back().to < 3 && r.level <= clean.back().level * 1.4)
            clean.back().to = r.to;  // a tiny gap inside one note
        else
            clean.push_back(r);
    }
    const double startSec = p.tickToSeconds(static_cast<double>(startTick));
    auto tickAt = [&](int64_t frame) {
        const double sec = startSec + static_cast<double>(frame * hop) / a.sampleRate;
        return static_cast<int64_t>(std::llround(p.secondsToTick(sec))) - startTick;
    };
    for (const auto& r : clean) {
        if (r.note < 0 || r.note > 127) continue;
        MidiNote n;
        n.tick = std::max<int64_t>(0, tickAt(r.from));
        n.lengthTick = std::max<int64_t>(1, tickAt(r.to) - n.tick);
        n.pitch = r.note;
        n.velocity = std::clamp(static_cast<int>(40 + 87 * std::sqrt(r.level / peak)), 1, 127);
        notes.push_back(n);
    }
    return notes;
}

std::optional<std::string> audioClipToMidi(Project& p, MediaProvider& media, const std::string& trackId,
                                           const std::string& clipId) {
    const Track* t = p.findTrack(trackId);
    if (!t) return std::nullopt;
    const AudioClip* clip = nullptr;
    for (const auto& c : t->audioClips)
        if (c.id == clipId) clip = &c;
    if (!clip) return std::nullopt;
    const AudioBuffer audio = clipAudio(p, media, trackId, clipId);
    const int64_t start = clip->startTick, length = clip->lengthTick;
    auto notes = transcribeMelody(audio, p, start);
    if (notes.empty()) return std::nullopt;
    const std::string name = t->name + " (MIDI)";
    const std::string id = addTrack(p, TrackKind::Instrument, name);
    setInstrument(p, id, makeBuiltin(kBuiltinSynth, "Ynot Synth"));
    p.findTrack(id)->midiClips.push_back({newId(), start, length, std::move(notes), std::nullopt, {}});
    return id;
}

}  // namespace ynot
