// Ynot Sampler: a built-in sample player that runs on every platform.
//
// Two modes:
//   * Chop: the sample is cut into slices, one per pad/key from `baseNote` up (36 = the first
//     pad on most controllers, e.g. Akai MPK). Slices play to their end (one-shot); hitting the
//     same pad again restarts it.
//   * Keys: the whole sample plays chromatically across the keyboard from `rootNote`, held
//     while the key is down (with release), sustain pedal and pitch bend.
//
// The sample lives in the project as ordinary content-addressed media (params "mediaId"), so it
// syncs and opens on every device like a recording.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ynot/engine.hpp"
#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

inline constexpr const char* kBuiltinSampler = "ynot.sampler";
inline constexpr int kMaxSlices = 64;

// `sample` may be null (no sample loaded yet): the sampler is then silent. The buffer must
// outlive the processor (the engine's MediaProvider keeps it).
std::unique_ptr<PluginProcessor> makeSampler(const Json& params, const AudioBuffer* sample, double sampleRate);

// ---- slicing (frames are in the sample's own sample rate)

// Finds where new sounds start (drum hits, notes, words) and returns up to `maxSlices` slice
// starts, always including 0, sorted. Strongest onsets win when there are more.
std::vector<int64_t> detectSlices(const AudioBuffer& sample, int maxSlices = 16);
std::vector<int64_t> equalSlices(int64_t frames, int count);
// Slice starts stored in a sampler's params (sorted, 0 first).
std::vector<int64_t> samplerSlices(const PluginInstance& sampler);

// New instrument track playing `sample` in Ynot Sampler (chop mode, auto-sliced to up to 16
// pads). The sample is stored as project media. Returns the track id.
std::string addSamplerTrack(Project& p, const std::string& packageDir, const AudioBuffer& sample,
                            const std::string& name);
// Puts a new sample into an existing sampler track's sampler (re-sliced automatically).
bool loadSample(Project& p, const std::string& packageDir, const std::string& trackId, const AudioBuffer& sample);
// The audio under an audio clip (as it plays), e.g. to send a recording to the sampler.
AudioBuffer clipAudio(const Project& p, MediaProvider& media, const std::string& trackId, const std::string& clipId);

// Slice edits on a sampler track. Returns false if the track has no sampler.
bool setSlices(Project& p, const std::string& trackId, std::vector<int64_t> starts);
bool addSlice(Project& p, const std::string& trackId, int64_t frame);
bool removeSlice(Project& p, const std::string& trackId, int index);  // index 0 can't be removed
bool moveSlice(Project& p, const std::string& trackId, int index, int64_t frame);

PluginInstance* samplerOf(Project& p, const std::string& trackId);
const PluginInstance* samplerOf(const Project& p, const std::string& trackId);

}  // namespace ynot
