#include "ynot/sampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "ynot/demo.hpp"
#include "ynot/edit.hpp"
#include "ynot/project_format.hpp"
#include "ynot/recorder.hpp"

namespace ynot {

namespace {

constexpr int kVoices = 32;

class Sampler : public PluginProcessor {
public:
    Sampler(const Json& params, const AudioBuffer* sample, double sr) : sample_(sample), sr_(sr) {
        keys_ = params.getString("mode", "chop") == "keys";
        baseNote_ = static_cast<int>(params.getNumber("baseNote", 36));
        rootNote_ = static_cast<int>(params.getNumber("rootNote", 60));
        tune_ = params.getNumber("tune", 0);
        gain_ = dbToGain(params.getNumber("gainDb", 0));
        attack_ = std::max(1.0, params.getNumber("attackMs", 1) * 0.001 * sr);
        release_ = std::max(1.0, params.getNumber("releaseMs", keys_ ? 120 : 15) * 0.001 * sr);
        if (const Json* s = params.find("slices"); s && s->isArray())
            for (const auto& v : s->asArray())
                if (v.isNumber()) slices_.push_back(v.asInt());
        if (sample_) {
            const int64_t n = sample_->frames();
            slices_.erase(std::remove_if(slices_.begin(), slices_.end(), [n](int64_t f) { return f < 0 || f >= n; }),
                          slices_.end());
            std::sort(slices_.begin(), slices_.end());
            slices_.erase(std::unique(slices_.begin(), slices_.end()), slices_.end());
            if (slices_.empty() || slices_.front() != 0) slices_.insert(slices_.begin(), 0);
        }
    }

    void process(float* left, float* right, int frames, const MidiEvent* events, int numEvents) override {
        int ev = 0;
        for (int i = 0; i < frames; ++i) {
            while (ev < numEvents && events[ev].frameOffset <= i) handle(events[ev++]);
            float l = 0, r = 0;
            for (auto& v : voices_)
                if (v.active) renderVoice(v, l, r);
            left[i] += l;
            right[i] += r;
        }
        while (ev < numEvents) handle(events[ev++]);
    }

    void setParameter(const std::string& id, double v) override {
        if (id == "gain") gain_ = v * 2.0;
    }

    void reset() override {
        for (auto& v : voices_) v.active = false;
        sustain_ = false;
        bend_ = 1.0;
    }

private:
    struct Voice {
        bool active = false, held = false, sustained = false, releasing = false;
        int note = 0;
        double pos = 0, end = 0, rate = 1;  // in sample frames; rate before pitch bend
        double env = 0, level = 0;
        bool bends = false;
    };

    void handle(const MidiEvent& e) {
        if (e.kind == MidiEvent::Kind::PitchBend) {
            bend_ = std::pow(2.0, 2.0 * (e.velocity - kPitchBendCentre) / 8192.0 / 12.0);
            return;
        }
        if (e.kind == MidiEvent::Kind::Control) {
            if (e.pitch == kCcSustain) {
                sustain_ = e.velocity >= 64;
                if (!sustain_)
                    for (auto& v : voices_)
                        if (v.active && v.sustained && !v.held) v.releasing = true;
            } else if (e.pitch == 120 || e.pitch == 123) {
                for (auto& v : voices_) v.releasing = true;
            }
            return;
        }
        if (e.noteOn && e.velocity > 0) noteOn(e.pitch, e.velocity);
        else noteOff(e.pitch);
    }

    void noteOn(int note, int velocity) {
        if (!sample_ || sample_->frames() < 2) return;
        Voice nv;
        nv.note = note;
        nv.level = gain_ * std::pow(velocity / 127.0, 1.5);
        const double srcRate = sample_->sampleRate / sr_;
        if (keys_) {
            nv.pos = 0;
            nv.end = static_cast<double>(sample_->frames() - 1);
            nv.rate = srcRate * std::pow(2.0, (note - rootNote_ + tune_) / 12.0);
            nv.bends = true;
        } else {
            const int s = note - baseNote_;
            if (s < 0 || s >= static_cast<int>(slices_.size())) return;
            nv.pos = static_cast<double>(slices_[static_cast<size_t>(s)]);
            nv.end = static_cast<double>(s + 1 < static_cast<int>(slices_.size()) ? slices_[static_cast<size_t>(s + 1)]
                                                                                   : sample_->frames()) - 1;
            nv.rate = srcRate * std::pow(2.0, tune_ / 12.0);
            // A pad cuts itself off (like hardware samplers): retrigger restarts it.
            for (auto& v : voices_)
                if (v.active && v.note == note) v.releasing = true;
        }
        nv.active = true;
        nv.held = true;
        Voice* slot = nullptr;
        for (auto& v : voices_)
            if (!v.active) { slot = &v; break; }
        if (!slot) {  // steal the quietest
            slot = &voices_[0];
            for (auto& v : voices_)
                if (v.env * v.level < slot->env * slot->level) slot = &v;
        }
        *slot = nv;
    }

