#include "ynot/engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "ynot/drums.hpp"
#include "ynot/pitch_correct.hpp"
#include "ynot/project_format.hpp"

namespace ynot {

namespace {
constexpr int kMaxLiveMidiPerBlock = 512;
}

double dbToGain(double db) { return db <= -144.0 ? 0.0 : std::pow(10.0, db / 20.0); }

namespace {
constexpr double kPi = 3.14159265358979323846;

double paramOr(const Json& params, const char* key, double def) { return params.getNumber(key, def); }

// --------------------------------------------------------------- builtin synth

class BasicSynth : public PluginProcessor {
public:
    BasicSynth(const Json& params, double sr) : sr_(sr) {
        wave_ = paramOr(params, "wave", 1.0);
        attack_ = paramOr(params, "attack", 0.005);
        release_ = paramOr(params, "release", 0.15);
        level_ = paramOr(params, "level", 0.25);
    }
    void process(float* l, float* r, int frames, const MidiEvent* ev, int nev) override {
        int e = 0;
        for (int i = 0; i < frames; ++i) {
            while (e < nev && ev[e].frameOffset <= i) handle(ev[e++]);
            double s = 0;
            for (auto& v : voices_) {
                if (!v.active) continue;
                double a = v.on ? std::min(1.0, v.env + 1.0 / std::max(1.0, attack_ * sr_))
                                : v.env - 1.0 / std::max(1.0, release_ * sr_);
                v.env = a;
                if (v.env <= 0) { v.active = false; continue; }
                double osc = wave_ < 0.5 ? std::sin(2 * kPi * v.phase) : 2.0 * v.phase - 1.0;
                v.phase += v.inc;
                if (v.phase >= 1.0) v.phase -= 1.0;
                s += osc * v.env * v.vel;
            }
            float out = static_cast<float>(s * level_);
            l[i] += out;
            r[i] += out;
        }
        while (e < nev) handle(ev[e++]);
    }
    void setParameter(const std::string& id, double v) override {
        if (id == "level") level_ = v;
        else if (id == "wave") wave_ = v;
    }
    void reset() override {
        for (auto& v : voices_) v = Voice{};
    }

private:
    struct Voice {
        bool active = false, on = false;
        int pitch = 0;
        double phase = 0, inc = 0, env = 0, vel = 0;
    };
    void handle(const MidiEvent& e) {
        if (e.noteOn && e.velocity > 0) {
            Voice* slot = nullptr;
            for (auto& v : voices_)
                if (!v.active) { slot = &v; break; }
            if (!slot) slot = &voices_[0];  // simple voice stealing
            *slot = Voice{true, true, e.pitch, 0.0,
                          440.0 * std::pow(2.0, (e.pitch - 69) / 12.0) / sr_, 0.0, e.velocity / 127.0};
        } else {
            for (auto& v : voices_)
                if (v.active && v.on && v.pitch == e.pitch) v.on = false;
        }
    }
    double sr_, wave_, attack_, release_, level_;
    std::array<Voice, 16> voices_{};
};

class GainFx : public PluginProcessor {
public:
    explicit GainFx(const Json& params) { setDb(paramOr(params, "gainDb", 0.0)); }
    void process(float* l, float* r, int frames, const MidiEvent*, int) override {
        for (int i = 0; i < frames; ++i) { l[i] *= g_; r[i] *= g_; }
    }
    // normalized 0..1 maps to -60..+12 dB
    void setParameter(const std::string& id, double v) override {
        if (id == "gainDb") setDb(-60.0 + v * 72.0);
    }
    void reset() override {}

private:
    void setDb(double db) { g_ = static_cast<float>(dbToGain(db)); }
    float g_ = 1.0f;
};

class LowpassFx : public PluginProcessor {
public:
    LowpassFx(const Json& params, double sr) : sr_(sr) { setCutoff(paramOr(params, "cutoffHz", 8000)); }
    void process(float* l, float* r, int frames, const MidiEvent*, int) override {
        for (int i = 0; i < frames; ++i) {
            zl_ += a_ * (l[i] - zl_);
            zr_ += a_ * (r[i] - zr_);
            l[i] = zl_;
            r[i] = zr_;
        }
    }
    // normalized 0..1 maps logarithmically to 20 Hz..20 kHz
    void setParameter(const std::string& id, double v) override {
        if (id == "cutoffHz") setCutoff(20.0 * std::pow(1000.0, std::clamp(v, 0.0, 1.0)));
    }
    void reset() override { zl_ = zr_ = 0; }

private:
    void setCutoff(double hz) {
        hz = std::clamp(hz, 20.0, sr_ * 0.45);
        a_ = static_cast<float>(1.0 - std::exp(-2.0 * kPi * hz / sr_));
    }
    double sr_;
    float a_ = 1, zl_ = 0, zr_ = 0;
};

}  // namespace

std::vector<std::string> BuiltinPluginHost::builtinIds() {
    return {kBuiltinSynth, kBuiltinGain, kBuiltinFilter, kBuiltinDrums, kBuiltinTune};
}

bool BuiltinPluginHost::isAvailable(const PluginInstance& p) const {
    if (p.format != PluginFormat::Builtin) return false;
    auto ids = builtinIds();
    return std::find(ids.begin(), ids.end(), p.uid) != ids.end();
}

std::unique_ptr<PluginProcessor> BuiltinPluginHost::load(const PluginInstance& p, double sr, int) {
    if (!isAvailable(p)) return nullptr;
    if (p.uid == kBuiltinSynth) return std::make_unique<BasicSynth>(p.params, sr);
    if (p.uid == kBuiltinDrums) return makeDrumKit(p.params, sr);
    if (p.uid == kBuiltinTune) return makePitchCorrector(p.params, sr);
    if (p.uid == kBuiltinGain) return std::make_unique<GainFx>(p.params);
    if (p.uid == kBuiltinFilter) return std::make_unique<LowpassFx>(p.params, sr);
    return nullptr;
}

const AudioBuffer* PackageMediaProvider::get(const MediaRef& m) {
    auto it = cache_.find(m.id);
    if (it != cache_.end()) return &it->second;
    try {
        auto [ins, _] = cache_.emplace(m.id, loadMedia(m, dir_));
        return &ins->second;
    } catch (...) {
        return nullptr;  // missing media (e.g. not yet downloaded) is reported by the engine
    }
}

// --------------------------------------------------------------- engine

struct Engine::TrackState {
    std::string id;
    // Live mix: written by the mixer (any thread), read by the audio thread.
    std::atomic<bool> muted{false};
    std::atomic<double> gain{1.0}, pan{0.0};
    std::atomic<float> peak[2] = {{0.f}, {0.f}};
    const AutomationLane* gainLane = nullptr;  // points into `track` copy below
    const AutomationLane* panLane = nullptr;
    Track track;  // snapshot

