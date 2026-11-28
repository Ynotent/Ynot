#include "ynot/stretch.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "ynot/edit.hpp"
#include "ynot/project_format.hpp"
#include "ynot/recorder.hpp"

namespace ynot {

namespace {
constexpr double kPi = 3.14159265358979323846;

// De-interleaved input that reads silence outside its range.
struct Planar {
    std::vector<std::vector<float>> ch;
    int64_t frames = 0;
    float at(size_t c, int64_t i) const { return i < 0 || i >= frames ? 0.f : ch[c][static_cast<size_t>(i)]; }
};

Planar toPlanar(const AudioBuffer& b) {
    Planar p;
    p.frames = b.frames();
    const auto nch = static_cast<size_t>(std::max(1, b.channels));
    p.ch.assign(nch, std::vector<float>(static_cast<size_t>(p.frames)));
    for (int64_t f = 0; f < p.frames; ++f)
        for (size_t c = 0; c < nch; ++c) p.ch[c][static_cast<size_t>(f)] = b.samples[static_cast<size_t>(f) * nch + c];
    return p;
}

AudioBuffer fromPlanar(const std::vector<std::vector<double>>& out, const std::vector<double>& weight, int64_t frames,
                       double rate) {
    AudioBuffer b;
    b.sampleRate = rate;
    b.channels = static_cast<int>(out.size());
    b.samples.resize(static_cast<size_t>(frames) * out.size());
    for (int64_t f = 0; f < frames; ++f) {
        const double w = weight.empty() ? 1.0 : weight[static_cast<size_t>(f)];
        const double g = w > 1e-3 ? 1.0 / w : 0.0;  // overlap-add normalisation
        for (size_t c = 0; c < out.size(); ++c)
            b.samples[static_cast<size_t>(f) * out.size() + c] = static_cast<float>(out[c][static_cast<size_t>(f)] * g);
    }
    return b;
}

// Periodic Hann: overlapping copies at a hop of n/2 sum to exactly 1.
std::vector<double> hann(int n) {
    std::vector<double> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    return w;
}

// ---- Varispeed: band-limited resampling (windowed sinc), so speeding up doesn't alias.
AudioBuffer varispeed(const Planar& in, double rate, int64_t outFrames) {
    const double step = static_cast<double>(in.frames) / static_cast<double>(outFrames);  // input per output
    const double fc = std::min(1.0, 1.0 / step) * 0.95;                                    // cutoff, x Nyquist
    const int zeroCrossings = 16;
    const double halfWidth = zeroCrossings / fc;
    // Kernel (sinc x Blackman) tabulated at 1/512-sample steps; linear lookup is far cheaper than trig.
    constexpr int kSteps = 512;
    std::vector<double> table(static_cast<size_t>(std::ceil(halfWidth * kSteps)) + 2);
    for (size_t i = 0; i < table.size(); ++i) {
        const double d = static_cast<double>(i) / kSteps;
        if (d >= halfWidth) continue;
        const double t = d / halfWidth;
        const double win = 0.42 + 0.5 * std::cos(kPi * t) + 0.08 * std::cos(2 * kPi * t);
        const double arg = kPi * fc * d;
        table[i] = fc * (arg < 1e-9 ? 1.0 : std::sin(arg) / arg) * win;
    }
    auto kernel = [&](double d) {
        const double pos = std::abs(d) * kSteps;
        const auto i = static_cast<size_t>(pos);
        if (i + 1 >= table.size()) return 0.0;
        const double frac = pos - static_cast<double>(i);
        return table[i] + (table[i + 1] - table[i]) * frac;
    };
    std::vector<std::vector<double>> out(in.ch.size(), std::vector<double>(static_cast<size_t>(outFrames)));
    for (int64_t j = 0; j < outFrames; ++j) {
        const double x = static_cast<double>(j) * step;
        const auto lo = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(x - halfWidth)));
        const auto hi = std::min<int64_t>(in.frames - 1, static_cast<int64_t>(std::floor(x + halfWidth)));
        for (int64_t i = lo; i <= hi; ++i) {
            const double k = kernel(x - static_cast<double>(i));
            for (size_t c = 0; c < in.ch.size(); ++c) out[c][static_cast<size_t>(j)] += k * in.ch[c][static_cast<size_t>(i)];
        }
    }
    return fromPlanar(out, {}, outFrames, rate);
}

