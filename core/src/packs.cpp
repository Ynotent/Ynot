#include "ynot/packs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ynot/clearance.hpp"
#include "ynot/demo.hpp"
#include "ynot/drums.hpp"
#include "ynot/edit.hpp"
#include "ynot/inspire.hpp"
#include "ynot/project_format.hpp"
#include "ynot/recorder.hpp"
#include "ynot/sampler.hpp"

namespace ynot {

namespace {

constexpr double kTau = 6.283185307179586;

struct LoopSpec {
    const char* id;
    const char* name;
    double bpm;
    int tonic;
    bool minor;
    double busy, bright;
    uint32_t seed;
    bool drumsOnly, withDrums;
};

const std::vector<LoopSpec>& loopSpecs() {
    static const std::vector<LoopSpec> v = {
        // Breaks (drums only).
        {"breaks/lofi-84", "Dusty Swing Break", 84, 9, true, 0.4, 0.3, 11, true, true},
        {"breaks/boombap-92", "Head Nod Break", 92, 2, true, 0.5, 0.4, 23, true, true},
        {"breaks/pop-110", "Bright Pop Beat", 110, 0, false, 0.5, 0.6, 31, true, true},
        {"breaks/house-124", "Warehouse Four", 124, 5, false, 0.6, 0.6, 47, true, true},
        {"breaks/trap-140", "Half-time Trap Hats", 140, 1, true, 0.7, 0.5, 59, true, true},
        {"breaks/trap-150", "Rolling Trap Beat", 150, 6, true, 0.8, 0.5, 61, true, true},
        // Melodic loops (no drums), each with its key and tempo.
        {"loops/late-night-keys", "Late Night Keys", 84, 9, true, 0.25, 0.5, 101, false, false},
        {"loops/sunday-chords", "Sunday Chords", 92, 5, false, 0.3, 0.55, 113, false, false},
        {"loops/sunrise-pluck", "Sunrise Pluck", 104, 0, false, 0.7, 0.7, 127, false, false},
        {"loops/neon-stabs", "Neon Stabs", 124, 2, true, 0.8, 0.7, 131, false, false},
        {"loops/cold-bells", "Cold Bells", 140, 6, true, 0.6, 0.8, 149, false, false},
        {"loops/midnight-drive", "Midnight Drive", 118, 4, true, 0.5, 0.6, 157, false, false},
        // Full ideas (music + drums).
        {"ideas/lofi-study", "Lo-fi Study Loop", 82, 7, false, 0.35, 0.4, 211, false, true},
        {"ideas/trap-dark", "Dark Trap Idea", 142, 11, true, 0.6, 0.5, 223, false, true},
        {"ideas/house-glow", "Glow House Idea", 124, 9, true, 0.7, 0.6, 227, false, true},
    };
    return v;
}

InspiredIdea ideaFor(const LoopSpec& s) {
    ReferenceProfile r;
    r.bpm = s.bpm;
    r.key.tonic = s.tonic;
    r.key.minor = s.minor;
    r.busyness = s.busy;
    r.brightness = s.bright;
    return generateIdea(r, s.seed, 4);
}

AudioBuffer drumHit(int note, double sr) {
    auto kit = makeDrumKit(Json::object(), sr);
    const int n = static_cast<int>(sr * (note == 49 || note == 46 ? 1.6 : 0.8));
    std::vector<float> l(static_cast<size_t>(n), 0.f), r(static_cast<size_t>(n), 0.f);
    const MidiEvent on(0, true, note, 112);
    for (int done = 0; done < n; done += 512) {
        const int k = std::min(512, n - done);
        kit->process(l.data() + done, r.data() + done, k, done == 0 ? &on : nullptr, done == 0 ? 1 : 0);
    }
    AudioBuffer a;
    a.sampleRate = sr;
    a.channels = 2;
    for (int i = 0; i < n; ++i) {
        a.samples.push_back(l[static_cast<size_t>(i)]);
        a.samples.push_back(r[static_cast<size_t>(i)]);
    }
    return a;
}

// Long tuned kick ("808"), sub bass and a shaker: synthesised here.
AudioBuffer synthHit(const std::string& which, double sr) {
    AudioBuffer a;
    a.sampleRate = sr;
    a.channels = 2;
    const double len = which == "808" ? 1.8 : which == "sub" ? 1.2 : 0.25;
    double phase = 0;
    uint32_t seed = 12345;
    for (int i = 0; i < static_cast<int>(len * sr); ++i) {
        const double t = i / sr;
        double v = 0;
        if (which == "808") {
            const double f = 49.0 + 120.0 * std::exp(-t * 28);  // G1 with a pitch drop
            phase += kTau * f / sr;
            v = 0.9 * std::tanh(1.6 * std::sin(phase)) * std::exp(-t * 1.6) * std::min(1.0, t / 0.002);
        } else if (which == "sub") {
            phase += kTau * 65.41 / sr;  // C2
            v = 0.7 * std::sin(phase) * std::min(1.0, t / 0.01) * std::exp(-t * 1.2);
        } else {
            seed = seed * 1664525u + 1013904223u;
            const double noise = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
            v = 0.4 * noise * std::exp(-t * 30) * std::min(1.0, t / 0.008);
        }
        a.samples.push_back(static_cast<float>(v));
        a.samples.push_back(static_cast<float>(v));
    }
    return a;
}

std::string keyText(int tonic, bool minor) {
    KeyEstimate k;
    k.tonic = tonic;
    k.minor = minor;
    return k.name();
}

}  // namespace

const std::vector<PackSound>& packSounds() {
    static const std::vector<PackSound> v = [] {
        std::vector<PackSound> out;
        for (const auto& d : drumKitSounds())
            out.push_back({"drums/" + std::to_string(d.note), "Ynot Drum Hits", d.name, "one-shot", 0, "", 0});
        out.push_back({"drums/808", "Ynot Drum Hits", "808 Kick (G)", "one-shot", 0, "", 0});
        out.push_back({"drums/sub", "Ynot Drum Hits", "Sub Bass (C)", "one-shot", 0, "", 0});
        out.push_back({"drums/shaker", "Ynot Drum Hits", "Shaker", "one-shot", 0, "", 0});
        for (const auto& s : loopSpecs()) {
            const char* pack = s.drumsOnly ? "Ynot Breaks" : s.withDrums ? "Ynot Ideas" : "Ynot Loops";
            out.push_back({s.id, pack, s.name, "loop", s.bpm, s.drumsOnly ? "" : keyText(s.tonic, s.minor), 4});
        }
        return out;
    }();
    return v;
}

const PackSound* findPackSound(const std::string& id) {
    for (const auto& s : packSounds())
        if (s.id == id) return &s;
    return nullptr;
}

AudioBuffer renderPackSound(const std::string& id, double sr) {
    if (id == "drums/808") return synthHit("808", sr);
    if (id == "drums/sub") return synthHit("sub", sr);
    if (id == "drums/shaker") return synthHit("shaker", sr);
    if (id.rfind("drums/", 0) == 0) return drumHit(std::atoi(id.c_str() + 6), sr);
    for (const auto& s : loopSpecs())
        if (id == s.id) {
            auto idea = ideaFor(s);
            if (s.drumsOnly) {
                idea.chords.clear();
                idea.bass.clear();
                idea.melody.clear();
            }
            return renderIdea(idea, sr, s.withDrums);
        }
    return {};
}

std::string addPackMedia(Project& p, const std::string& packageDir, const std::string& soundId, double sampleRate) {
    const auto* s = findPackSound(soundId);
    if (!s) return {};
    const auto audio = renderPackSound(soundId, sampleRate);
    if (audio.frames() <= 0) return {};
    const std::string mediaId = addMedia(p, packageDir, audio, "pack").id;
    SampleCredit c;
    if (const auto* old = findCredit(p, mediaId)) c = *old;
    c.mediaId = mediaId;
    c.fileName = s->pack + ": " + s->name;
    c.title = s->name;
    c.artist = "Ynot";
    c.license = kPackLicense;
    c.status = ClearanceStatus::RoyaltyFree;
    setCredit(p, c);
    return mediaId;
}

std::string addPackSoundToSampler(Project& p, const std::string& packageDir, const std::string& soundId, double sampleRate) {
    const auto* s = findPackSound(soundId);
    if (!s) return {};
    const std::string mediaId = addPackMedia(p, packageDir, soundId, sampleRate);
    if (mediaId.empty()) return {};
    const std::string id = addTrack(p, TrackKind::Instrument, s->name);
    setInstrument(p, id, makeBuiltin(kBuiltinSampler, "Ynot Sampler"));
    auto* sampler = samplerOf(p, id);
    sampler->params["mediaId"] = mediaId;
    const MediaRef* m = p.findMedia(mediaId);
    if (s->kind == "one-shot") {
        // One-shots play across the keyboard (a tuned 808 or sub is most useful that way).
        sampler->params["mode"] = "keys";
        sampler->params["rootNote"] = soundId == "drums/808" ? 43 : soundId == "drums/sub" ? 36 : 60;
        sampler->params["slices"] = Json::array();
    } else {
        // Loops: one slice per beat (4 bars = 16 pads).
        sampler->params["mode"] = "chop";
        Json sl = Json::array();
        for (auto f : equalSlices(m ? m->frames : 0, 16)) sl.push(Json(f));
        sampler->params["slices"] = sl;
    }
    return id;
}

std::string addPackSoundAsClip(Project& p, const std::string& packageDir, const std::string& soundId,
                               const std::string& trackId, int64_t startTick, double sampleRate) {
    const auto* s = findPackSound(soundId);
    if (!s) return {};
    std::string tid = trackId;
    const Track* t = p.findTrack(tid);
    if (!t || t->kind != TrackKind::Audio) tid = addTrack(p, TrackKind::Audio, s->name);
    const std::string mediaId = addPackMedia(p, packageDir, soundId, sampleRate);
    const MediaRef* m = p.findMedia(mediaId);
    if (!m) return {};
    Track* track = p.findTrack(tid);
    AudioClip c;
    c.id = newId();
    c.mediaId = mediaId;
    c.startTick = std::max<int64_t>(0, startTick);
    // Loops are made at their own tempo: at the song's tempo they're stretched to fit later
    // (Time stretch > Match tempo); the clip covers the audio's real length.
    c.lengthTick = std::max<int64_t>(1, ticksSpanning(p, c.startTick, m->frames, m->sampleRate));
    const std::string id = c.id;
    track->audioClips.push_back(c);
    std::sort(track->audioClips.begin(), track->audioClips.end(),
              [](const AudioClip& a, const AudioClip& b) { return a.startTick < b.startTick; });
    return id;
}

}  // namespace ynot
