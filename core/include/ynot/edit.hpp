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
