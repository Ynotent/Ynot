// Arrangement editing operations, shared by every platform's timeline UI.
//
// Every edit is non-destructive: audio media is never modified (trims move
// clip windows and source offsets) and MIDI notes hidden by a trim are kept,
// so extending a clip back out restores them. The same functions run on a
// Mac with a mouse and an iPhone with touch, so an edit always means the same
// thing everywhere.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ynot/json.hpp"
#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

constexpr int64_t kMinClipTicks = kTicksPerQuarter / 16;  // 1/64 note

int64_t snapTick(int64_t tick, int64_t grid);  // grid <= 0 means no snapping

// Longest a clip can be, given its media and source offset (audio only).
int64_t maxAudioClipTicks(const Project& p, const AudioClip& c);

// Moves a clip to `newStartTick`, optionally onto another track of the same
// kind (audio clips -> audio tracks, MIDI clips -> instrument tracks). Audio
// clips replace what they land on, like a recording would.
bool moveClip(Project& p, const std::string& trackId, const std::string& clipId, int64_t newStartTick,
              const std::string& toTrackId = {});
// Moves the clip's left edge; the right edge stays put.
bool trimClipStart(Project& p, const std::string& trackId, const std::string& clipId, int64_t newStartTick);
// Moves the clip's right edge.
bool trimClipEnd(Project& p, const std::string& trackId, const std::string& clipId, int64_t newEndTick);
// Splits at `atTick`; returns the id of the new right-hand clip.
std::optional<std::string> splitClip(Project& p, const std::string& trackId, const std::string& clipId,
                                     int64_t atTick);
bool deleteClip(Project& p, const std::string& trackId, const std::string& clipId);

// MIDI punch-in: removes notes that start inside [fromTick, toTick) from the track's
// MIDI clips (notes outside the range are untouched). Returns how many were removed.
int eraseNotesInRange(Track& track, int64_t fromTick, int64_t toTick);

// ---- Tracks and plugins ---------------------------------------------------------------
// Adds a track at the end; instrument tracks start with the builtin Ynot Synth so they
// make sound on every platform. Returns the new track's id.
std::string addTrack(Project& p, TrackKind kind, const std::string& name = {});
bool removeTrack(Project& p, const std::string& trackId);
bool renameTrack(Project& p, const std::string& trackId, const std::string& name);
// Inserts an effect at `index` (-1 = end of the chain). Returns the plugin's id, empty on failure.
std::string addInsert(Project& p, const std::string& trackId, PluginInstance plugin, int index = -1);
// Replaces the instrument of an instrument track. Returns false for audio tracks.
bool setInstrument(Project& p, const std::string& trackId, PluginInstance plugin);
// Removes an instrument or an insert by plugin id.
bool removePlugin(Project& p, const std::string& trackId, const std::string& pluginId);
bool setPluginBypassed(Project& p, const std::string& trackId, const std::string& pluginId, bool bypassed);
// Moves an insert within the chain.
bool moveInsert(Project& p, const std::string& trackId, const std::string& pluginId, int newIndex);
// Imports decoded audio (from any file format the platform can read) as a clip on an
// audio track at `startTick`. The audio is stored in the project as content-addressed
// media, so it plays on every platform afterwards — e.g. a WMV imported on Windows
// opens on a Mac. Returns the new clip id, or empty if the track isn't an audio track.
std::string importAudioClip(Project& p, const std::string& packageDir, const std::string& trackId,
                            const AudioBuffer& audio, int64_t startTick);

// Several clips at once (mouse selection). A clip is identified by (trackId, clipId).
struct ClipRef {
    std::string trackId, clipId;
    bool operator==(const ClipRef& o) const { return trackId == o.trackId && clipId == o.clipId; }
};
// Moves every listed clip by the same number of ticks (they keep their spacing); a move that
// would push a clip before the song start is limited so the earliest clip lands at 0.
// Audio clips cover what they land on, as when moving one clip.
bool moveClips(Project& p, const std::vector<ClipRef>& clips, int64_t deltaTicks);
bool deleteClips(Project& p, const std::vector<ClipRef>& clips);
// Clipboard: copies of clips, remembered with their tracks.
struct ClipboardItem {
    std::string trackId;
    std::optional<AudioClip> audio;
    std::optional<MidiClip> midi;
};
std::vector<ClipboardItem> copyClips(const Project& p, const std::vector<ClipRef>& clips);
// Pastes so the earliest copied clip starts at `atTick`, on the same tracks (skipping tracks that
// no longer exist). Returns the new clips.
std::vector<ClipRef> pasteClips(Project& p, const std::vector<ClipboardItem>& items, int64_t atTick);
// Copies the clips to right after the last of them. Returns the new clips.
std::vector<ClipRef> duplicateClips(Project& p, const std::vector<ClipRef>& clips);
// Every clip whose span overlaps [fromTick, toTick) on the listed tracks.
std::vector<ClipRef> clipsInRange(const Project& p, const std::vector<std::string>& trackIds, int64_t fromTick,
                                  int64_t toTick);

// Cycle-recording takes: make take `index` the one that plays (keeps the clip's trims).
bool selectTake(Project& p, const std::string& trackId, const std::string& clipId, int index);

// Tempo. Clips stay on their bars and beats. Audio clips keep their real duration (a tempo
// change doesn't speed audio up; time stretch does), so their length in beats adjusts.
void setTempo(Project& p, double bpm);                         // tempo at the start of the song
bool setTempoChange(Project& p, int64_t tick, double bpm);     // add, or replace the one at `tick`
bool removeTempoChange(Project& p, int64_t tick);

// The colours new tracks cycle through (matches design/tokens.json trackPalette).
const std::vector<std::string>& trackPalette();

// Clip geometry independent of kind, for UIs.
struct ClipSpan {
    std::string trackId, clipId;
    bool audio;
    int64_t startTick, lengthTick;
};
std::optional<ClipSpan> findClip(const Project& p, const std::string& trackId, const std::string& clipId);

// Undo/redo by project snapshots (the JSON document, no audio), so any edit —
// including ones made by future features — is undoable without per-edit code.
class History {
public:
    explicit History(size_t limit = 200) : limit_(limit) {}
    void record(const Project& before);  // call before applying an edit
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo(Project& current);
    bool redo(Project& current);
    void clear();

private:
    size_t limit_;
    std::vector<Json> undo_, redo_;
};

}  // namespace ynot
