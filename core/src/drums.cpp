#include "ynot/drums.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "ynot/demo.hpp"
#include "ynot/edit.hpp"

namespace ynot {

namespace {
constexpr double kPi = 3.14159265358979323846;

enum Sound { Kick, Snare, Clap, ClosedHat, OpenHat, LowTom, HighTom, Rim, Cowbell, Crash, kNumSounds };

const std::vector<DrumSound>& kit() {
    static const std::vector<DrumSound> k = {
        {36, "Kick"},     {38, "Snare"},    {39, "Clap"},   {42, "Closed hat"}, {46, "Open hat"},
        {45, "Low tom"},  {50, "High tom"}, {37, "Rim"},    {56, "Cowbell"},    {49, "Crash"},
    };
    return k;
}

int soundForNote(int note) {
    switch (note) {
        case 35: case 36: return Kick;
        case 38: case 40: return Snare;
        case 39: return Clap;
        case 42: case 44: return ClosedHat;
        case 46: return OpenHat;
        case 41: case 43: case 45: case 47: return LowTom;
        case 48: case 50: return HighTom;
        case 37: return Rim;
        case 56: return Cowbell;
        case 49: case 51: case 52: case 55: case 57: return Crash;
        default: return -1;
    }
}

// RBJ biquad (direct form I).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void set(bool highpass, bool bandpass, double f, double q, double sr) {
        const double w = 2 * kPi * f / sr, c = std::cos(w), s = std::sin(w), alpha = s / (2 * q);
        const double a0 = 1 + alpha;
        if (bandpass) {
            b0 = alpha / a0; b1 = 0; b2 = -alpha / a0;
        } else if (highpass) {
            b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = (1 + c) / 2 / a0;
        } else {
            b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = (1 - c) / 2 / a0;
        }
        a1 = -2 * c / a0;
        a2 = (1 - alpha) / a0;
    }
    double run(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    void clear() { x1 = x2 = y1 = y2 = 0; }
};

// Synthesized (analog-style) drum kit: one voice per sound; a new hit restarts it, and a
// closed hat chokes the open hat, as on a real kit. Deterministic noise, so renders match
// on every platform.
class DrumKit : public PluginProcessor {
public:
    DrumKit(const Json& params, double sr) : sr_(sr) {
        level_ = params.getNumber("level", 0.8);
        for (auto& v : voices_) setupFilters(v);
    }

    void process(float* l, float* r, int frames, const MidiEvent* ev, int nev) override {
        int e = 0;
        for (int i = 0; i < frames; ++i) {
            while (e < nev && ev[e].frameOffset <= i) handle(ev[e++]);
            double s = 0;
            for (int k = 0; k < kNumSounds; ++k) {
                auto& v = voices_[static_cast<size_t>(k)];
                if (!v.active) continue;
                s += v.vel * sample(k, v);
                v.t += 1.0 / sr_;
                if (v.t > kLength[k]) v.active = false;
            }
            const auto out = static_cast<float>(std::tanh(s * level_));  // gentle safety clip
            l[i] += out;
            r[i] += out;
        }
        while (e < nev) handle(ev[e++]);
    }
    void setParameter(const std::string& id, double v) override {
        if (id == "level") level_ = v;
    }
    void reset() override {
        for (auto& v : voices_) {
            v.active = false;
            v.hp.clear();
            v.bp.clear();
        }
    }

private:
    static constexpr double kLength[kNumSounds] = {0.9, 0.4, 0.45, 0.2, 0.9, 0.7, 0.6, 0.08, 0.5, 2.2};
    struct Voice {
        bool active = false;
        double t = 0, vel = 0, phase = 0;
        std::array<double, 6> metal{};
        Biquad hp, bp;
    };

