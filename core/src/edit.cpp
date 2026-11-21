#include "ynot/edit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ynot/engine.hpp"
#include "ynot/project_format.hpp"
#include "ynot/recorder.hpp"

namespace ynot {

int64_t snapTick(int64_t tick, int64_t grid) {
    if (grid <= 0) return tick;
    return static_cast<int64_t>(std::llround(static_cast<double>(tick) / static_cast<double>(grid))) * grid;
}

namespace {
AudioClip* findAudio(Track& t, const std::string& id) {
    for (auto& c : t.audioClips)
        if (c.id == id) return &c;
    return nullptr;
}
MidiClip* findMidi(Track& t, const std::string& id) {
    for (auto& c : t.midiClips)
        if (c.id == id) return &c;
    return nullptr;
}
double mediaRate(const Project& p, const AudioClip& c) {
    const MediaRef* m = p.findMedia(c.mediaId);
    return m ? m->sampleRate : 48000.0;
}
}  // namespace

int64_t maxAudioClipTicks(const Project& p, const AudioClip& c) {
    const MediaRef* m = p.findMedia(c.mediaId);
    if (!m || m->frames <= 0) return std::numeric_limits<int64_t>::max() / 4;  // unknown: don't constrain
    return frameToTick(p, std::max<int64_t>(0, m->frames - c.sourceOffsetFrames), m->sampleRate);
}

std::optional<ClipSpan> findClip(const Project& p, const std::string& trackId, const std::string& clipId) {
    const Track* t = p.findTrack(trackId);
    if (!t) return std::nullopt;
    for (auto& c : t->audioClips)
        if (c.id == clipId) return ClipSpan{trackId, clipId, true, c.startTick, c.lengthTick};
    for (auto& c : t->midiClips)
        if (c.id == clipId) return ClipSpan{trackId, clipId, false, c.startTick, c.lengthTick};
    return std::nullopt;
}

bool moveClip(Project& p, const std::string& trackId, const std::string& clipId, int64_t newStartTick,
              const std::string& toTrackId) {
    Track* from = p.findTrack(trackId);
    if (!from) return false;
    Track* to = toTrackId.empty() ? from : p.findTrack(toTrackId);
    if (!to) return false;
    newStartTick = std::max<int64_t>(0, newStartTick);

    if (AudioClip* c = findAudio(*from, clipId)) {
        if (to->kind != TrackKind::Audio) return false;
        AudioClip moved = *c;
        moved.startTick = newStartTick;
        from->audioClips.erase(from->audioClips.begin() + (c - from->audioClips.data()));
        placeClipReplacing(p, *to, moved);
        return true;
    }
    if (MidiClip* c = findMidi(*from, clipId)) {
        if (to->kind != TrackKind::Instrument) return false;
        MidiClip moved = *c;
        moved.startTick = newStartTick;
        from->midiClips.erase(from->midiClips.begin() + (c - from->midiClips.data()));
        to->midiClips.push_back(moved);
        std::sort(to->midiClips.begin(), to->midiClips.end(),
                  [](const MidiClip& a, const MidiClip& b) { return a.startTick < b.startTick; });
        return true;
    }
    return false;
}

bool trimClipStart(Project& p, const std::string& trackId, const std::string& clipId, int64_t newStartTick) {
    Track* t = p.findTrack(trackId);
    if (!t) return false;
    if (AudioClip* c = findAudio(*t, clipId)) {
        const int64_t end = c->startTick + c->lengthTick;
        const double rate = mediaRate(p, *c);
        // Can't reveal audio before the start of the recording.
        const int64_t earliest = c->startTick - frameToTick(p, c->sourceOffsetFrames, rate);
        newStartTick = std::clamp<int64_t>(newStartTick, std::max<int64_t>(0, earliest), end - kMinClipTicks);
        const int64_t delta = newStartTick - c->startTick;
        c->sourceOffsetFrames = std::max<int64_t>(0, c->sourceOffsetFrames + (delta >= 0 ? tickToFrame(p, delta, rate)
                                                                                          : -tickToFrame(p, -delta, rate)));
        c->startTick = newStartTick;
        c->lengthTick = end - newStartTick;
        return true;
    }
    if (MidiClip* c = findMidi(*t, clipId)) {
        const int64_t end = c->startTick + c->lengthTick;
        newStartTick = std::clamp<int64_t>(newStartTick, 0, end - kMinClipTicks);
        const int64_t delta = newStartTick - c->startTick;
        for (auto& n : c->notes) n.tick -= delta;  // notes before the clip start are hidden, not deleted
        c->startTick = newStartTick;
        c->lengthTick = end - newStartTick;
        return true;
    }
    return false;
}

bool trimClipEnd(Project& p, const std::string& trackId, const std::string& clipId, int64_t newEndTick) {
    Track* t = p.findTrack(trackId);
    if (!t) return false;
    if (AudioClip* c = findAudio(*t, clipId)) {
        const int64_t maxLen = maxAudioClipTicks(p, *c);
        c->lengthTick = std::clamp<int64_t>(newEndTick - c->startTick, kMinClipTicks, maxLen);
        return true;
    }
    if (MidiClip* c = findMidi(*t, clipId)) {
        c->lengthTick = std::max<int64_t>(kMinClipTicks, newEndTick - c->startTick);
        return true;
    }
    return false;
}

std::optional<std::string> splitClip(Project& p, const std::string& trackId, const std::string& clipId,
                                     int64_t atTick) {
    Track* t = p.findTrack(trackId);
    if (!t) return std::nullopt;
    if (AudioClip* c = findAudio(*t, clipId)) {
        const int64_t s = c->startTick, e = c->startTick + c->lengthTick;
        if (atTick < s + kMinClipTicks || atTick > e - kMinClipTicks) return std::nullopt;
        AudioClip right = *c;
        right.id = newId();
        right.startTick = atTick;
        right.lengthTick = e - atTick;
        right.sourceOffsetFrames = c->sourceOffsetFrames + tickToFrame(p, atTick - s, mediaRate(p, *c));
        c->lengthTick = atTick - s;
        std::string id = right.id;
        t->audioClips.push_back(right);
        std::sort(t->audioClips.begin(), t->audioClips.end(),
                  [](const AudioClip& a, const AudioClip& b) { return a.startTick < b.startTick; });
        return id;
    }
    if (MidiClip* c = findMidi(*t, clipId)) {
        const int64_t s = c->startTick, e = c->startTick + c->lengthTick;
        if (atTick < s + kMinClipTicks || atTick > e - kMinClipTicks) return std::nullopt;
        MidiClip right = *c;
        right.id = newId();
        right.startTick = atTick;
        right.lengthTick = e - atTick;
        for (auto& n : right.notes) n.tick -= atTick - s;
        c->lengthTick = atTick - s;
        std::string id = right.id;
        t->midiClips.push_back(right);
        std::sort(t->midiClips.begin(), t->midiClips.end(),
                  [](const MidiClip& a, const MidiClip& b) { return a.startTick < b.startTick; });
        return id;
    }
    return std::nullopt;
}

bool deleteClip(Project& p, const std::string& trackId, const std::string& clipId) {
    Track* t = p.findTrack(trackId);
    if (!t) return false;
    auto a = std::remove_if(t->audioClips.begin(), t->audioClips.end(), [&](const AudioClip& c) { return c.id == clipId; });
    auto m = std::remove_if(t->midiClips.begin(), t->midiClips.end(), [&](const MidiClip& c) { return c.id == clipId; });
    bool removed = a != t->audioClips.end() || m != t->midiClips.end();
    t->audioClips.erase(a, t->audioClips.end());
    t->midiClips.erase(m, t->midiClips.end());
    return removed;
}

int eraseNotesInRange(Track& track, int64_t fromTick, int64_t toTick) {
    int removed = 0;
    for (auto& c : track.midiClips) {
        auto& notes = c.notes;
        const auto before = notes.size();
        notes.erase(std::remove_if(notes.begin(), notes.end(),
                                   [&](const MidiNote& n) {
                                       if (n.tick < 0 || n.tick >= c.lengthTick) return false;  // hidden: keep
                                       const int64_t abs = c.startTick + n.tick;
                                       return abs >= fromTick && abs < toTick;
                                   }),
                    notes.end());
        removed += static_cast<int>(before - notes.size());
    }
    return removed;
}

std::string importAudioClip(Project& p, const std::string& packageDir, const std::string& trackId,
                            const AudioBuffer& audio, int64_t startTick) {
    Track* t = p.findTrack(trackId);
    if (!t || t->kind != TrackKind::Audio || audio.frames() <= 0 || audio.sampleRate <= 0) return {};
    const std::string mediaId = addMedia(p, packageDir, audio, "import").id;
    t = p.findTrack(trackId);
    AudioClip c;
    c.id = newId();
    c.mediaId = mediaId;
    c.startTick = std::max<int64_t>(0, startTick);
    c.lengthTick = std::max<int64_t>(kMinClipTicks, frameToTick(p, audio.frames(), audio.sampleRate));
    const std::string id = c.id;
    placeClipReplacing(p, *t, std::move(c));
    return id;
}

const std::vector<std::string>& trackPalette() {
    static const std::vector<std::string> v = {"#7C5CFF", "#2BD9A8", "#FFB547", "#FF6B9A",
                                               "#4DA3FF", "#B8E04A", "#FF8A4D", "#A98BFF"};
    return v;
}

std::string addTrack(Project& p, TrackKind kind, const std::string& name) {
    Track t;
    t.id = newId();
    t.kind = kind;
    int count = 0;
    double maxOrder = 0;
    for (auto& existing : p.tracks) {
        maxOrder = std::max(maxOrder, existing.order);
        count += existing.kind == kind;
    }
    t.order = maxOrder + 1;
    t.name = !name.empty() ? name : (kind == TrackKind::Audio ? "Audio " : "Instrument ") + std::to_string(count + 1);
    t.color = trackPalette()[p.tracks.size() % trackPalette().size()];
    if (kind == TrackKind::Instrument) {
        PluginInstance synth;
        synth.id = newId();
        synth.name = "Ynot Synth";
        synth.vendor = "Ynot";
        synth.format = PluginFormat::Builtin;
        synth.uid = kBuiltinSynth;
        synth.version = "1";
        t.instrument = synth;
    }
    p.tracks.push_back(t);
    return t.id;
}

bool removeTrack(Project& p, const std::string& trackId) {
    auto it = std::find_if(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return t.id == trackId; });
    if (it == p.tracks.end()) return false;
    p.tracks.erase(it);
    return true;
}