// ---- Mono: WSOLA (waveform-similarity overlap-add).
AudioBuffer wsola(const Planar& in, double ratio, double rate, int64_t outFrames) {
    int W = static_cast<int>(std::lround(0.046 * rate));  // ~2 periods of a low bass note
    W += W % 2;
    const int Hs = W / 2;
    const int tol = static_cast<int>(std::lround(0.0125 * rate));  // search +-12.5 ms (half a period at 40 Hz)
    const auto win = hann(W);
    const size_t nch = in.ch.size();
    // Alignment is found on the channel sum so every channel gets the same splice (stereo stays put).
    std::vector<float> monoData(static_cast<size_t>(in.frames));
    for (int64_t f = 0; f < in.frames; ++f) {
        float s = 0;
        for (size_t c = 0; c < nch; ++c) s += in.ch[c][static_cast<size_t>(f)];
        monoData[static_cast<size_t>(f)] = s;
    }
    auto mono = [&](int64_t i) { return i < 0 || i >= in.frames ? 0.f : monoData[static_cast<size_t>(i)]; };
    // Normalised correlation of the segment at `cand` with the one at `target`, every `step` samples.
    auto score = [&](int64_t target, int64_t cand, int step) {
        double xy = 0, yy = 1e-9;
        for (int i = 0; i < W; i += step) {
            const double a = mono(target + i), b = mono(cand + i);
            xy += a * b;
            yy += b * b;
        }
        return xy / std::sqrt(yy);
    };

    std::vector<std::vector<double>> out(nch, std::vector<double>(static_cast<size_t>(outFrames)));
    std::vector<double> weight(static_cast<size_t>(outFrames));
    int64_t prev = 0;
    for (int64_t k = 0;; ++k) {
        const int64_t outStart = k * Hs - W / 2;
        if (outStart >= outFrames) break;
        const int64_t nominal = std::llround(static_cast<double>(k * Hs) / ratio) - W / 2;
        int64_t chosen = nominal;
        if (k > 0) {
            const int64_t target = prev + Hs;  // where the previous grain naturally continues
            constexpr int kCoarse = 4;
            int64_t best = 0;
            double bestScore = -1e300;
            for (int64_t d = -tol; d <= tol; d += kCoarse) {
                const double s = score(target, nominal + d, kCoarse);
                if (s > bestScore) bestScore = s, best = d;
            }
            const int64_t coarse = best;
            bestScore = -1e300;
            for (int64_t d = std::max<int64_t>(-tol, coarse - kCoarse + 1); d <= std::min<int64_t>(tol, coarse + kCoarse - 1);
                 ++d) {
                const double s = score(target, nominal + d, 1);
                if (s > bestScore) bestScore = s, best = d;
            }
            chosen = nominal + best;
        }
        prev = chosen;
        for (int i = 0; i < W; ++i) {
            const int64_t o = outStart + i;
            if (o < 0 || o >= outFrames) continue;
            const double w = win[static_cast<size_t>(i)];
            for (size_t c = 0; c < nch; ++c) out[c][static_cast<size_t>(o)] += w * in.at(c, chosen + i);
            weight[static_cast<size_t>(o)] += w;
        }
    }
    return fromPlanar(out, weight, outFrames, rate);
}