    void noteOff(int note) {
        for (auto& v : voices_)
            if (v.active && v.held && v.note == note) {
                v.held = false;
                if (!keys_) continue;  // one-shot slices play to their end
                if (sustain_) v.sustained = true;
                else v.releasing = true;
            }
    }

    void renderVoice(Voice& v, float& l, float& r) {
        const auto& s = *sample_;
        const int64_t i0 = static_cast<int64_t>(v.pos);
        if (v.pos >= v.end || i0 + 1 >= s.frames()) {
            v.active = false;
            return;
        }
        // Envelope: linear attack, exponential-ish release; a short fade at the slice end
        // avoids clicks when a slice stops mid-sound.
        if (v.releasing) {
            v.env -= 1.0 / release_;
            if (v.env <= 0) {
                v.active = false;
                return;
            }
        } else if (v.env < 1.0) {
            v.env = std::min(1.0, v.env + 1.0 / attack_);
        }
        const double tail = std::min(1.0, (v.end - v.pos) / (0.003 * s.sampleRate + 1.0));
        const double g = v.env * v.level * tail;
        const double frac = v.pos - static_cast<double>(i0);
        const int ch = s.channels;
        const size_t a = static_cast<size_t>(i0 * ch), b = static_cast<size_t>((i0 + 1) * ch);
        const double sl = s.samples[a] + (s.samples[b] - s.samples[a]) * frac;
        const double sr = ch > 1 ? s.samples[a + 1] + (s.samples[b + 1] - s.samples[a + 1]) * frac : sl;
        l += static_cast<float>(sl * g);
        r += static_cast<float>(sr * g);
        v.pos += v.rate * (v.bends ? bend_ : 1.0);
    }

