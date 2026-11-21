// Quantize and swing, shared by every platform.
//
// Grid: straight notes (1/1 ... 1/32) or triplets (1/4T, 1/8T, 1/16T).
// Swing: classic drum-machine swing. Notes are taken in pairs of grid steps and
// the second step of each pair is moved to `swingPercent` of the pair: 50% is
// straight, 66% is a triplet shuffle, 75% is a dotted feel. This is the scheme
// popularised by hardware drum machines and samplers, whose well-known settings
// (54/58/62/66/71/75%) are offered as presets.
// Strength: 100% snaps exactly; lower values move notes only part of the way,
// keeping some of the player's feel.
//
// The grid is aligned to the song (tick 0), not to the clip, so a clip that
// starts off the grid still quantizes onto the song's beats.
#pragma once

#include <string>
#include <vector>

#include "ynot/model.hpp"

namespace ynot {

struct QuantizeSettings {
    int64_t gridTicks = kTicksPerQuarter / 4;  // 1/16
    double swingPercent = 50.0;                // 50..75; ignored for triplet grids
    double strength = 1.0;                     // 0..1
    bool quantizeLengths = false;              // also snap note ends to the grid
};

struct QuantizePreset {
    std::string id;           // stable id for settings/sync
    std::string name;         // shown in menus
    std::string description;  // one line for tooltips/help
    QuantizeSettings settings;
};

// Grid values, in ticks.
inline constexpr int64_t kGrid1_1 = kTicksPerQuarter * 4;
inline constexpr int64_t kGrid1_2 = kTicksPerQuarter * 2;
inline constexpr int64_t kGrid1_4 = kTicksPerQuarter;
inline constexpr int64_t kGrid1_8 = kTicksPerQuarter / 2;
inline constexpr int64_t kGrid1_16 = kTicksPerQuarter / 4;
inline constexpr int64_t kGrid1_32 = kTicksPerQuarter / 8;
inline constexpr int64_t kGrid1_4T = kTicksPerQuarter * 2 / 3;
inline constexpr int64_t kGrid1_8T = kTicksPerQuarter / 3;
inline constexpr int64_t kGrid1_16T = kTicksPerQuarter / 6;

bool isTripletGrid(int64_t gridTicks);
std::string gridName(int64_t gridTicks);  // "1/16", "1/8T", ...

// Where `tick` (song position) lands under these settings.
int64_t quantizeTick(int64_t tick, const QuantizeSettings& q);

// Quantizes every visible note of a clip in place. Note lengths are kept unless
// quantizeLengths is set. Returns how many notes moved.
int quantizeClip(MidiClip& clip, const QuantizeSettings& q);

// Built-in grids and swing feels.
const std::vector<QuantizePreset>& quantizePresets();
const QuantizePreset* findQuantizePreset(const std::string& id);

// Common time signatures offered in the UI (numerator, denominator).
const std::vector<std::pair<int, int>>& commonTimeSignatures();

}  // namespace ynot