// ---- Poly: phase vocoder with identity phase locking and transient phase reset.
class Fft {
public:
    explicit Fft(int n) : n_(n), rev_(static_cast<size_t>(n)), tw_(static_cast<size_t>(n / 2)) {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[static_cast<size_t>(i)] = r;
        }
        for (int i = 0; i < n / 2; ++i) tw_[static_cast<size_t>(i)] = std::polar(1.0, -2.0 * kPi * i / n);
    }
    void run(std::vector<std::complex<double>>& x, bool inverse) const {
        for (int i = 0; i < n_; ++i)
            if (i < rev_[static_cast<size_t>(i)]) std::swap(x[static_cast<size_t>(i)], x[static_cast<size_t>(rev_[static_cast<size_t>(i)])]);
        for (int len = 2; len <= n_; len <<= 1) {
            const int stride = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    auto w = tw_[static_cast<size_t>(j * stride)];
                    if (inverse) w = std::conj(w);
                    auto& a = x[static_cast<size_t>(i + j)];
                    auto& b = x[static_cast<size_t>(i + j + len / 2)];
                    const auto t = w * b;
                    b = a - t;
                    a += t;
                }
        }
        if (inverse)
            for (auto& v : x) v /= n_;
    }

private:
    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<double>> tw_;
};

double wrapPhase(double p) { return p - 2.0 * kPi * std::floor((p + kPi) / (2.0 * kPi)); }

