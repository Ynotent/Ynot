#include "ynot/pitch_correct.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ynot {

namespace {
constexpr double kMinHz = 70.0, kMaxHz = 1000.0;

// YIN on a decimated mono signal. Returns the period in (decimated) samples, or 0.
double yinPeriod(const float* x, int n, int tauMin, int tauMax, std::vector<double>& d) {
    if (n <= tauMax + 16) return 0;
    const int w = n - tauMax;
    d.assign(static_cast<size_t>(tauMax + 1), 0.0);
    double energy = 0;
    for (int j = 0; j < w; ++j) energy += static_cast<double>(x[j]) * x[j];
    if (energy / w < 1e-6) return 0;  // too quiet to call (about -60 dBFS)
    for (int tau = 1; tau <= tauMax; ++tau) {
        double s = 0;
        for (int j = 0; j < w; ++j) {
            const double diff = static_cast<double>(x[j]) - x[j + tau];
            s += diff * diff;
        }
        d[static_cast<size_t>(tau)] = s;
    }
    // Cumulative mean normalised difference.
    double running = 0;
    d[0] = 1;
    for (int tau = 1; tau <= tauMax; ++tau) {
        running += d[static_cast<size_t>(tau)];
        d[static_cast<size_t>(tau)] = running > 0 ? d[static_cast<size_t>(tau)] * tau / running : 1;
    }
    int best = -1;
    for (int tau = std::max(2, tauMin); tau < tauMax; ++tau)
        if (d[static_cast<size_t>(tau)] < 0.15) {
            while (tau + 1 < tauMax && d[static_cast<size_t>(tau + 1)] < d[static_cast<size_t>(tau)]) ++tau;
            best = tau;
            break;
        }
    if (best < 0) return 0;  // not clearly pitched (breath, consonant, noise)
    // Parabolic interpolation for sub-sample precision (a few cents).
    const double a = d[static_cast<size_t>(best - 1)], b = d[static_cast<size_t>(best)], c = d[static_cast<size_t>(best + 1)];
    const double denom = a - 2 * b + c;
    return best + (std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0);
}

int decimationFor(double sr) { return std::max(1, static_cast<int>(std::lround(sr / 12000.0))); }

class PitchCorrector : public PluginProcessor {
public:
    PitchCorrector(const Json& params, double sr) : sr_(sr) {
        key_ = static_cast<int>(params.getNumber("key", 0));
        scale_ = static_cast<int>(params.getNumber("scale", 1));
        speedMs_ = params.getNumber("speed", 20);
        humanize_ = params.getNumber("humanize", 0);
        flexCents_ = params.getNumber("flex", 0);
        mix_ = params.getNumber("mix", 1);
        reference_ = params.getNumber("reference", 440);
        dec_ = decimationFor(sr);
        decRate_ = sr / dec_;
        center_ = static_cast<int>(std::lround(0.016 * sr));  // nominal delay: room for 70 Hz periods
        for (auto& r : ring_) r.assign(kRing, 0.f);
        decHist_.assign(static_cast<size_t>(kDecWindow), 0.f);
        scratch_.reserve(static_cast<size_t>(decRate_ / kMinHz) + 2);
        reset();
    }

    int latencySamples() const override { return center_; }

