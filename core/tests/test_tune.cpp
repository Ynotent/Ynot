#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
constexpr double kSr = 48000;
constexpr double kTau = 6.283185307179586;

// A voice-like tone: fundamental plus a few harmonics.
std::vector<float> voice(double hz, double seconds, double amp = 0.4) {
    std::vector<float> v(static_cast<size_t>(seconds * kSr));
    for (size_t i = 0; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / kSr;
        v[i] = static_cast<float>(amp * (std::sin(kTau * hz * t) + 0.5 * std::sin(2 * kTau * hz * t) +
                                         0.25 * std::sin(3 * kTau * hz * t) + 0.12 * std::sin(4 * kTau * hz * t)) /
                                  1.87);
    }
    return v;
}

// Runs a mono signal through Ynot Tune (both channels) and returns the left output.
std::vector<float> tune(const std::vector<float>& in, Json params) {
    auto fx = makePitchCorrector(params, kSr);
    std::vector<float> l = in, r = in;
    for (size_t done = 0; done < in.size(); done += 512) {
        const int n = static_cast<int>(std::min<size_t>(512, in.size() - done));
        fx->process(l.data() + done, r.data() + done, n, nullptr, 0);
    }
    return l;
}

double pitchAt(const std::vector<float>& x, double fromSec, double toSec) {
    const auto a = static_cast<size_t>(fromSec * kSr), b = static_cast<size_t>(toSec * kSr);
    return detectPitchHz(x.data() + a, static_cast<int>(b - a), kSr);
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

Json params(int key, int scale, double speed) {
    Json p = Json::object();
    p["key"] = static_cast<int64_t>(key);
    p["scale"] = static_cast<int64_t>(scale);
    p["speed"] = speed;
    return p;
}
}  // namespace

TEST("tune: pitch detection is accurate across the vocal range") {
    for (double hz : {82.4, 110.0, 220.0, 330.5, 440.0, 659.3, 880.0}) {
        auto v = voice(hz, 0.2);
        CHECK(std::abs(cents(detectPitchHz(v.data(), static_cast<int>(v.size()), kSr), hz)) < 3.0);
    }
    std::vector<float> silence(9600, 0.f);
    CHECK_EQ(detectPitchHz(silence.data(), 9600, kSr), 0.0);
    std::vector<float> noise(9600);
    uint32_t s = 1;
    for (auto& x : noise) {
        s = s * 1664525u + 1013904223u;
        x = static_cast<float>(s >> 8) / 16777216.f - 0.5f;
    }
    CHECK_EQ(detectPitchHz(noise.data(), 9600, kSr), 0.0);  // breath/noise: not pitched
}

TEST("tune: scales and keys pick the nearest allowed note") {
    CHECK_EQ(tuneScales().size(), size_t(9));
    CHECK_EQ(nearestScaleNote(70.14, 0, 1), 71);  // A#+ in C major -> B
    CHECK_EQ(nearestScaleNote(70.14, 0, 0), 70);  // chromatic -> A#
    CHECK_EQ(nearestScaleNote(69.4, 0, 1), 69);
    CHECK_EQ(nearestScaleNote(61.2, 2, 1), 61);   // D major has C#
    CHECK_EQ(nearestScaleNote(61.2, 0, 1), 62);   // C major has no C#: D is nearer than C
    CHECK_EQ(std::string(keyName(9)), std::string("A"));
}

TEST("tune: hard tuning pulls an out-of-tune note onto the scale, keeping its level") {
    auto in = voice(452.0, 1.5);  // A4 + 47 cents
    auto out = tune(in, params(0, 1, 0));
    CHECK(std::abs(cents(pitchAt(out, 0.6, 1.2), 440.0)) < 5.0);
    CHECK_NEAR(th::rms(AudioBuffer{kSr, 1, std::vector<float>(out.begin() + 30000, out.end())}),
               th::rms(AudioBuffer{kSr, 1, std::vector<float>(in.begin() + 30000, in.end())}), 0.03);

    auto between = voice(470.0, 1.5);  // between A# and B
    CHECK(std::abs(cents(pitchAt(tune(between, params(0, 1, 0)), 0.6, 1.2), 493.88)) < 5.0);  // C major: B
    CHECK(std::abs(cents(pitchAt(tune(between, params(0, 0, 0)), 0.6, 1.2), 466.16)) < 5.0);  // chromatic: A#
    auto low = voice(104.0, 1.5);  // a low male note, G#2 + 15 cents
    CHECK(std::abs(cents(pitchAt(tune(low, params(0, 0, 0)), 0.6, 1.2), 103.83)) < 5.0);
}

