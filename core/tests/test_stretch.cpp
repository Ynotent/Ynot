#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr double kTau = 6.283185307179586;

AudioBuffer tone(std::initializer_list<double> freqs, double seconds, double rate = 48000, int channels = 2) {
    AudioBuffer b;
    b.sampleRate = rate;
    b.channels = channels;
    const auto n = static_cast<int64_t>(seconds * rate);
    const double amp = 0.5 / static_cast<double>(freqs.size());
    for (int64_t i = 0; i < n; ++i) {
        double v = 0;
        for (double f : freqs) v += amp * std::sin(kTau * f * static_cast<double>(i) / rate);
        for (int c = 0; c < channels; ++c) b.samples.push_back(static_cast<float>(v));
    }
    return b;
}

double channelSample(const AudioBuffer& b, int64_t f, int c = 0) {
    return b.samples[static_cast<size_t>(f * b.channels + c)];
}

// Frequency from upward zero crossings in [from, to) of channel 0.
double zeroCrossingHz(const AudioBuffer& b, int64_t from, int64_t to) {
    int64_t first = -1, last = -1, count = 0;
    for (int64_t f = from + 1; f < to; ++f)
        if (channelSample(b, f - 1) < 0 && channelSample(b, f) >= 0) {
            if (first < 0) first = f;
            else ++count;
            last = f;
        }
    return count > 0 ? static_cast<double>(count) * b.sampleRate / static_cast<double>(last - first) : 0.0;
}

// Power at one frequency over [from, to) (Goertzel).
double powerAt(const AudioBuffer& b, double hz, int64_t from, int64_t to) {
    const double w = kTau * hz / b.sampleRate;
    double s1 = 0, s2 = 0;
    for (int64_t f = from; f < to; ++f) {
        const double win = 0.5 - 0.5 * std::cos(kTau * static_cast<double>(f - from) / static_cast<double>(to - from));
        const double s = channelSample(b, f) * win + 2 * std::cos(w) * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - 2 * std::cos(w) * s1 * s2;
}

double maxStep(const AudioBuffer& b, int64_t from, int64_t to) {
    double m = 0;
    for (int64_t f = from + 1; f < to; ++f) m = std::max(m, std::abs(channelSample(b, f) - channelSample(b, f - 1)));
    return m;
}
}  // namespace

TEST("stretch: Mono and Poly change length but keep pitch and level") {
    const AudioBuffer in = tone({220.0}, 1.0);
    for (auto mode : {StretchMode::Mono, StretchMode::Poly})
        for (double ratio : {0.5, 0.75, 1.5, 2.0}) {
            AudioBuffer out = timeStretch(in, ratio, mode);
            CHECK_EQ(out.frames(), std::llround(48000 * ratio));
            CHECK_EQ(out.channels, 2);
            const int64_t a = out.frames() / 5, b = out.frames() * 4 / 5;  // ignore the edges
            CHECK_NEAR(zeroCrossingHz(out, a, b), 220.0, 2.0);
            CHECK_NEAR(th::rms(out, a, b), th::rms(in, 4800, 43200), 0.04);
            // Smooth: no clicks bigger than the tone's own steepest slope (0.5 * 2pi * 220 / 48k = 0.0144).
            CHECK(maxStep(out, a, b) < 0.03);
            // Stereo stays identical when the input is.
            double diff = 0;
            for (int64_t f = a; f < b; ++f) diff = std::max(diff, std::abs(channelSample(out, f, 0) - channelSample(out, f, 1)));
            CHECK(diff < 1e-5);
        }
}

TEST("stretch: Poly keeps every note of a chord in tune") {
    const AudioBuffer in = tone({261.63, 329.63, 392.0}, 1.5);  // C major
    AudioBuffer out = timeStretch(in, 1.6, StretchMode::Poly);
    const int64_t a = out.frames() / 4, b = out.frames() * 3 / 4;
    const double noise = powerAt(out, 295.0, a, b);  // between the notes
    for (double hz : {261.63, 329.63, 392.0}) CHECK(powerAt(out, hz, a, b) > 300 * noise);
    // A semitone off would show up strongly at the neighbouring frequency.
    CHECK(powerAt(out, 261.63, a, b) > 300 * powerAt(out, 277.18, a, b));
}

TEST("stretch: Poly at 100% reproduces the input") {
    const AudioBuffer in = tone({261.63, 440.0, 1234.5}, 0.5);
    AudioBuffer out = timeStretch(in, 1.0, StretchMode::Poly);
    double err = 0;
    for (int64_t f = 0; f < in.frames(); ++f) err = std::max(err, std::abs(channelSample(out, f) - channelSample(in, f)));
    CHECK(err < 1e-4);
}

TEST("stretch: Varispeed changes pitch with speed, like tape") {
    const AudioBuffer in = tone({440.0}, 1.0);
    AudioBuffer slow = timeStretch(in, 2.0, StretchMode::Varispeed);
    AudioBuffer fast = timeStretch(in, 0.5, StretchMode::Varispeed);
    CHECK_EQ(slow.frames(), int64_t(96000));
    CHECK_NEAR(zeroCrossingHz(slow, 10000, 86000), 220.0, 1.0);
    CHECK_NEAR(zeroCrossingHz(fast, 2000, 22000), 880.0, 2.0);
    CHECK_NEAR(th::rms(fast, 2000, 22000), th::rms(in, 2000, 46000), 0.01);
    // Speeding up doesn't fold high content back down as aliasing: 20 kHz at 2x would alias to 8 kHz.
    AudioBuffer high = timeStretch(tone({20000.0}, 0.5), 0.5, StretchMode::Varispeed);
    CHECK(th::rms(high, 1000, 11000) < 0.01);
}