    const AudioBuffer* sample_;
    double sr_;
    bool keys_ = false;
    int baseNote_ = 36, rootNote_ = 60;
    double tune_ = 0, gain_ = 1, attack_ = 48, release_ = 720;
    std::vector<int64_t> slices_;
    std::array<Voice, kVoices> voices_{};
    bool sustain_ = false;
    double bend_ = 1.0;
};

std::vector<float> monoOf(const AudioBuffer& a) {
    std::vector<float> m(static_cast<size_t>(a.frames()));
    for (int64_t f = 0; f < a.frames(); ++f) {
        float s = 0;
        for (int c = 0; c < a.channels; ++c) s += a.samples[static_cast<size_t>(f * a.channels + c)];
        m[static_cast<size_t>(f)] = s / static_cast<float>(std::max(1, a.channels));
    }
    return m;
}

Json slicesJson(const std::vector<int64_t>& s) {
    Json a = Json::array();
    for (auto f : s) a.push(Json(f));
    return a;
}

std::vector<int64_t> normalized(std::vector<int64_t> s, int64_t frames) {
    s.erase(std::remove_if(s.begin(), s.end(), [frames](int64_t f) { return f < 0 || (frames > 0 && f >= frames); }),
            s.end());
    s.push_back(0);
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    if (s.size() > static_cast<size_t>(kMaxSlices)) s.resize(kMaxSlices);
    return s;
}

int64_t sampleFrames(const Project& p, const PluginInstance& s) {
    const MediaRef* m = p.findMedia(s.params.getString("mediaId", ""));
    return m ? m->frames : 0;
}

}  // namespace

std::unique_ptr<PluginProcessor> makeSampler(const Json& params, const AudioBuffer* sample, double sampleRate) {
    return std::make_unique<Sampler>(params, sample, sampleRate);
}

std::vector<int64_t> detectSlices(const AudioBuffer& sample, int maxSlices) {
    const auto mono = monoOf(sample);
    const int64_t n = static_cast<int64_t>(mono.size());
    maxSlices = std::clamp(maxSlices, 1, kMaxSlices);
    const int hop = std::max(64, static_cast<int>(sample.sampleRate * 0.005));  // 5 ms
    const int64_t frames = n / hop;
    if (frames < 3) return {0};
    // Onset strength: rise in log energy, frame to frame (works for drums, plucks and words).
    std::vector<double> energy(static_cast<size_t>(frames)), flux(static_cast<size_t>(frames), 0.0);
    // Energy over the 20 ms ending at each 5 ms step: long enough that a deep kick's single
    // wave cycle doesn't look like several hits, short enough to place the slice tightly.
    const int64_t win = 4 * static_cast<int64_t>(hop);
    for (int64_t f = 0; f < frames; ++f) {
        double e = 0;
        const int64_t end = (f + 1) * hop, begin = std::max<int64_t>(0, end - win);
        for (int64_t i = begin; i < end; ++i) {
            const double v = mono[static_cast<size_t>(i)];
            e += v * v;
        }
        energy[static_cast<size_t>(f)] = 10.0 * std::log10(e / static_cast<double>(end - begin) + 1e-10);
    }
    for (int64_t f = 1; f < frames; ++f) {
        // Compare with the quietest of the last 3 frames, so slow swells don't count.
        double before = energy[static_cast<size_t>(f - 1)];
        for (int64_t k = std::max<int64_t>(0, f - 3); k < f; ++k) before = std::min(before, energy[static_cast<size_t>(k)]);
        const bool audible = energy[static_cast<size_t>(f)] > -50.0;
        flux[static_cast<size_t>(f)] = audible ? std::max(0.0, energy[static_cast<size_t>(f)] - before) : 0.0;
    }
    // Peaks above 6 dB rise, at least 60 ms apart; strongest first.
    struct Peak { int64_t frame; double strength; };
    std::vector<Peak> peaks;
    for (int64_t f = 1; f + 1 < frames; ++f) {
        const double v = flux[static_cast<size_t>(f)];
        if (v >= 6.0 && v >= flux[static_cast<size_t>(f - 1)] && v > flux[static_cast<size_t>(f + 1)]) peaks.push_back({f, v});
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.strength > b.strength; });
    const int64_t minGap = static_cast<int64_t>(0.06 * sample.sampleRate);
    std::vector<int64_t> out = {0};
    // Sound right at the start counts as the first slice.
    for (const auto& p : peaks) {
        if (static_cast<int>(out.size()) >= maxSlices) break;
        // Start just before the attack: the first sample that rises above a little of the hit's
        // peak, minus 1 ms, so the attack is whole and there's no silence before it.
        // The 20 ms energy window can flag a hit up to a window late: look back that far.
        const int64_t at = std::max<int64_t>(0, (p.frame - 1) * hop - win);
        float hitPeak = 0;
        for (int64_t k = at; k < std::min(n, at + win + 4 * hop); ++k) hitPeak = std::max(hitPeak, std::abs(mono[static_cast<size_t>(k)]));
        // Above the level already there (a previous sound's tail), so the slice doesn't start early.
        float before = 0;
        for (int64_t k = at; k < std::min(n, at + hop / 2); ++k) before = std::max(before, std::abs(mono[static_cast<size_t>(k)]));
        const float threshold = std::max(0.05f * hitPeak, 2.5f * before);
        int64_t best = at;
        for (int64_t k = at; k < std::min(n, at + win + 4 * hop); ++k)
            if (std::abs(mono[static_cast<size_t>(k)]) > threshold) {
                best = k;
                break;
            }
        best = std::max<int64_t>(0, best - static_cast<int64_t>(0.001 * sample.sampleRate));
        bool clear = true;
        for (auto o : out) clear &= std::llabs(o - best) >= minGap;
        if (clear) out.push_back(best);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int64_t> equalSlices(int64_t frames, int count) {
    count = std::clamp(count, 1, kMaxSlices);
    std::vector<int64_t> out;
    for (int i = 0; i < count; ++i) out.push_back(frames * i / count);
    return normalized(out, frames);
}

std::vector<int64_t> samplerSlices(const PluginInstance& s) {
    std::vector<int64_t> out;
    if (const Json* a = s.params.find("slices"); a && a->isArray())
        for (const auto& v : a->asArray())
            if (v.isNumber()) out.push_back(v.asInt());
    return normalized(out, 0);
}

PluginInstance* samplerOf(Project& p, const std::string& trackId) {
    Track* t = p.findTrack(trackId);
    if (!t || !t->instrument || t->instrument->format != PluginFormat::Builtin || t->instrument->uid != kBuiltinSampler)
        return nullptr;
    return &*t->instrument;
}

const PluginInstance* samplerOf(const Project& p, const std::string& trackId) {
    return samplerOf(const_cast<Project&>(p), trackId);
}

std::string addSamplerTrack(Project& p, const std::string& packageDir, const AudioBuffer& sample,
                            const std::string& name) {
    const std::string id = addTrack(p, TrackKind::Instrument, name.empty() ? "Sampler" : name);
    setInstrument(p, id, makeBuiltin(kBuiltinSampler, "Ynot Sampler"));
    loadSample(p, packageDir, id, sample);
    return id;
}

bool loadSample(Project& p, const std::string& packageDir, const std::string& trackId, const AudioBuffer& sample) {
    if (!samplerOf(p, trackId) || sample.frames() <= 0) return false;
    const std::string mediaId = addMedia(p, packageDir, sample, "sample").id;
    PluginInstance* s = samplerOf(p, trackId);
    s->params["mediaId"] = mediaId;
    if (!s->params.contains("mode")) s->params["mode"] = "chop";
    s->params["slices"] = slicesJson(detectSlices(sample, 16));
    return true;
}

AudioBuffer clipAudio(const Project& p, MediaProvider& media, const std::string& trackId, const std::string& clipId) {
    AudioBuffer out;
    const Track* t = p.findTrack(trackId);
    if (!t) return out;
    for (const auto& c : t->audioClips) {
        if (c.id != clipId) continue;
        const MediaRef* m = p.findMedia(c.mediaId);
        const AudioBuffer* b = m ? media.get(*m) : nullptr;
        if (!b) return out;
        out.sampleRate = b->sampleRate;
        out.channels = b->channels;
        const int64_t len = framesSpanning(p, c.startTick, c.lengthTick, b->sampleRate);
        const int64_t from = std::clamp<int64_t>(c.sourceOffsetFrames, 0, b->frames());
        const int64_t to = std::clamp<int64_t>(from + len, from, b->frames());
        out.samples.assign(b->samples.begin() + from * b->channels, b->samples.begin() + to * b->channels);
        const double g = dbToGain(c.gainDb);
        if (std::abs(g - 1.0) > 1e-9)
            for (auto& v : out.samples) v = static_cast<float>(v * g);
        return out;
    }
    return out;
}

bool setSlices(Project& p, const std::string& trackId, std::vector<int64_t> starts) {
    PluginInstance* s = samplerOf(p, trackId);
    if (!s) return false;
    s->params["slices"] = slicesJson(normalized(std::move(starts), sampleFrames(p, *s)));
    return true;
}

bool addSlice(Project& p, const std::string& trackId, int64_t frame) {
    const PluginInstance* s = samplerOf(p, trackId);
    if (!s) return false;
    auto v = samplerSlices(*s);
    if (static_cast<int>(v.size()) >= kMaxSlices) return false;
    v.push_back(frame);
    return setSlices(p, trackId, v);
}

bool removeSlice(Project& p, const std::string& trackId, int index) {
    const PluginInstance* s = samplerOf(p, trackId);
    if (!s) return false;
    auto v = samplerSlices(*s);
    if (index <= 0 || index >= static_cast<int>(v.size())) return false;
    v.erase(v.begin() + index);
    return setSlices(p, trackId, v);
}

bool moveSlice(Project& p, const std::string& trackId, int index, int64_t frame) {
    const PluginInstance* s = samplerOf(p, trackId);
    if (!s) return false;
    auto v = samplerSlices(*s);
    if (index <= 0 || index >= static_cast<int>(v.size())) return false;
    // Stays between its neighbours, so slice numbers (pads) don't swap.
    const int64_t lo = v[static_cast<size_t>(index - 1)] + 1;
    const int64_t hi = index + 1 < static_cast<int>(v.size()) ? v[static_cast<size_t>(index + 1)] - 1 : sampleFrames(p, *s) - 1;
    if (hi < lo) return false;
    v[static_cast<size_t>(index)] = std::clamp(frame, lo, hi);
    return setSlices(p, trackId, v);
}

}  // namespace ynot