    // Source: either frozen audio, audio clips, or instrument + MIDI.
    const AudioBuffer* frozenAudio = nullptr;
    struct ClipSrc {
        int64_t startFrame, endFrame;
        double srcOffset, ratio, gain;
        const AudioBuffer* buf;
    };
    std::vector<ClipSrc> clips;
    std::unique_ptr<PluginProcessor> instrument;
    struct TimedEvent { int64_t frame; MidiEvent ev; };
    std::vector<TimedEvent> events;  // sorted
    size_t eventCursor = 0;
    std::vector<MidiEvent> blockEvents;
    std::vector<std::unique_ptr<PluginProcessor>> inserts;
    struct PluginAuto { PluginProcessor* proc; std::string param; const AutomationLane* lane; };
    std::vector<PluginAuto> pluginAutomation;
    std::vector<int> monitor;  // live input channels routed into this track
    bool liveMidi = false;     // plays live MIDI input
    int64_t latency = 0;       // plugin delay on this track: sources are read this far ahead
};

Engine::Engine() = default;
Engine::~Engine() = default;

void Engine::prepare(const Project& project, MediaProvider& media, PluginHost& host,
                     const EngineOptions& opts) {
    opts_ = opts;
    tracks_.clear();
    issues_.clear();
    tempo_ = TempoMap(project);
    masterGain_.store(opts.isolateTrackPreFader.empty() ? static_cast<float>(dbToGain(project.masterGainDb)) : 1.f);
    beatsPerBar_ = std::max(1, project.timeSigNumerator);
    beatTicks_ = project.beatTicks();
    clickRemaining_ = 0;
    scratchL_.assign(static_cast<size_t>(opts.maxBlock), 0.f);
    scratchR_.assign(static_cast<size_t>(opts.maxBlock), 0.f);
    liveScratch_.assign(static_cast<size_t>(kMaxLiveMidiPerBlock), MidiEvent{});
    const double sr = opts.sampleRate;
    auto tickToFrame = [&](int64_t tick) { return tempo_.tickToFrame(tick, sr); };

    bool isolate = !opts.isolateTrackPreFader.empty();
    bool anySolo = false;
    for (auto& t : project.tracks) anySolo |= t.solo;

    for (const Track* tp : project.orderedTracks()) {
        if (isolate && tp->id != opts.isolateTrackPreFader) continue;
        auto ts = std::make_unique<TrackState>();
        ts->track = *tp;
        const Track& t = ts->track;
        ts->id = t.id;
        if (!isolate) {
            auto mon = opts.monitorInputs.find(t.id);
            if (mon != opts.monitorInputs.end()) ts->monitor = mon->second;
            ts->liveMidi = opts.liveMidiTracks.count(t.id) > 0;
        }
        ts->muted = !isolate && (t.mute || (anySolo && !t.solo));
        ts->gain = isolate ? 1.0 : dbToGain(t.gainDb);
        ts->pan = isolate ? 0.0 : t.pan;
        if (!isolate) {
            ts->gainLane = t.lane("track.gainDb");
            ts->panLane = t.lane("track.pan");
        }

        // Prefer a valid freeze when the user froze the track.
        bool useFreeze = false;
        if (!isolate && t.frozen && t.freeze) {
            if (auto* m = project.findMedia(t.freeze->mediaId)) {
                ts->frozenAudio = media.get(*m);
                useFreeze = ts->frozenAudio != nullptr;
            }
            if (!useFreeze)
                issues_.push_back({t.id, "Frozen audio for '" + t.name +
                                             "' is missing on this device; trying live processing."});
            else if (!hasFreshFreeze(project, t))
                issues_.push_back({t.id, "Freeze of '" + t.name +
                                             "' is older than the latest edits; refreeze to update."});
        }
        if (useFreeze) {
            AudioBuffer const* b = ts->frozenAudio;
            ts->clips.push_back({0, std::numeric_limits<int64_t>::max(), 0.0, b->sampleRate / sr, 1.0, b});
            tracks_.push_back(std::move(ts));
            continue;
        }

        for (auto& c : t.audioClips) {
            auto* m = project.findMedia(c.mediaId);
            const AudioBuffer* b = m ? media.get(*m) : nullptr;
            if (!b) {
                issues_.push_back({t.id, "Recording for a clip on '" + t.name +
                                             "' is not available on this device yet (still syncing?)."});
                continue;
            }
            // Clip edges are rounded to whole output samples, but the source position is
            // computed from the exact (fractional) start time, so splitting a clip never
            // shifts the audio after the split point.
            const int64_t startFrame = tickToFrame(c.startTick);
            const double exactStart = tempo_.tickToSeconds(static_cast<double>(c.startTick)) * sr;
            const double ratio = b->sampleRate / sr;
            const double srcOffset =
                static_cast<double>(c.sourceOffsetFrames) + (static_cast<double>(startFrame) - exactStart) * ratio;
            ts->clips.push_back({startFrame, tickToFrame(c.startTick + c.lengthTick), srcOffset, ratio,
                                 dbToGain(c.gainDb), b});
        }

        if (t.instrument && !t.instrument->bypassed) {
            ts->instrument = host.load(*t.instrument, sr, opts.maxBlock);
            if (!ts->instrument)
                issues_.push_back({t.id, "Instrument '" + t.instrument->name + "' (" +
                                             toString(t.instrument->format) +
                                             ") is unavailable on this device; track is silent. "
                                             "Freeze it on a device that has the plugin."});
        }
        for (auto& c : t.midiClips) {
            int64_t clipEnd = c.startTick + c.lengthTick;
            for (auto& n : c.notes) {
                if (n.tick < 0) continue;  // hidden by a start trim (kept, not deleted)
                int64_t on = c.startTick + n.tick;
                if (on >= clipEnd) continue;
                int64_t off = std::min(on + n.lengthTick, clipEnd);
                ts->events.push_back({tickToFrame(on), {0, true, n.pitch, n.velocity}});
                ts->events.push_back({tickToFrame(off), {0, false, n.pitch, 0}});
            }
        }
        std::stable_sort(ts->events.begin(), ts->events.end(),
                         [](const TrackState::TimedEvent& a, const TrackState::TimedEvent& b) {
                             if (a.frame != b.frame) return a.frame < b.frame;
                             return !a.ev.noteOn && b.ev.noteOn;  // note-offs first
                         });
        ts->blockEvents.reserve(ts->events.size() + kMaxLiveMidiPerBlock);

        for (auto& p : t.inserts) {
            if (p.bypassed) continue;
            auto proc = host.load(p, sr, opts.maxBlock);
            if (!proc) {
                issues_.push_back({t.id, "Effect '" + p.name + "' (" + toString(p.format) +
                                             ") is unavailable on this device and is bypassed."});
                continue;
            }
            for (auto& lane : t.automation) {
                std::string prefix = "plugin:" + p.id + ":";
                if (lane.target.rfind(prefix, 0) == 0)
                    ts->pluginAutomation.push_back({proc.get(), lane.target.substr(prefix.size()), &lane});
            }
            ts->inserts.push_back(std::move(proc));
        }
        ts->latency = ts->instrument ? ts->instrument->latencySamples() : 0;
        for (auto& ins : ts->inserts) ts->latency += ins->latencySamples();
        if (ts->instrument)
            for (auto& lane : t.automation) {
                std::string prefix = "plugin:" + t.instrument->id + ":";
                if (lane.target.rfind(prefix, 0) == 0)
                    ts->pluginAutomation.push_back(
                        {ts->instrument.get(), lane.target.substr(prefix.size()), &lane});
            }
        tracks_.push_back(std::move(ts));
    }
    seek(0);
}

void Engine::seek(int64_t frame) {
    pos_ = frame;
    for (auto& ts : tracks_) {
        auto it = std::lower_bound(ts->events.begin(), ts->events.end(), frame + ts->latency,
                                   [](const TrackState::TimedEvent& e, int64_t f) { return e.frame < f; });
        ts->eventCursor = static_cast<size_t>(it - ts->events.begin());
        if (ts->instrument) ts->instrument->reset();
        for (auto& p : ts->inserts) p->reset();
    }
}

const std::string& Engine::trackId(int index) const { return tracks_[static_cast<size_t>(index)]->id; }

int Engine::trackIndex(const std::string& id) const {
    for (size_t i = 0; i < tracks_.size(); ++i)
        if (tracks_[i]->id == id) return static_cast<int>(i);
    return -1;
}

void Engine::setTrackMix(int index, double gainDb, double pan, bool muted) {
    if (index < 0 || index >= numTracks()) return;
    auto& ts = *tracks_[static_cast<size_t>(index)];
    ts.gain.store(dbToGain(gainDb));
    ts.pan.store(std::clamp(pan, -1.0, 1.0));
    ts.muted.store(muted);
}

float Engine::takePeak(int index, int channel) {
    channel = std::clamp(channel, 0, 1);
    if (index < 0) return masterPeak_[channel].exchange(0.f);
    if (index >= numTracks()) return 0.f;
    return tracks_[static_cast<size_t>(index)]->peak[channel].exchange(0.f);
}

void Engine::setLoop(int64_t startTick, int64_t endTick, bool enabled) {
    const double sr = opts_.sampleRate;
    const int64_t s = tempo_.tickToFrame(std::max<int64_t>(0, startTick), sr);
    const int64_t e = tempo_.tickToFrame(endTick, sr);
    loopStart_.store(s);
    loopEnd_.store(e);
    loopOn_.store(enabled && e > s);
}

void Engine::jumpTo(int64_t frame) {
    pos_ = frame;
    for (auto& ts : tracks_) {
        auto it = std::lower_bound(ts->events.begin(), ts->events.end(), frame + ts->latency,
                                   [](const TrackState::TimedEvent& e, int64_t f) { return e.frame < f; });
        ts->eventCursor = static_cast<size_t>(it - ts->events.begin());
        if (ts->instrument) ts->instrument->reset();  // release notes still sounding at the loop end
    }
}

void Engine::process(float* left, float* right, int frames, const float* const* inputs, int numInputs,
                     bool transportRunning, const MidiEvent* liveMidi, int numLiveMidi) {
    frames = std::min(frames, opts_.maxBlock);
    const bool loop = transportRunning && loopOn_.load(std::memory_order_relaxed);
    const int64_t ls = loopStart_.load(std::memory_order_relaxed), le = loopEnd_.load(std::memory_order_relaxed);
    if (!loop || le <= ls || pos_ >= le || pos_ + frames < le) {
        processSegment(left, right, frames, inputs, numInputs, transportRunning, liveMidi, numLiveMidi);
        return;
    }
    // The block reaches the loop end: render up to it, jump back, render the rest (repeating
    // for loops shorter than a block).
    constexpr int kMaxInputs = 64;
    const float* ins[kMaxInputs];
    const int numIns = std::min(numInputs, kMaxInputs);
    for (int done = 0; done < frames;) {
        int n = frames - done;
        const bool wrap = pos_ < le && pos_ + n >= le;
        if (wrap) n = static_cast<int>(le - pos_);
        for (int c = 0; c < numIns; ++c) ins[c] = inputs && inputs[c] ? inputs[c] + done : nullptr;
        int numLive = 0;
        for (int k = 0; k < numLiveMidi && numLive < static_cast<int>(liveScratch_.size()); ++k)
            if (liveMidi[k].frameOffset >= done && liveMidi[k].frameOffset < done + n) {
                liveScratch_[static_cast<size_t>(numLive)] = liveMidi[k];
                liveScratch_[static_cast<size_t>(numLive++)].frameOffset -= done;
            }
        processSegment(left + done, right + done, n, inputs ? ins : nullptr, numIns, transportRunning,
                       liveScratch_.data(), numLive);
        done += n;
        if (wrap) jumpTo(ls);
    }
}

void Engine::processSegment(float* left, float* right, int frames, const float* const* inputs, int numInputs,
                            bool transportRunning, const MidiEvent* liveMidi, int numLiveMidi) {
    std::fill(left, left + frames, 0.f);
    std::fill(right, right + frames, 0.f);
    const int64_t blockStart = pos_, blockEnd = pos_ + frames;
    const double sr = opts_.sampleRate;
    const auto blockTick = static_cast<int64_t>(tempo_.secondsToTick(static_cast<double>(blockStart) / sr));

    for (auto& tsp : tracks_) {
        TrackState& ts = *tsp;
        float* l = scratchL_.data();
        float* r = scratchR_.data();
        std::fill(l, l + frames, 0.f);
        std::fill(r, r + frames, 0.f);

        // Audio sources (clips or frozen render), linear-interpolated resampling. With plugin
        // latency on the track, sources are read that far ahead so the output lands on time.
        const int64_t srcStart = blockStart + ts.latency, srcEnd = blockEnd + ts.latency;
        for (auto& c : ts.clips) {
            if (!transportRunning) break;
            if (c.endFrame <= srcStart || c.startFrame >= srcEnd) continue;
            const AudioBuffer& b = *c.buf;
            const int64_t n = b.frames();
            int from = static_cast<int>(std::max<int64_t>(0, c.startFrame - srcStart));
            int to = static_cast<int>(std::min<int64_t>(frames, c.endFrame - srcStart));
            for (int i = from; i < to; ++i) {
                double src = c.srcOffset + static_cast<double>(srcStart + i - c.startFrame) * c.ratio;
                if (src < 0) continue;
                int64_t i0 = static_cast<int64_t>(src);
                if (i0 + 1 >= n) break;
                float f = static_cast<float>(src - static_cast<double>(i0));
                auto sample = [&](int64_t idx, int ch) {
                    return b.samples[static_cast<size_t>(idx * b.channels + std::min(ch, b.channels - 1))];
                };
                float sl = sample(i0, 0) + (sample(i0 + 1, 0) - sample(i0, 0)) * f;
                float sr2 = sample(i0, 1) + (sample(i0 + 1, 1) - sample(i0, 1)) * f;
                l[i] += sl * static_cast<float>(c.gain);
                r[i] += sr2 * static_cast<float>(c.gain);
            }
        }

        // Live input monitoring: the input runs through the track's inserts, fader and pan,
        // so the performer hears what will be recorded (works with the transport stopped).
        if (!ts.monitor.empty() && inputs) {
            for (size_t k = 0; k < ts.monitor.size() && k < 2; ++k) {
                const int in = ts.monitor[k];
                if (in < 0 || in >= numInputs || !inputs[in]) continue;
                const bool mono = ts.monitor.size() == 1;
                for (int i = 0; i < frames; ++i) {
                    if (mono || k == 0) l[i] += inputs[in][i];
                    if (mono || k == 1) r[i] += inputs[in][i];
                }
            }
        }

        // Block-rate plugin automation.
        for (auto& pa : ts.pluginAutomation) pa.proc->setParameter(pa.param, pa.lane->valueAt(blockTick, 0.0));

        if (ts.instrument) {
            ts.blockEvents.clear();
            while (transportRunning && ts.eventCursor < ts.events.size() &&
                   ts.events[ts.eventCursor].frame < blockEnd + ts.latency) {
                auto e = ts.events[ts.eventCursor++];
                e.ev.frameOffset = static_cast<int>(std::max<int64_t>(0, e.frame - ts.latency - blockStart));
                ts.blockEvents.push_back(e.ev);
            }
            if (ts.liveMidi && liveMidi) {
                // Merge live events in offset order (insertion sort: no allocation).
                for (int k = 0; k < numLiveMidi && k < kMaxLiveMidiPerBlock; ++k) {
                    MidiEvent ev = liveMidi[k];
                    ev.frameOffset = std::clamp(ev.frameOffset, 0, frames - 1);
                    ts.blockEvents.push_back(ev);
                    for (size_t j = ts.blockEvents.size() - 1;
                         j > 0 && ts.blockEvents[j - 1].frameOffset > ts.blockEvents[j].frameOffset; --j)
                        std::swap(ts.blockEvents[j - 1], ts.blockEvents[j]);
                }
            }
            ts.instrument->process(l, r, frames, ts.blockEvents.data(),
                                   static_cast<int>(ts.blockEvents.size()));
        }
        for (auto& p : ts.inserts) p->process(l, r, frames, nullptr, 0);

        if (ts.muted.load(std::memory_order_relaxed)) continue;
        const double faderGain = ts.gain.load(std::memory_order_relaxed);
        const double faderPan = ts.pan.load(std::memory_order_relaxed);
        double gainDb0 = ts.gainLane ? ts.gainLane->valueAt(blockTick, ts.track.gainDb) : 0;
        double gain = ts.gainLane ? dbToGain(gainDb0) : faderGain;
        double pan = ts.panLane ? ts.panLane->valueAt(blockTick, faderPan) : faderPan;
        pan = std::clamp(pan, -1.0, 1.0);
        // Constant-power pan law (-3 dB centre).
        double angle = (pan + 1.0) * kPi / 4.0;
        auto gl = static_cast<float>(gain * std::cos(angle) * std::sqrt(2.0));
        auto gr = static_cast<float>(gain * std::sin(angle) * std::sqrt(2.0));
        if (!opts_.isolateTrackPreFader.empty()) gl = gr = 1.0f;
        float pl = 0, pr = 0;
        for (int i = 0; i < frames; ++i) {
            const float a = l[i] * gl, b = r[i] * gr;
            left[i] += a;
            right[i] += b;
            pl = std::max(pl, std::abs(a));
            pr = std::max(pr, std::abs(b));
        }
        if (pl > ts.peak[0].load(std::memory_order_relaxed)) ts.peak[0].store(pl, std::memory_order_relaxed);
        if (pr > ts.peak[1].load(std::memory_order_relaxed)) ts.peak[1].store(pr, std::memory_order_relaxed);
    }
    if (opts_.isolateTrackPreFader.empty()) {
        const float mg = masterGain_.load(std::memory_order_relaxed);
        float pl = 0, pr = 0;
        for (int i = 0; i < frames; ++i) {
            left[i] *= mg;
            right[i] *= mg;
            pl = std::max(pl, std::abs(left[i]));
            pr = std::max(pr, std::abs(right[i]));
        }
        if (pl > masterPeak_[0].load(std::memory_order_relaxed)) masterPeak_[0].store(pl, std::memory_order_relaxed);
        if (pr > masterPeak_[1].load(std::memory_order_relaxed)) masterPeak_[1].store(pr, std::memory_order_relaxed);
    }
    if (transportRunning) {
        const int64_t forcedUntil = clickUntil_.load(std::memory_order_relaxed);
        if (metronome_.load(std::memory_order_relaxed))
            renderClick(left, right, frames, blockStart);
        else if (blockStart < forcedUntil)  // count-in: click even when the metronome is off
            renderClick(left, right, static_cast<int>(std::min<int64_t>(frames, forcedUntil - blockStart)), blockStart);
        pos_ = blockEnd;
    }
}

void Engine::renderClick(float* left, float* right, int frames, int64_t blockStart) {
    const double sr = opts_.sampleRate;
    const int clickLen = static_cast<int>(0.03 * sr);  // 30 ms
    // Beats are time-signature beats in musical time; the tempo map places them in frames.
    const auto bt = static_cast<double>(beatTicks_);
    auto beat = static_cast<int64_t>(std::ceil(tempo_.secondsToTick(static_cast<double>(blockStart) / sr) / bt - 1e-9));
    auto frameOfBeat = [&](int64_t b) {
        return static_cast<int64_t>(std::llround(tempo_.tickToSeconds(static_cast<double>(b) * bt) * sr));
    };
    int64_t nextBeatFrame = frameOfBeat(beat);
    if (nextBeatFrame < blockStart) nextBeatFrame = frameOfBeat(++beat);
    for (int i = 0; i < frames; ++i) {
        if (blockStart + i == nextBeatFrame) {
            const bool downbeat = beat % beatsPerBar_ == 0;
            clickInc_ = (downbeat ? 1500.0 : 1000.0) / sr;
            clickPhase_ = 0;
            clickRemaining_ = clickLen;
            ++beat;
            nextBeatFrame = frameOfBeat(beat);
        }
        if (clickRemaining_ > 0) {
            const double env = static_cast<double>(clickRemaining_) / clickLen;
            const auto v = static_cast<float>(0.3 * env * env * std::sin(2 * kPi * clickPhase_));
            clickPhase_ += clickInc_;
            --clickRemaining_;
            left[i] += v;
            right[i] += v;
        }
    }
}

namespace {
AudioBuffer renderWith(const Project& p, MediaProvider& media, PluginHost& host, EngineOptions opts,
                       double tailSeconds, std::vector<EngineIssue>* issues) {
    Engine e;
    e.prepare(p, media, host, opts);
    if (issues) *issues = e.issues();
    auto total = static_cast<int64_t>(
        std::ceil((p.tickToSeconds(static_cast<double>(p.endTick())) + tailSeconds) * opts.sampleRate));
    AudioBuffer out;
    out.sampleRate = opts.sampleRate;
    out.channels = 2;
    out.samples.resize(static_cast<size_t>(total * 2));
    std::vector<float> l(static_cast<size_t>(opts.maxBlock)), r(static_cast<size_t>(opts.maxBlock));
    for (int64_t done = 0; done < total;) {
        int n = static_cast<int>(std::min<int64_t>(opts.maxBlock, total - done));
        e.process(l.data(), r.data(), n);
        for (int i = 0; i < n; ++i) {
            out.samples[static_cast<size_t>((done + i) * 2)] = l[static_cast<size_t>(i)];
            out.samples[static_cast<size_t>((done + i) * 2 + 1)] = r[static_cast<size_t>(i)];
        }
        done += n;
    }
    return out;
}
}  // namespace

AudioBuffer renderProject(const Project& p, MediaProvider& media, PluginHost& host, double sampleRate,
                          double tailSeconds, std::vector<EngineIssue>* issues) {
    EngineOptions o;
    o.sampleRate = sampleRate;
    return renderWith(p, media, host, o, tailSeconds, issues);
}

AudioBuffer renderTrackPreFader(const Project& p, const std::string& trackId, MediaProvider& media,
                                PluginHost& host, double sampleRate, double tailSeconds) {
    EngineOptions o;
    o.sampleRate = sampleRate;
    o.isolateTrackPreFader = trackId;
    return renderWith(p, media, host, o, tailSeconds, nullptr);
}

}  // namespace ynot