TEST("stretch: modes have stable ids") {
    for (auto m : {StretchMode::Mono, StretchMode::Poly, StretchMode::Varispeed})
        CHECK(stretchModeFromId(stretchModeId(m)) == m);
    CHECK(!stretchModeFromId("nonsense"));
}

TEST("stretch clip: non-destructive, re-stretches from the original, resets exactly") {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    Track t;
    t.id = "t";
    t.kind = TrackKind::Audio;
    p.tracks = {t};
    const auto dir = th::tempDir("stretch");
    const std::string clipId = importAudioClip(p, dir, "t", tone({220.0}, 1.0), 0);  // 1 s = 2 beats
    const std::string original = p.findTrack("t")->audioClips[0].mediaId;
    PackageMediaProvider media(dir);
    auto clip = [&]() -> const AudioClip& { return p.findTrack("t")->audioClips.at(0); };
    CHECK_EQ(clip().lengthTick, 2 * kTicksPerQuarter);

    std::string err;
    CHECK(stretchClip(p, dir, media, "t", clipId, 2.0, StretchMode::Poly, &err));
    CHECK_EQ(clip().lengthTick, 4 * kTicksPerQuarter);
    CHECK(clip().mediaId != original);
    CHECK_EQ(clip().stretchSourceMediaId, original);
    CHECK(clipStretchMode(clip()) == StretchMode::Poly);
    CHECK_EQ(p.findMedia(clip().mediaId)->origin, std::string("stretch"));

    // The ratio is always relative to the original: 0.5 now means half the original length.
    CHECK(stretchClip(p, dir, media, "t", clipId, 0.5, StretchMode::Mono, &err));
    CHECK_EQ(clip().lengthTick, kTicksPerQuarter);
    CHECK_EQ(clip().stretchSourceMediaId, original);
    CHECK_NEAR(clipStretchRatio(clip()), 0.5, 1e-9);

    // Save / load keeps the stretch.
    saveProjectPackage(p, dir);
    Project loaded = loadProjectPackage(dir);
    CHECK(projectToJson(loaded) == projectToJson(p));

    // Reset brings back the original media and length.
    CHECK(stretchClip(p, dir, media, "t", clipId, 1.0, StretchMode::Poly, &err));
    CHECK_EQ(clip().mediaId, original);
    CHECK_EQ(clip().lengthTick, 2 * kTicksPerQuarter);
    CHECK_EQ(clip().sourceOffsetFrames, int64_t(0));
    CHECK(clip().stretchSourceMediaId.empty());

    CHECK(!stretchClip(p, dir, media, "t", clipId, 5.0, StretchMode::Poly, &err));
    CHECK(!err.empty());
}

TEST("stretch clip: a split half of a stretched clip re-stretches from its own part of the original") {
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    Track t;
    t.id = "t";
    t.kind = TrackKind::Audio;
    p.tracks = {t};
    const auto dir = th::tempDir("stretch-split");
    // 1 s: first half 220 Hz, second half 440 Hz.
    AudioBuffer a = tone({220.0}, 0.5), b = tone({440.0}, 0.5);
    a.samples.insert(a.samples.end(), b.samples.begin(), b.samples.end());
    const std::string clipId = importAudioClip(p, dir, "t", a, 0);
    PackageMediaProvider media(dir);
    CHECK(stretchClip(p, dir, media, "t", clipId, 2.0, StretchMode::Poly));  // now 2 s = 4 beats
    auto right = splitClip(p, "t", clipId, 2 * kTicksPerQuarter);              // at 1 s: the 440 Hz half
    CHECK(right.has_value());
    // Back to 100%: the right half becomes the original 440 Hz half, 0.5 s long.
    CHECK(stretchClip(p, dir, media, "t", *right, 1.0, StretchMode::Poly));
    const AudioClip* r = nullptr;
    for (auto& c : p.findTrack("t")->audioClips)
        if (c.id == *right) r = &c;
    CHECK(r != nullptr);
    CHECK_EQ(r->lengthTick, kTicksPerQuarter);
    CHECK_EQ(r->sourceOffsetFrames, int64_t(24000));
    CHECK_EQ(r->startTick, 2 * kTicksPerQuarter);
}

TEST("stretch: tempo and bar helpers") {
    Project p;
    p.tempoBpm = 120;
    CHECK_NEAR(ratioForTempo(p, 90), 0.75, 1e-12);  // a 90 BPM loop plays faster at 120
    CHECK_NEAR(ratioForTempo(p, 0), 1.0, 1e-12);
    MediaRef m;
    m.id = "m";
    m.sampleRate = 48000;
    m.frames = 48000 * 3;  // 3 s
    p.media.push_back(m);
    AudioClip c;
    c.mediaId = "m";
    c.lengthTick = 6 * kTicksPerQuarter;  // 3 s at 120 BPM = 1.5 bars of 4/4 -> 2 bars
    CHECK_NEAR(ratioToFitBars(p, c), 4.0 / 3.0, 1e-9);
}
