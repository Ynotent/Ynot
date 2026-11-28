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
    // Time stretch (see stretch.hpp). Empty stretchSourceMediaId = not stretched; otherwise
    // mediaId is a render of the original audio and these fields point back to it.
    std::string stretchSourceMediaId;
    int64_t stretchSourceOffsetFrames = 0;  // original frame at the render's frame 0
    double stretchRatio = 1.0;              // length / original length
    std::string stretchMode;                // "mono", "poly", "varispeed"
    // Cycle-recording takes: where each pass starts in the media. sourceOffsetFrames is
    // takeOffsets[activeTake] plus any trim, so switching takes keeps trims and splits.
    std::vector<int64_t> takeOffsets;
    int activeTake = 0;
};

struct MidiNote {
    int64_t tick = 0;  // relative to the clip start
    int64_t lengthTick = 0;
    int pitch = 60;
    int velocity = 100;
};

// A drum/step pattern's grid. The notes in the clip are the truth (they play anywhere);
// this tells the step editor how to show them.
struct StepPattern {
    int64_t stepTicks = kTicksPerQuarter / 4;  // 1/16
    double swingPercent = 50.0;                // 50 = straight
    std::vector<int> rows;                     // note numbers, top to bottom
};

struct MidiClip {
    std::string id;
    int64_t startTick = 0;
    int64_t lengthTick = 0;
    std::vector<MidiNote> notes;
    std::optional<StepPattern> steps;  // set for step-sequencer patterns
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
    std::string tempoSignature;   // the song's tempo map when frozen (empty: older projects)
    Platform renderedOn = Platform::Linux;
    std::string renderedAt;       // ISO-8601
};

enum class TrackKind { Audio, Instrument };

// A tempo change: from `tick` on, the song runs at `bpm` (quarter notes per minute) until the
// next change. Changes are instant steps.
struct TempoChange {
    int64_t tick = 0;
    double bpm = 120.0;
};
constexpr double kMinBpm = 20.0, kMaxBpm = 400.0;

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
    double tempoBpm = 120.0;              // tempo at the start of the song
    std::vector<TempoChange> tempoChanges;  // later changes, sorted by tick (all > 0)
    struct Loop {
        int64_t startTick = 0, endTick = 0;
        bool enabled = false;
        bool valid() const { return endTick > startTick; }
    } loop;  // cycle region: playback repeats it; recording records a take per pass
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    double sampleRateHint = 48000;  // last-used device rate; informative only
    double masterGainDb = 0.0;      // master fader
    std::vector<Track> tracks;
    std::vector<MediaRef> media;
    Json extras = Json::object();

    Track* findTrack(const std::string& id);
    const Track* findTrack(const std::string& id) const;
    const MediaRef* findMedia(const std::string& id) const;
    std::vector<const Track*> orderedTracks() const;

    // Musical time <-> seconds, following the tempo map. Use these rather than assuming one tempo.
    double tempoAt(int64_t tick) const;
    double tickToSeconds(double tick) const;
    double secondsToTick(double seconds) const;
    std::string tempoSignature() const;  // changes whenever the tempo map does
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

// Precomputed tempo map for fast, allocation-free lookups (the audio thread uses one).
class TempoMap {
public:
    TempoMap() = default;
    explicit TempoMap(const Project& p);
    double tickToSeconds(double tick) const;
    double secondsToTick(double seconds) const;
    double bpmAt(double tick) const;
    int64_t tickToFrame(int64_t tick, double sampleRate) const;
    int64_t frameToTick(int64_t frame, double sampleRate) const;

private:
    struct Segment {
        double tick, seconds, secondsPerTick;
    };
    std::vector<Segment> segs_{{0.0, 0.0, 60.0 / 120.0 / kTicksPerQuarter}};
    const Segment& segForTick(double tick) const;
    const Segment& segForSeconds(double seconds) const;
};

}  // namespace ynot
