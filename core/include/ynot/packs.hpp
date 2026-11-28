// Ynot Sound Packs: one-shots, drum breaks and melodic loops made by Ynot itself (synthesised
// and generated, no recordings), so they're original and free to use in any release. They're
// generated on the device, identical on every platform, and cost no download.
#pragma once

#include <string>
#include <vector>

#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

struct PackSound {
    std::string id;    // stable: "drums/kick", "breaks/lofi-85-a", ...
    std::string pack;  // "Ynot Drum Hits", "Ynot Breaks", "Ynot Loops"
    std::string name;
    std::string kind;  // "one-shot" or "loop"
    double bpm = 0;    // loops
    std::string key;   // loops with pitch, e.g. "A minor"
    int bars = 0;
};

const std::vector<PackSound>& packSounds();
const PackSound* findPackSound(const std::string& id);
AudioBuffer renderPackSound(const std::string& id, double sampleRate);

inline constexpr const char* kPackLicense = "Ynot Sound Pack: royalty-free, cleared for any use";

// Adds the sound to the song as media (origin "pack", credited royalty-free) and returns its id.
std::string addPackMedia(Project& p, const std::string& packageDir, const std::string& soundId, double sampleRate);
// A new Ynot Sampler track playing the sound (loops are chopped onto pads). Returns the track id.
std::string addPackSoundToSampler(Project& p, const std::string& packageDir, const std::string& soundId, double sampleRate);
// The sound as a clip on an audio track (new track if trackId is empty). Returns the clip id.
std::string addPackSoundAsClip(Project& p, const std::string& packageDir, const std::string& soundId,
                               const std::string& trackId, int64_t startTick, double sampleRate);

}  // namespace ynot
