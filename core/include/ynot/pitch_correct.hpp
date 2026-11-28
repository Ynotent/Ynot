// Ynot Tune: built-in real-time pitch correction for vocals and solo instruments.
//
// Detects the sung pitch (YIN) and moves it to the nearest note of the chosen key and scale
// with a pitch-synchronous delay-line shifter, which keeps the voice's character (formants)
// for normal correction amounts. Retune speed sets how fast notes are pulled in: 0 ms gives the
// hard, stepped "robot" effect, 20-60 ms natural correction, 100+ ms gentle.
// Original Ynot design; it reports its latency so the engine keeps the track in time.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ynot/engine.hpp"
#include "ynot/json.hpp"

namespace ynot {

inline constexpr const char* kBuiltinTune = "ynot.fx.tune";

struct ScaleInfo {
    const char* id;
    const char* name;
    std::vector<int> steps;  // semitones above the key
};
const std::vector<ScaleInfo>& tuneScales();  // chromatic, major, minor, harmonic minor, ...
const char* keyName(int key);                // 0 = C ... 11 = B

// Parameters (Json, also settable with setParameter using the same names and plain values):
//   key 0..11, scale (index into tuneScales()), speed (ms, 0..400), humanize (0..1),
//   flex (cents, 0..100: notes further than this from the scale are left alone), mix (0..1),
//   reference (Hz for A4, default 440).
std::unique_ptr<PluginProcessor> makePitchCorrector(const Json& params, double sampleRate);

// Analysis helpers (exposed for tests and the UI's pitch readout).
// Fundamental frequency of `samples` (mono), or 0 if not clearly pitched.
double detectPitchHz(const float* samples, int count, double sampleRate);
// Nearest note (MIDI number, fractional input) allowed by key/scale.
int nearestScaleNote(double midi, int key, int scale);

}  // namespace ynot
