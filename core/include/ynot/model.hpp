// In-memory project model shared by every platform.
//
// Time is stored in musical ticks (kTicksPerQuarter per quarter note), never in
// samples, so a project recorded at 48 kHz on an iPhone opens unchanged on a
// 44.1 kHz or 96 kHz desktop interface. Audio media keeps its native rate and is
// resampled at playback.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ynot/json.hpp"
#include "ynot/platform.hpp"

namespace ynot {

constexpr int64_t kTicksPerQuarter = 960;

std::string newId();  // random 128-bit id, hex encoded

struct MediaRef {
    std::string id;           // stable id used by clips
    std::string sha256;       // content hash; media files are immutable blobs
    std::string fileName;     // relative to the package media/ folder
    double sampleRate = 48000;
    int channels = 2;
    int64_t frames = 0;
    std::string origin;       // "recording", "import", "freeze", "bounce"
};

struct AudioClip {
    std::string id;
    std::string mediaId;
    int64_t startTick = 0;
    int64_t lengthTick = 0;
    int64_t sourceOffsetFrames = 0;  // in the media's own sample rate
    double gainDb = 0.0;
};

struct MidiNote {
    int64_t tick = 0;  // relative to the clip start
    int64_t lengthTick = 0;
    int pitch = 60;
    int velocity = 100;
};

struct MidiClip {
    std::string id;
    int64_t startTick = 0;
    int64_t lengthTick = 0;
    std::vector<MidiNote> notes;
};

struct AutomationPoint {
    int64_t tick = 0;
    double value = 0.0;  // normalized 0..1 for plugin params; native units for track params
};

struct AutomationLane {
    // "track.gainDb", "track.pan", or "plugin:<pluginId>:<paramId>".
    std::string target;
    std::vector<AutomationPoint> points;  // sorted by tick
    double valueAt(int64_t tick, double fallback) const;
};

struct PluginInstance {
    std::string id;
    std::string name;
    std::string vendor;
    PluginFormat format = PluginFormat::Builtin;
    std::string uid;          // VST3 class id, AU type/subtype/manufacturer, builtin id
    std::string version;
    std::string stateBase64;  // opaque plugin chunk, carried untouched across devices
    Json params = Json::object();  // builtin parameters, or a portable mirror for UI
    bool bypassed = false;
    std::set<Platform> seenOn;  // platforms where this plugin was successfully loaded
};

struct FreezeInfo {
    std::string mediaId;          // rendered audio of the track (post-inserts, pre-fader)
    std::string sourceFingerprint;// fingerprint of the track when it was frozen
    Platform renderedOn = Platform::Linux;
    std::string renderedAt;       // ISO-8601
};

enum class TrackKind { Audio, Instrument };

struct Track {
    std::string id;
    std::string name;
    TrackKind kind = TrackKind::Audio;
    double order = 0;  // sort key (fractional, collaboration-friendly)
    double gainDb = 0.0;
    double pan = 0.0;  // -1..1
    bool mute = false;
    bool solo = false;
    bool armed = false;
    std::string color = "#7C5CFF";
    std::optional<PluginInstance> instrument;
    std::vector<PluginInstance> inserts;
    std::vector<AudioClip> audioClips;
    std::vector<MidiClip> midiClips;
    std::vector<AutomationLane> automation;
    std::optional<FreezeInfo> freeze;
    bool frozen = false;  // user intent: play the freeze render instead of live processing
    Json extras = Json::object();  // unknown/forward-compatible fields, preserved on save

    const AutomationLane* lane(const std::string& target) const;
};

struct Project {
    int formatVersion = 1;
    std::string id;
    std::string name;
    double tempoBpm = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    double sampleRateHint = 48000;  // last-used device rate; informative only
    std::vector<Track> tracks;
    std::vector<MediaRef> media;
    Json extras = Json::object();

    Track* findTrack(const std::string& id);
    const Track* findTrack(const std::string& id) const;
    const MediaRef* findMedia(const std::string& id) const;
    std::vector<const Track*> orderedTracks() const;

    double secondsPerTick() const { return 60.0 / tempoBpm / static_cast<double>(kTicksPerQuarter); }
    // Time signature in ticks. The beat is the denominator's note value (an eighth in 6/8);
    // tempo stays in quarter notes per minute on every platform.
    int64_t beatTicks() const {
        const int den = (timeSigDenominator == 2 || timeSigDenominator == 4 || timeSigDenominator == 8 ||
                         timeSigDenominator == 16) ? timeSigDenominator : 4;
        return kTicksPerQuarter * 4 / den;
    }
    int64_t barTicks() const { return beatTicks() * std::max(1, timeSigNumerator); }
    int64_t endTick() const;
};

}  // namespace ynot
