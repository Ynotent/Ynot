// Time stretch: change an audio clip's length without changing its pitch.
//
// Modes (stored per clip, identical on every platform):
//   Mono      - one voice at a time: vocals, bass, solo instruments. WSOLA: the waveform
//               is cut into short grains that are re-spliced where they line up, so a
//               single pitch stays natural with no phasiness.
//   Poly      - chords, pads, full mixes, drum loops. Phase vocoder with phase locking
//               and transient phase reset: every partial is shifted in time separately,
//               so all notes of a chord stay in tune.
//   Varispeed - tape-style speed change: pitch follows speed (slower = lower).
//
// Stretching is non-destructive. The clip keeps a reference to its original audio and
// every new stretch is rendered again from that original, so repeated stretches never
// lose quality, and Reset brings the original back exactly. The rendered result is
// ordinary media, so other devices play it without re-rendering.
#pragma once

#include <optional>
#include <string>

#include "ynot/engine.hpp"
#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

enum class StretchMode { Mono, Poly, Varispeed };

constexpr double kMinStretchRatio = 0.25;  // 4x faster
constexpr double kMaxStretchRatio = 4.0;   // 4x slower

const char* stretchModeId(StretchMode m);    // "mono", "poly", "varispeed" (stored in projects)
const char* stretchModeName(StretchMode m);  // "Mono", "Poly", "Varispeed"
std::optional<StretchMode> stretchModeFromId(const std::string& id);

// Pure DSP. ratio = output length / input length (2.0 = twice as long, half speed).
// The output has exactly round(frames * ratio) frames, the input's rate and channels.
AudioBuffer timeStretch(const AudioBuffer& in, double ratio, StretchMode mode);

// Clip editing, split so the slow render can run on a background thread:
//   auto job = prepareStretch(...);          // message thread: copies the original audio
//   auto out = renderStretch(*job);          // any thread
//   commitStretch(project, dir, *job, out);  // message thread
struct StretchJob {
    std::string trackId, clipId;
    std::string sourceMediaId;        // the clip's original (unstretched) media
    int64_t sourceOffsetFrames = 0;   // where the clip's audio starts in the original
    AudioBuffer source;               // the original audio under the clip
    double ratio = 1.0;
    StretchMode mode = StretchMode::Poly;
    bool reset() const;               // ratio 1: restore the original, nothing to render
};

// The ratio relative to the clip's original audio (1.0 when not stretched).
double clipStretchRatio(const AudioClip& c);
std::optional<StretchMode> clipStretchMode(const AudioClip& c);

// ratio is relative to the clip's *original* length (so 1.0 = reset). Returns nullopt
// with `error` set if the clip can't be stretched.
std::optional<StretchJob> prepareStretch(const Project& p, MediaProvider& media, const std::string& trackId,
                                         const std::string& clipId, double ratio, StretchMode mode,
                                         std::string* error = nullptr);
AudioBuffer renderStretch(const StretchJob& job);
// Fails (returns false) if the clip was deleted meanwhile.
bool commitStretch(Project& p, const std::string& packageDir, const StretchJob& job, const AudioBuffer& rendered);

// All three steps at once (tests, small clips).
bool stretchClip(Project& p, const std::string& packageDir, MediaProvider& media, const std::string& trackId,
                 const std::string& clipId, double ratio, StretchMode mode, std::string* error = nullptr);

// Ratio that makes a loop recorded at `sourceBpm` play in time at the song's tempo at `atTick`.
double ratioForTempo(const Project& p, double sourceBpm, int64_t atTick = 0);
// Ratio that makes the clip's original length a whole number of bars (nearest, at least 1).
double ratioToFitBars(const Project& p, const AudioClip& c);

}  // namespace ynot