AudioBuffer phaseVocoder(const Planar& in, double ratio, double rate, int64_t outFrames) {
    int N = 1024;
    while (N < 0.085 * rate) N <<= 1;  // 4096 at 44.1/48 kHz: fine enough to separate chord notes
    const int Hs = N / 4;
    const int bins = N / 2 + 1;
    const auto win = hann(N);
    const Fft fft(N);
    const size_t nch = in.ch.size();

    std::vector<std::vector<double>> out(nch, std::vector<double>(static_cast<size_t>(outFrames)));
    std::vector<double> weight(static_cast<size_t>(outFrames));
    std::vector<std::vector<std::complex<double>>> X(nch, std::vector<std::complex<double>>(static_cast<size_t>(N)));
    std::vector<std::complex<double>> buf(static_cast<size_t>(N));
    std::vector<double> mag(static_cast<size_t>(bins)), phase(static_cast<size_t>(bins)), synth(static_cast<size_t>(bins));
    std::vector<double> prevMag(static_cast<size_t>(bins)), prevPhase(static_cast<size_t>(bins)),
        prevSynth(static_cast<size_t>(bins)), inst(static_cast<size_t>(bins));
    std::vector<int> peaks;
    bool first = true;
    int64_t prevA = 0;

    for (int64_t k = -(N / 2) / Hs;; ++k) {
        const int64_t outStart = k * Hs - N / 2;
        if (outStart >= outFrames) break;
        const int64_t a = std::llround(static_cast<double>(k * Hs) / ratio);  // analysis centre
        for (size_t c = 0; c < nch; ++c) {
            auto& x = X[c];
            for (int i = 0; i < N; ++i) x[static_cast<size_t>(i)] = in.at(c, a - N / 2 + i) * win[static_cast<size_t>(i)];
            fft.run(x, false);
        }
        // Phase is tracked on the channel sum and applied as a common rotation to each channel,
        // which keeps the stereo image (inter-channel phase) intact.
        double flux = 0, prevEnergy = 1e-12, energy = 0;
        for (int b = 0; b < bins; ++b) {
            std::complex<double> m{};
            for (size_t c = 0; c < nch; ++c) m += X[c][static_cast<size_t>(b)];
            mag[static_cast<size_t>(b)] = std::abs(m);
            phase[static_cast<size_t>(b)] = std::arg(m);
            flux += std::max(0.0, mag[static_cast<size_t>(b)] - prevMag[static_cast<size_t>(b)]);
            prevEnergy += prevMag[static_cast<size_t>(b)];
            energy += mag[static_cast<size_t>(b)];
        }
        // A sharp onset (drum hit, pluck) restarts the phases so the attack stays crisp.
        const bool transient = !first && energy > 1e-6 && flux > 0.8 * prevEnergy;
        if (first || transient) {
            synth = phase;
        } else {
            const auto Ha = static_cast<double>(std::max<int64_t>(1, a - prevA));
            for (int b = 0; b < bins; ++b) {
                const double omega = 2.0 * kPi * b / N;
                const double dphi = wrapPhase(phase[static_cast<size_t>(b)] - prevPhase[static_cast<size_t>(b)] - omega * Ha);
                inst[static_cast<size_t>(b)] = omega + dphi / Ha;
            }
            peaks.clear();
            double maxMag = 0;
            for (double v : mag) maxMag = std::max(maxMag, v);
            for (int b = 0; b < bins; ++b) {
                const double v = mag[static_cast<size_t>(b)];
                if (v < maxMag * 1e-5) continue;
                bool isPeak = true;
                for (int d = -2; d <= 2 && isPeak; ++d)
                    if (d != 0 && b + d >= 0 && b + d < bins && mag[static_cast<size_t>(b + d)] > v) isPeak = false;
                if (isPeak) peaks.push_back(b);
            }
            if (peaks.empty()) {
                for (int b = 0; b < bins; ++b)
                    synth[static_cast<size_t>(b)] = prevSynth[static_cast<size_t>(b)] + inst[static_cast<size_t>(b)] * Hs;
            } else {
                // Each peak advances at its own frequency; the bins around it keep their phase
                // relationship to the peak ("identity phase locking"), which avoids phasiness.
                int from = 0;
                for (size_t i = 0; i < peaks.size(); ++i) {
                    const int p = peaks[i];
                    int to = bins - 1;
                    if (i + 1 < peaks.size()) {  // region ends at the quietest bin before the next peak
                        to = p;
                        for (int b = p; b <= peaks[i + 1]; ++b)
                            if (mag[static_cast<size_t>(b)] < mag[static_cast<size_t>(to)]) to = b;
                    }
                    const double sp = prevSynth[static_cast<size_t>(p)] + inst[static_cast<size_t>(p)] * Hs;
                    for (int b = from; b <= to; ++b)
                        synth[static_cast<size_t>(b)] = sp + (phase[static_cast<size_t>(b)] - phase[static_cast<size_t>(p)]);
                    from = to + 1;
                }
            }
        }
        for (size_t c = 0; c < nch; ++c) {
            for (int b = 0; b < bins; ++b) {
                const double rot = synth[static_cast<size_t>(b)] - phase[static_cast<size_t>(b)];
                buf[static_cast<size_t>(b)] = X[c][static_cast<size_t>(b)] * std::polar(1.0, rot);
            }
            for (int b = 1; b < N / 2; ++b) buf[static_cast<size_t>(N - b)] = std::conj(buf[static_cast<size_t>(b)]);
            fft.run(buf, true);
            for (int i = 0; i < N; ++i) {
                const int64_t o = outStart + i;
                if (o < 0 || o >= outFrames) continue;
                const double w = win[static_cast<size_t>(i)];
                out[c][static_cast<size_t>(o)] += buf[static_cast<size_t>(i)].real() * w;
                if (c == 0) weight[static_cast<size_t>(o)] += w * w;
            }
        }
        for (int b = 0; b < bins; ++b) synth[static_cast<size_t>(b)] = wrapPhase(synth[static_cast<size_t>(b)]);
        prevMag = mag;
        prevPhase = phase;
        prevSynth = synth;
        prevA = a;
        first = false;
    }
    return fromPlanar(out, weight, outFrames, rate);
}

const AudioClip* findClipIn(const Project& p, const std::string& trackId, const std::string& clipId) {
    const Track* t = p.findTrack(trackId);
    if (!t) return nullptr;
    for (auto& c : t->audioClips)
        if (c.id == clipId) return &c;
    return nullptr;
}

