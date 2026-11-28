// Ynot Drums: a built-in synthesized drum kit (no samples, so it is tiny and identical on every
// platform), and step-sequencer patterns that play it.
//
// Sounds follow the General MIDI drum map, so patterns also drive any third-party drum plugin.
// A pattern is an ordinary MIDI clip whose notes sit on a step grid; StepPattern remembers the
// grid (step size, swing, rows) so the step editor can show and edit it.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ynot/engine.hpp"
#include "ynot/model.hpp"

namespace ynot {

inline constexpr const char* kBuiltinDrums = "ynot.drums.basic";

struct DrumSound {
    int note;          // General MIDI drum note
    const char* name;  // "Kick", "Snare", ...
};
// The kit's sounds, in step-editor row order.
const std::vector<DrumSound>& drumKitSounds();
const char* drumName(int note);  // "Note 60" style fallback is up to the caller (returns nullptr)

std::unique_ptr<PluginProcessor> makeDrumKit(const Json& params, double sampleRate);

// ---- step patterns

// Tick position of `step` on a pattern grid, with swing applied to every second step
// (50 = straight, 66 = triplet shuffle, 75 = dotted).
int64_t stepTick(const StepPattern& s, int step);
int stepCount(const StepPattern& s, int64_t clipLengthTicks);

// New drum track ("Drums") with Ynot Drums and an empty pattern clip of `bars` bars at `startTick`.
// Returns the track id.
std::string addDrumTrack(Project& p, int64_t startTick, int bars = 1);
// New empty pattern clip on an existing instrument track. Returns the clip id.
std::string addPatternClip(Project& p, const std::string& trackId, int64_t startTick, int bars);

// Step editing. Velocity 0 = step off.
int stepVelocity(const MidiClip& c, int note, int step);
bool setStep(Project& p, const std::string& trackId, const std::string& clipId, int note, int step, int velocity);
bool setPatternSwing(Project& p, const std::string& trackId, const std::string& clipId, double swingPercent);
// Changes the grid (1/8, 1/16, 1/32, triplets...); steps keep their order from the start.
bool setPatternStepTicks(Project& p, const std::string& trackId, const std::string& clipId, int64_t stepTicks);
// Grows/shrinks the pattern; growing repeats the existing bars, so a 1-bar beat becomes 2 bars of it.
bool setPatternBars(Project& p, const std::string& trackId, const std::string& clipId, int bars);
bool clearPatternRow(Project& p, const std::string& trackId, const std::string& clipId, int note);

}  // namespace ynot