    double noise() {  // xorshift32 -> [-1, 1)
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<double>(rng_) / 2147483648.0 - 1.0;
    }
    double metallic(Voice& v) {  // six detuned square waves: the classic analog cymbal source
        static constexpr double f[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
        double s = 0;
        for (size_t k = 0; k < 6; ++k) {
            v.metal[k] += f[k] * 1.7 / sr_;
            if (v.metal[k] >= 1) v.metal[k] -= 1;
            s += v.metal[k] < 0.5 ? 1.0 : -1.0;
        }
        return s / 6.0;
    }
    void setupFilters(Voice& v) {
        const auto k = static_cast<int>(&v - voices_.data());
        switch (k) {
            case Snare: v.hp.set(true, false, 1800, 0.7, sr_); break;
            case Clap: v.bp.set(false, true, 1200, 1.4, sr_); break;
            case ClosedHat: case OpenHat: v.hp.set(true, false, 7000, 0.8, sr_); break;
            case Rim: v.bp.set(false, true, 1700, 3, sr_); break;
            case Cowbell: v.bp.set(false, true, 800, 2.5, sr_); break;
            case Crash: v.hp.set(true, false, 5000, 0.7, sr_); break;
            default: break;
        }
    }
    double sine(Voice& v, double freq) {
        v.phase += freq / sr_;
        if (v.phase >= 1) v.phase -= 1;
        return std::sin(2 * kPi * v.phase);
    }
    double sample(int k, Voice& v) {
        const double t = v.t;
        switch (k) {
            case Kick: {
                const double body = sine(v, 45 + 110 * std::exp(-t / 0.035)) * std::exp(-t / 0.3);
                const double click = t < 0.003 ? noise() * (1 - t / 0.003) * 0.4 : 0.0;
                return 1.1 * body + click;
            }
            case Snare:
                return 0.5 * sine(v, 185) * std::exp(-t / 0.07) + 0.7 * v.hp.run(noise()) * std::exp(-t / 0.14);
            case Clap: {
                double env = 0;
                for (double start : {0.0, 0.011, 0.022})
                    if (t >= start) env += std::exp(-(t - start) / 0.0055);
                if (t >= 0.03) env += 0.8 * std::exp(-(t - 0.03) / 0.16);
                return 1.4 * v.bp.run(noise()) * std::min(env, 1.2);
            }
            case ClosedHat: return 0.55 * v.hp.run(0.6 * metallic(v) + 0.4 * noise()) * std::exp(-t / 0.035);
            case OpenHat: return 0.5 * v.hp.run(0.6 * metallic(v) + 0.4 * noise()) * std::exp(-t / 0.3);
            case LowTom: return 0.9 * sine(v, 95 + 50 * std::exp(-t / 0.06)) * std::exp(-t / 0.25);
            case HighTom: return 0.85 * sine(v, 170 + 60 * std::exp(-t / 0.05)) * std::exp(-t / 0.2);
            case Rim: return 0.8 * (sine(v, 1700) * 0.6 + v.bp.run(noise())) * std::exp(-t / 0.012);
            case Cowbell: {
                v.metal[0] += 540.0 / sr_;
                v.metal[1] += 800.0 / sr_;
                for (size_t m = 0; m < 2; ++m)
                    if (v.metal[m] >= 1) v.metal[m] -= 1;
                const double sq = (v.metal[0] < 0.5 ? 1 : -1) + (v.metal[1] < 0.5 ? 1 : -1);
                return 0.6 * v.bp.run(sq) * (0.6 * std::exp(-t / 0.02) + 0.4 * std::exp(-t / 0.2));
            }
            case Crash: return 0.45 * v.hp.run(0.5 * metallic(v) + 0.5 * noise()) * std::exp(-t / 0.9);
            default: return 0;
        }
    }
    void handle(const MidiEvent& e) {
        if (!e.noteOn || e.velocity <= 0) return;  // drums are one-shots: note-offs don't cut them
        const int k = soundForNote(e.pitch);
        if (k < 0) return;
        auto& v = voices_[static_cast<size_t>(k)];
        v.active = true;
        v.t = 0;
        v.phase = 0;
        v.metal.fill(0);
        v.hp.clear();
        v.bp.clear();
        const double vel = std::clamp(e.velocity, 1, 127) / 127.0;
        v.vel = vel * vel;  // velocity curve: soft hits really are softer
        if (k == ClosedHat) voices_[OpenHat].active = false;  // choke
    }

    double sr_, level_ = 0.8;
    uint32_t rng_ = 0x1234567u;
    std::array<Voice, kNumSounds> voices_{};
};

int64_t straightTick(const StepPattern& s, int step) { return static_cast<int64_t>(step) * s.stepTicks; }

// Index of the step a note sits on, or -1 if it isn't on the grid.
int stepOf(const StepPattern& s, int64_t tick) {
    const auto guess = static_cast<int>(std::llround(static_cast<double>(tick) / static_cast<double>(s.stepTicks)));
    for (int k = std::max(0, guess - 1); k <= guess + 1; ++k)
        if (std::llabs(stepTick(s, k) - tick) <= s.stepTicks / 4) return k;
    return -1;
}

MidiClip* findPattern(Project& p, const std::string& trackId, const std::string& clipId) {
    Track* t = p.findTrack(trackId);
    if (!t) return nullptr;
    for (auto& c : t->midiClips)
        if (c.id == clipId) {
            if (!c.steps) c.steps = StepPattern{};  // any MIDI clip can be opened as a pattern
            return &c;
        }
    return nullptr;
}

void sortNotes(MidiClip& c) {
    std::stable_sort(c.notes.begin(), c.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
    });
}
}  // namespace