// The part of the original (unstretched) media that the clip currently shows.
bool originalRegion(const Project& p, const AudioClip& c, const MediaRef** orig, int64_t* start, int64_t* length) {
    const bool stretched = !c.stretchSourceMediaId.empty();
    *orig = p.findMedia(stretched ? c.stretchSourceMediaId : c.mediaId);
    if (!*orig) return false;
    const double prevRatio = clipStretchRatio(c);
    const double visible = static_cast<double>(framesSpanning(p, c.startTick, c.lengthTick, (*orig)->sampleRate));
    *start = stretched ? c.stretchSourceOffsetFrames + std::llround(static_cast<double>(c.sourceOffsetFrames) / prevRatio)
                       : c.sourceOffsetFrames;
    *start = std::clamp<int64_t>(*start, 0, (*orig)->frames);
    *length = std::min<int64_t>(std::llround(visible / prevRatio), (*orig)->frames - *start);
    return *length > 0;
}
}  // namespace

const char* stretchModeId(StretchMode m) {
    switch (m) {
        case StretchMode::Mono: return "mono";
        case StretchMode::Poly: return "poly";
        case StretchMode::Varispeed: return "varispeed";
    }
    return "poly";
}

const char* stretchModeName(StretchMode m) {
    switch (m) {
        case StretchMode::Mono: return "Mono";
        case StretchMode::Poly: return "Poly";
        case StretchMode::Varispeed: return "Varispeed";
    }
    return "Poly";
}

std::optional<StretchMode> stretchModeFromId(const std::string& id) {
    for (auto m : {StretchMode::Mono, StretchMode::Poly, StretchMode::Varispeed})
        if (id == stretchModeId(m)) return m;
    return std::nullopt;
}

AudioBuffer timeStretch(const AudioBuffer& in, double ratio, StretchMode mode) {
    const int64_t outFrames = std::max<int64_t>(1, std::llround(static_cast<double>(in.frames()) * ratio));
    if (in.frames() <= 0 || in.channels <= 0) {
        AudioBuffer empty;
        empty.sampleRate = in.sampleRate;
        empty.channels = std::max(1, in.channels);
        return empty;
    }
    const auto planar = toPlanar(in);
    switch (mode) {
        case StretchMode::Varispeed: return varispeed(planar, in.sampleRate, outFrames);
        case StretchMode::Mono: return wsola(planar, ratio, in.sampleRate, outFrames);
        case StretchMode::Poly: return phaseVocoder(planar, ratio, in.sampleRate, outFrames);
    }
    return in;
}

bool StretchJob::reset() const { return std::abs(ratio - 1.0) < 1e-6; }

double clipStretchRatio(const AudioClip& c) { return c.stretchSourceMediaId.empty() ? 1.0 : c.stretchRatio; }

std::optional<StretchMode> clipStretchMode(const AudioClip& c) {
    if (c.stretchSourceMediaId.empty()) return std::nullopt;
    return stretchModeFromId(c.stretchMode);
}

std::optional<StretchJob> prepareStretch(const Project& p, MediaProvider& media, const std::string& trackId,
                                         const std::string& clipId, double ratio, StretchMode mode,
                                         std::string* error) {
    auto fail = [&](const char* msg) -> std::optional<StretchJob> {
        if (error) *error = msg;
        return std::nullopt;
    };
    const AudioClip* c = findClipIn(p, trackId, clipId);
    if (!c) return fail("That clip no longer exists.");
    if (!(ratio >= kMinStretchRatio - 1e-9 && ratio <= kMaxStretchRatio + 1e-9))
        return fail("Clips can be stretched from 25% to 400% of their original length.");
    const MediaRef* orig = nullptr;
    int64_t start = 0, length = 0;
    if (!originalRegion(p, *c, &orig, &start, &length)) return fail("The clip's original audio is missing.");
    StretchJob job;
    job.trackId = trackId;
    job.clipId = clipId;
    job.sourceMediaId = orig->id;
    job.sourceOffsetFrames = start;
    job.ratio = ratio;
    job.mode = mode;
    job.source.sampleRate = orig->sampleRate;
    job.source.channels = orig->channels;
    if (job.reset()) return job;  // nothing to render
    const AudioBuffer* src = media.get(*orig);
    if (!src || src->channels <= 0) return fail("The clip's original audio couldn't be loaded.");
    job.source.channels = src->channels;
    length = std::min(length, src->frames() - start);
    if (length <= 0) return fail("The clip's original audio is missing.");
    const auto ch = static_cast<size_t>(src->channels);
    job.source.samples.assign(src->samples.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(start) * ch),
                              src->samples.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(start + length) * ch));
    return job;
}