    void process(float* l, float* r, int frames, const MidiEvent*, int) override {
        float* ch[2] = {l, r};
        for (int i = 0; i < frames; ++i) {
            // Analysis: decimate the mono sum; run pitch detection every hop.
            const float mono = 0.5f * (l[i] + r[i]);
            decAcc_ += mono;
            if (++decCount_ == dec_) {
                pushDecimated(decAcc_ / static_cast<float>(dec_));
                decAcc_ = 0;
                decCount_ = 0;
                if (++sinceDetect_ >= kHop) {
                    sinceDetect_ = 0;
                    analyse();
                }
            }
            // Smooth the correction (retune speed) and update the shift ratio.
            correction_ += (desired_ - correction_) * smoothing_;
            if ((i & 15) == 0) ratio_ = std::pow(2.0, correction_ / 12.0);

            // Write, then read through the two crossfading taps.
            for (int c = 0; c < 2; ++c) ring_[static_cast<size_t>(c)][static_cast<size_t>(w_ & kMask)] = ch[c][i];
            ++w_;
            const double g = xfadeLeft_ > 0 ? static_cast<double>(xfadeLeft_) / xfadeLen_ : 0.0;
            for (int c = 0; c < 2; ++c) {
                const auto& ring = ring_[static_cast<size_t>(c)];
                double wet = read(ring, main_);
                if (xfadeLeft_ > 0) wet = wet * (1 - g) + read(ring, old_) * g;
                const double dry = ring[static_cast<size_t>((w_ - 1 - center_) & kMask)];
                ch[c][i] = static_cast<float>(dry + (wet - dry) * mix_);
            }
            main_ += ratio_;
            if (xfadeLeft_ > 0) {
                old_ += ratio_;
                --xfadeLeft_;
            }
            keepDelayCentred();
        }
    }

    void setParameter(const std::string& id, double v) override {
        if (id == "key") key_ = static_cast<int>(v);
        else if (id == "scale") scale_ = static_cast<int>(v);
        else if (id == "speed") speedMs_ = v;
        else if (id == "humanize") humanize_ = v;
        else if (id == "flex") flexCents_ = v;
        else if (id == "mix") mix_ = v;
        else if (id == "reference") reference_ = v;
    }

    void reset() override {
        for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.f);
        std::fill(decHist_.begin(), decHist_.end(), 0.f);
        w_ = 0;
        main_ = old_ = -static_cast<double>(center_);
        xfadeLeft_ = 0;
        decAcc_ = 0;
        decCount_ = sinceDetect_ = 0;
        correction_ = desired_ = 0;
        ratio_ = 1;
        period_ = 0;
        heldNote_ = -1;
        heldSamples_ = 0;
        smoothing_ = 1;
    }

private:
    static constexpr int kRing = 1 << 14, kMask = kRing - 1;
    static constexpr int kDecWindow = 512, kHop = 64;

    void pushDecimated(float v) {
        std::move(decHist_.begin() + 1, decHist_.end(), decHist_.begin());
        decHist_.back() = v;
    }

    void analyse() {
        const int tauMax = static_cast<int>(decRate_ / kMinHz), tauMin = static_cast<int>(decRate_ / kMaxHz);
        const double p = yinPeriod(decHist_.data(), kDecWindow, tauMin, tauMax, scratch_);
        const int hopSamples = kHop * dec_;
        if (p <= 0) {  // unpitched: drift back to no shift
            period_ = 0;
            desired_ = 0;
            heldNote_ = -1;
            setSmoothing(speedMs_);
            return;
        }
        const double hz = decRate_ / p;
        period_ = sr_ / hz;
        const double midi = 69.0 + 12.0 * std::log2(hz / reference_);
        const int target = nearestScaleNote(midi, key_, scale_);
        double want = target - midi;
        if (flexCents_ > 0 && std::abs(want) * 100.0 > flexCents_) want = 0;  // leave slides and bends alone
        desired_ = want;
        // Humanize: once a note is held, wobble around it (vibrato) is corrected more gently.
        if (target == heldNote_) heldSamples_ += hopSamples;
        else {
            heldNote_ = target;
            heldSamples_ = 0;
        }
        const bool sustained = heldSamples_ > 0.15 * sr_;
        setSmoothing(speedMs_ + (sustained ? humanize_ * 300.0 : 0.0));
    }

    void setSmoothing(double ms) {
        smoothing_ = ms <= 0.5 ? 1.0 : 1.0 - std::exp(-1.0 / (ms * 0.001 * sr_));
    }

    static double read(const std::vector<float>& ring, double pos) {
        const auto i0 = static_cast<int64_t>(std::floor(pos));
        const double f = pos - static_cast<double>(i0);
        const float a = ring[static_cast<size_t>(i0 & kMask)], b = ring[static_cast<size_t>((i0 + 1) & kMask)];
        return a + (b - a) * f;
    }

