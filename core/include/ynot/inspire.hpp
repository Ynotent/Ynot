// "Inspired by": make a brand-new, original idea in the style of a reference song.
//
// Ynot listens to the reference for things nobody owns: tempo, key, the general chord
// movement, and feel (energy, brightness, how busy it is). It then writes NEW parts (chords,
// bass, melody, drums) from scratch, with its own seeded randomness. The reference audio is
// never copied into the song or the idea: it's analysed and discarded.
//
// Originality check: the new melody is compared with the reference's melody (transposition-
// and tempo-independent). If it's too close, Ynot writes another one, and the final score is
// shown to the user. This keeps ideas clearly the user's own; it is not legal advice, and
// anything that still sounds like the reference should be changed or cleared.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ynot/analysis.hpp"
#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

struct ReferenceProfile {
    double bpm = 90;
    KeyEstimate key;
    std::vector<std::string> chords;  // the reference's chord movement, one per bar (up to 4)
    double energy = 0.5;      // 0..1, how loud and dense
    double brightness = 0.5;  // 0..1, dark .. bright
    double busyness = 0.5;    // 0..1, notes/hits per beat
    std::vector<MidiNote> melody;  // rough top line, only for the originality check
};
ReferenceProfile analyzeReference(const AudioBuffer& reference);

struct InspiredIdea {
    uint32_t seed = 0;
    double bpm = 90;
    KeyEstimate key;
    int bars = 4;
    std::vector<std::string> chordNames;  // one per bar
    std::vector<MidiNote> chords, bass, melody;  // ticks from the idea's start
    std::vector<MidiNote> drums;  // on a 1/16 grid (unswung ticks); `swing` is applied when played
    double swing = 50;            // percent (50 = straight)
    double similarity = 0;  // 0 = nothing alike .. 1 = the same melody
    std::string description;
};

InspiredIdea generateIdea(const ReferenceProfile& ref, uint32_t seed, int bars = 4);
// Generates until the melody is clearly different from the reference (similarity below
// kMaxSimilarity), trying up to 24 seeds from `seed`; returns the least similar if none pass.
inline constexpr double kMaxSimilarity = 0.3;
InspiredIdea generateOriginalIdea(const ReferenceProfile& ref, uint32_t seed, int bars = 4);

// Transposition- and tempo-independent melody similarity, 0..1.
double melodySimilarity(const std::vector<MidiNote>& a, const std::vector<MidiNote>& b);

// Adds the idea as four editable tracks (Ynot Synth x3, Ynot Drums) at `atTick`. Returns the
// track ids. The song's tempo is left alone unless setTempo is true.
std::vector<std::string> addIdeaTracks(Project& p, const InspiredIdea& idea, int64_t atTick, bool setTempo);
// The idea as audio, exactly `bars` long (for the sampler, or a sound pack).
AudioBuffer renderIdea(const InspiredIdea& idea, double sampleRate, bool withDrums = true);

}  // namespace ynot