AudioBuffer renderStretch(const StretchJob& job) {
    if (job.reset()) return {};
    return timeStretch(job.source, job.ratio, job.mode);
}

bool commitStretch(Project& p, const std::string& packageDir, const StretchJob& job, const AudioBuffer& rendered) {
    const AudioClip* existing = findClipIn(p, job.trackId, job.clipId);
    if (!existing) return false;
    AudioClip c = *existing;
    if (job.reset()) {
        const MediaRef* orig = p.findMedia(job.sourceMediaId);
        if (!orig) return false;
        const AudioClip* cur = existing;
        const MediaRef* o = nullptr;
        int64_t start = 0, length = 0;
        if (!originalRegion(p, *cur, &o, &start, &length)) return false;
        c.mediaId = job.sourceMediaId;
        c.sourceOffsetFrames = start;
        c.lengthTick = std::max<int64_t>(kMinClipTicks, ticksSpanning(p, c.startTick, length, orig->sampleRate));
        c.stretchSourceMediaId.clear();
        c.stretchSourceOffsetFrames = 0;
        c.stretchRatio = 1.0;
        c.stretchMode.clear();
    } else {
        if (rendered.frames() <= 0) return false;
        c.mediaId = addMedia(p, packageDir, rendered, "stretch").id;
        c.sourceOffsetFrames = 0;
        c.lengthTick = std::max<int64_t>(kMinClipTicks, ticksSpanning(p, c.startTick, rendered.frames(), rendered.sampleRate));
        c.stretchSourceMediaId = job.sourceMediaId;
        c.stretchSourceOffsetFrames = job.sourceOffsetFrames;
        c.stretchRatio = job.ratio;
        c.stretchMode = stretchModeId(job.mode);
    }
    Track* t = p.findTrack(job.trackId);
    t->audioClips.erase(std::remove_if(t->audioClips.begin(), t->audioClips.end(),
                                       [&](const AudioClip& x) { return x.id == job.clipId; }),
                        t->audioClips.end());
    placeClipReplacing(p, *t, std::move(c));  // a longer clip covers what follows, like recording over it
    return true;
}

bool stretchClip(Project& p, const std::string& packageDir, MediaProvider& media, const std::string& trackId,
                 const std::string& clipId, double ratio, StretchMode mode, std::string* error) {
    auto job = prepareStretch(p, media, trackId, clipId, ratio, mode, error);
    if (!job) return false;
    return commitStretch(p, packageDir, *job, renderStretch(*job));
}

double ratioForTempo(const Project& p, double sourceBpm, int64_t atTick) {
    const double bpm = p.tempoAt(atTick);
    if (sourceBpm <= 0 || bpm <= 0) return 1.0;
    return std::clamp(sourceBpm / bpm, kMinStretchRatio, kMaxStretchRatio);
}

double ratioToFitBars(const Project& p, const AudioClip& c) {
    const MediaRef* orig = nullptr;
    int64_t start = 0, length = 0;
    if (!originalRegion(p, c, &orig, &start, &length)) return 1.0;
    // Measured from where the clip sits, so tempo changes under it are respected.
    const double seconds = static_cast<double>(length) / orig->sampleRate;
    const TempoMap m(p);
    const double t0 = m.tickToSeconds(static_cast<double>(c.startTick));
    const double ticks = m.secondsToTick(t0 + seconds) - static_cast<double>(c.startTick);
    const auto barTicks = static_cast<double>(p.barTicks());
    const double bars = std::max(1.0, std::round(ticks / barTicks));
    const double target = m.tickToSeconds(static_cast<double>(c.startTick) + bars * barTicks) - t0;
    return std::clamp(target / seconds, kMinStretchRatio, kMaxStretchRatio);
}

}  // namespace ynot