const std::vector<DrumSound>& drumKitSounds() { return kit(); }

const char* drumName(int note) {
    for (const auto& d : kit())
        if (d.note == note) return d.name;
    return nullptr;
}

std::unique_ptr<PluginProcessor> makeDrumKit(const Json& params, double sampleRate) {
    return std::make_unique<DrumKit>(params, sampleRate);
}

int64_t stepTick(const StepPattern& s, int step) {
    int64_t t = straightTick(s, step);
    if (step % 2 == 1 && s.swingPercent > 50.0)
        t += static_cast<int64_t>(std::llround(2.0 * static_cast<double>(s.stepTicks) * (s.swingPercent - 50.0) / 100.0));
    return t;
}

int stepCount(const StepPattern& s, int64_t clipLengthTicks) {
    return static_cast<int>(std::max<int64_t>(1, clipLengthTicks / std::max<int64_t>(1, s.stepTicks)));
}

std::string addPatternClip(Project& p, const std::string& trackId, int64_t startTick, int bars) {
    Track* t = p.findTrack(trackId);
    if (!t || t->kind != TrackKind::Instrument) return {};
    MidiClip c;
    c.id = newId();
    c.startTick = std::max<int64_t>(0, startTick);
    c.lengthTick = std::max(1, bars) * p.barTicks();
    StepPattern s;
    for (const auto& d : kit()) s.rows.push_back(d.note);
    c.steps = s;
    const std::string id = c.id;
    t->midiClips.push_back(std::move(c));
    std::sort(t->midiClips.begin(), t->midiClips.end(),
              [](const MidiClip& a, const MidiClip& b) { return a.startTick < b.startTick; });
    return id;
}

std::string addDrumTrack(Project& p, int64_t startTick, int bars) {
    const std::string id = addTrack(p, TrackKind::Instrument, "Drums");
    Track* t = p.findTrack(id);
    auto drums = makeBuiltin(kBuiltinDrums, "Ynot Drums");
    drums.id = newId();
    t->instrument = drums;
    addPatternClip(p, id, startTick, bars);
    return id;
}

int stepVelocity(const MidiClip& c, int note, int step) {
    const StepPattern s = c.steps.value_or(StepPattern{});
    for (const auto& n : c.notes)
        if (n.pitch == note && stepOf(s, n.tick) == step) return n.velocity;
    return 0;
}