TEST("tune: retune speed eases notes in; flex-tune leaves bigger bends alone; mix 0 is clean") {
    auto in = voice(452.0, 1.5);
    auto fast = tune(in, params(0, 1, 0)), slow = tune(in, params(0, 1, 250));
    // Early on, the slow setting is still between the sung and the target pitch...
    const double early = std::abs(cents(pitchAt(slow, 0.10, 0.18), 440.0));
    CHECK(early > std::abs(cents(pitchAt(fast, 0.10, 0.18), 440.0)) + 5.0);
    // ...and gets there later.
    CHECK(std::abs(cents(pitchAt(slow, 1.1, 1.45), 440.0)) < 6.0);

    Json flex = params(0, 1, 0);
    flex["flex"] = 30.0;  // 47 cents off is more than 30: treated as expression
    CHECK(std::abs(cents(pitchAt(tune(in, flex), 0.6, 1.2), 452.0)) < 5.0);

    Json dry = params(0, 1, 0);
    dry["mix"] = 0.0;
    auto fx = makePitchCorrector(dry, kSr);
    const int lat = fx->latencySamples();
    CHECK(lat > 0 && lat < static_cast<int>(0.03 * kSr));
    auto out = tune(in, dry);
    double maxErr = 0;
    for (size_t i = static_cast<size_t>(lat); i < in.size(); ++i)
        maxErr = std::max(maxErr, static_cast<double>(std::abs(out[i] - in[i - static_cast<size_t>(lat)])));
    CHECK(maxErr < 1e-6);  // just the reported delay
}

TEST("tune: unpitched sounds pass through at their level") {
    std::vector<float> noise(48000);
    uint32_t s = 7;
    for (auto& x : noise) {
        s = s * 1664525u + 1013904223u;
        x = 0.3f * (static_cast<float>(s >> 8) / 16777216.f - 0.5f);
    }
    auto out = tune(noise, params(0, 1, 0));
    const double a = th::rms(AudioBuffer{kSr, 1, std::vector<float>(noise.begin() + 4800, noise.end())});
    const double b = th::rms(AudioBuffer{kSr, 1, std::vector<float>(out.begin() + 4800, out.end())});
    CHECK(std::abs(b - a) < 0.15 * a);
}

TEST("engine: plugin delay compensation keeps a track with Ynot Tune exactly in time") {
    auto dir = th::tempDir("pdc");
    Project p;
    p.id = "p";
    p.tempoBpm = 120;
    Track t;
    t.id = "v";
    t.kind = TrackKind::Audio;
    p.tracks = {t};
    AudioBuffer take;
    take.sampleRate = kSr;
    take.channels = 1;
    take.samples = voice(330.0, 1.0);
    importAudioClip(p, dir, "v", take, kTicksPerQuarter);  // starts at 0.5 s
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    const auto plain = renderProject(p, media, host, kSr, 0.2);
    auto fx = makeBuiltin(kBuiltinTune, "Ynot Tune");
    fx.params["mix"] = 0.0;  // pure delay inside the plugin: compensation must cancel it
    p.tracks[0].inserts.push_back(fx);
    const auto tuned = renderProject(p, media, host, kSr, 0.2);
    CHECK_EQ(tuned.frames(), plain.frames());
    double maxErr = 0;
    for (size_t i = 0; i < plain.samples.size(); ++i)
        maxErr = std::max(maxErr, static_cast<double>(std::abs(tuned.samples[i] - plain.samples[i])));
    CHECK(maxErr < 1e-5);
}