bool renameTrack(Project& p, const std::string& trackId, const std::string& name) {
    Track* t = p.findTrack(trackId);
    if (!t || name.empty()) return false;
    t->name = name;
    return true;
}

std::string addInsert(Project& p, const std::string& trackId, PluginInstance plugin, int index) {
    Track* t = p.findTrack(trackId);
    if (!t) return {};
    if (plugin.id.empty()) plugin.id = newId();
    const auto pos = (index < 0 || index > static_cast<int>(t->inserts.size())) ? t->inserts.end()
                                                                                 : t->inserts.begin() + index;
    std::string id = plugin.id;
    t->inserts.insert(pos, std::move(plugin));
    return id;
}

bool setInstrument(Project& p, const std::string& trackId, PluginInstance plugin) {
    Track* t = p.findTrack(trackId);
    if (!t || t->kind != TrackKind::Instrument) return false;
    if (plugin.id.empty()) plugin.id = newId();
    t->instrument = std::move(plugin);
    t->frozen = false;  // the sound changed: an old freeze no longer applies
    return true;
}

bool removePlugin(Project& p, const std::string& trackId, const std::string& pluginId) {
    Track* t = p.findTrack(trackId);
    if (!t) return false;
    if (t->instrument && t->instrument->id == pluginId) {
        t->instrument.reset();
        return true;
    }
    auto it = std::find_if(t->inserts.begin(), t->inserts.end(), [&](const PluginInstance& x) { return x.id == pluginId; });
    if (it == t->inserts.end()) return false;
    t->inserts.erase(it);
    return true;
}