    // Reading faster (pitch up) or slower (down) than writing makes the delay drift; when it
    // leaves its band, jump the read point by whole pitch periods (so the waveform lines up) and
    // crossfade from the old position.
    void keepDelayCentred() {
        const double delay = static_cast<double>(w_ - 1) - main_;
        const double jump = period_ > 0 ? period_ : 0.01 * sr_;
        const double off = delay - center_;
        if (std::abs(off) < 0.6 * jump) return;
        const double m = std::max(1.0, std::round(std::abs(off) / jump));
        old_ = main_;
        main_ += off > 0 ? m * jump : -m * jump;
        xfadeLen_ = std::clamp(static_cast<int>(jump * 0.5), 32, static_cast<int>(0.008 * sr_));
        xfadeLeft_ = xfadeLen_;
    }

    double sr_;
    int key_ = 0, scale_ = 1;
    double speedMs_ = 20, humanize_ = 0, flexCents_ = 0, mix_ = 1, reference_ = 440;
    int dec_ = 4;
    double decRate_ = 12000;
    int center_ = 768;
    std::array<std::vector<float>, 2> ring_;
    std::vector<float> decHist_;
    std::vector<double> scratch_;
    int64_t w_ = 0;
    double main_ = 0, old_ = 0;
    int xfadeLeft_ = 0, xfadeLen_ = 1;
    float decAcc_ = 0;
    int decCount_ = 0, sinceDetect_ = 0;
    double correction_ = 0, desired_ = 0, ratio_ = 1, period_ = 0, smoothing_ = 1;
    int heldNote_ = -1;
    double heldSamples_ = 0;
};
}  // namespace

const std::vector<ScaleInfo>& tuneScales() {
    static const std::vector<ScaleInfo> s = {
        {"chromatic", "Chromatic (every note)", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
        {"major", "Major", {0, 2, 4, 5, 7, 9, 11}},
        {"minor", "Minor", {0, 2, 3, 5, 7, 8, 10}},
        {"harmonic-minor", "Harmonic minor", {0, 2, 3, 5, 7, 8, 11}},
        {"major-pentatonic", "Major pentatonic", {0, 2, 4, 7, 9}},
        {"minor-pentatonic", "Minor pentatonic", {0, 3, 5, 7, 10}},
        {"blues", "Blues", {0, 3, 5, 6, 7, 10}},
        {"dorian", "Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"mixolydian", "Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
    };
    return s;
}

const char* keyName(int key) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((key % 12) + 12) % 12];
}

int nearestScaleNote(double midi, int key, int scale) {
    const auto& scales = tuneScales();
    const auto& steps = scales[static_cast<size_t>(std::clamp(scale, 0, static_cast<int>(scales.size()) - 1))].steps;
    int best = static_cast<int>(std::lround(midi));
    double bestDist = 1e9;
    for (int n = static_cast<int>(std::floor(midi)) - 7; n <= static_cast<int>(std::ceil(midi)) + 7; ++n) {
        const int degree = (((n - key) % 12) + 12) % 12;
        if (std::find(steps.begin(), steps.end(), degree) == steps.end()) continue;
        const double dist = std::abs(n - midi);
        if (dist < bestDist) bestDist = dist, best = n;
    }
    return best;
}

double detectPitchHz(const float* samples, int count, double sampleRate) {
    const int dec = decimationFor(sampleRate);
    const double rate = sampleRate / dec;
    std::vector<float> x;
    for (int i = 0; i + dec <= count; i += dec) {
        float s = 0;
        for (int k = 0; k < dec; ++k) s += samples[i + k];
        x.push_back(s / static_cast<float>(dec));
    }
    std::vector<double> scratch;
    const double p = yinPeriod(x.data(), static_cast<int>(x.size()), static_cast<int>(rate / kMaxHz),
                               static_cast<int>(rate / kMinHz), scratch);
    return p > 0 ? rate / p : 0.0;
}

std::unique_ptr<PluginProcessor> makePitchCorrector(const Json& params, double sampleRate) {
    return std::make_unique<PitchCorrector>(params, sampleRate);
}

}  // namespace ynot