bool setStep(Project& p, const std::string& trackId, const std::string& clipId, int note, int step, int velocity) {
    MidiClip* c = findPattern(p, trackId, clipId);
    if (!c || step < 0 || step >= stepCount(*c->steps, c->lengthTick)) return false;
    auto& s = *c->steps;
    c->notes.erase(std::remove_if(c->notes.begin(), c->notes.end(),
                                  [&](const MidiNote& n) { return n.pitch == note && stepOf(s, n.tick) == step; }),
                   c->notes.end());
    if (velocity > 0) {
        c->notes.push_back({stepTick(s, step), std::max<int64_t>(1, s.stepTicks / 2), note, std::clamp(velocity, 1, 127)});
        sortNotes(*c);
    }
    if (std::find(s.rows.begin(), s.rows.end(), note) == s.rows.end()) s.rows.push_back(note);
    return true;
}

bool setPatternSwing(Project& p, const std::string& trackId, const std::string& clipId, double swingPercent) {
    MidiClip* c = findPattern(p, trackId, clipId);
    if (!c) return false;
    const StepPattern before = *c->steps;
    c->steps->swingPercent = std::clamp(swingPercent, 50.0, 75.0);
    for (auto& n : c->notes)
        if (const int k = stepOf(before, n.tick); k >= 0) n.tick = stepTick(*c->steps, k);
    sortNotes(*c);
    return true;
}

bool setPatternStepTicks(Project& p, const std::string& trackId, const std::string& clipId, int64_t stepTicks) {
    MidiClip* c = findPattern(p, trackId, clipId);
    if (!c || stepTicks <= 0) return false;
    const StepPattern before = *c->steps;
    c->steps->stepTicks = stepTicks;
    // Notes keep their place in time, snapped to the new grid (the beat sounds the same).
    std::vector<MidiNote> moved;
    for (auto n : c->notes) {
        const int k = stepOf(before, n.tick);
        const int64_t straight = k >= 0 ? straightTick(before, k) : n.tick;
        const auto k2 = static_cast<int>(std::llround(static_cast<double>(straight) / static_cast<double>(stepTicks)));
        n.tick = stepTick(*c->steps, k2);
        n.lengthTick = std::max<int64_t>(1, stepTicks / 2);
        if (n.tick >= c->lengthTick) continue;
        const bool dup = std::any_of(moved.begin(), moved.end(),
                                     [&](const MidiNote& m) { return m.pitch == n.pitch && m.tick == n.tick; });
        if (!dup) moved.push_back(n);
    }
    c->notes = std::move(moved);
    sortNotes(*c);
    return true;
}

bool setPatternBars(Project& p, const std::string& trackId, const std::string& clipId, int bars) {
    MidiClip* c = findPattern(p, trackId, clipId);
    if (!c || bars < 1) return false;
    const int64_t oldLen = c->lengthTick, newLen = static_cast<int64_t>(bars) * p.barTicks();
    std::vector<MidiNote> notes;
    for (const auto& n : c->notes)
        if (n.tick >= 0 && n.tick < oldLen) notes.push_back(n);
    // Growing repeats what's there; shrinking keeps the start.
    std::vector<MidiNote> out;
    for (int64_t offset = 0; offset < newLen; offset += std::max<int64_t>(1, oldLen))
        for (auto n : notes) {
            n.tick += offset;
            if (n.tick < newLen) out.push_back(n);
        }
    c->notes = std::move(out);
    c->lengthTick = newLen;
    sortNotes(*c);
    return true;
}

bool clearPatternRow(Project& p, const std::string& trackId, const std::string& clipId, int note) {
    MidiClip* c = findPattern(p, trackId, clipId);
    if (!c) return false;
    c->notes.erase(std::remove_if(c->notes.begin(), c->notes.end(), [&](const MidiNote& n) { return n.pitch == note; }),
                   c->notes.end());
    return true;
}

}  // namespace ynot