bool setPluginBypassed(Project& p, const std::string& trackId, const std::string& pluginId, bool bypassed) {
    Track* t = p.findTrack(trackId);
    if (!t) return false;
    if (t->instrument && t->instrument->id == pluginId) {
        t->instrument->bypassed = bypassed;
        return true;
    }
    for (auto& x : t->inserts)
        if (x.id == pluginId) {
            x.bypassed = bypassed;
            return true;
        }
    return false;
}

bool moveInsert(Project& p, const std::string& trackId, const std::string& pluginId, int newIndex) {
    Track* t = p.findTrack(trackId);
    if (!t) return false;
    auto it = std::find_if(t->inserts.begin(), t->inserts.end(), [&](const PluginInstance& x) { return x.id == pluginId; });
    if (it == t->inserts.end()) return false;
    PluginInstance moved = *it;
    t->inserts.erase(it);
    newIndex = std::clamp(newIndex, 0, static_cast<int>(t->inserts.size()));
    t->inserts.insert(t->inserts.begin() + newIndex, moved);
    return true;
}

void History::record(const Project& before) {
    undo_.push_back(projectToJson(before));
    if (undo_.size() > limit_) undo_.erase(undo_.begin());
    redo_.clear();
}

bool History::undo(Project& current) {
    if (undo_.empty()) return false;
    redo_.push_back(projectToJson(current));
    current = projectFromJson(undo_.back());
    undo_.pop_back();
    return true;
}

bool History::redo(Project& current) {
    if (redo_.empty()) return false;
    undo_.push_back(projectToJson(current));
    current = projectFromJson(redo_.back());
    redo_.pop_back();
    return true;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
}

}  // namespace ynot
