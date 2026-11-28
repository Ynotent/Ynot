// Ynot's on-device music AI: listens to the song and tells you what's in it.
//
//   * Key detection (whole song, or one clip) from audio and MIDI.
//   * Chord detection along the timeline.
//   * Tempo detection of a recording or sample.
//   * Audio to MIDI: turns a sung or played melody (one note at a time) into MIDI notes.
//
// Everything runs on the device with ordinary DSP (no network, no model download), so it
// works the same on iOS, Android, Windows and macOS and offline. Results are suggestions the
// user applies, never silent changes to the song.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ynot/engine.hpp"
#include "ynot/media.hpp"
#include "ynot/model.hpp"

namespace ynot {

// Energy per pitch class (C, C#, ... B).
using Chroma = std::array<double, 12>;

Chroma chromaFromAudio(const AudioBuffer& a, int64_t fromFrame = 0, int64_t frames = -1);
Chroma chromaFromNotes(const std::vector<MidiNote>& notes, int64_t fromTick, int64_t toTick);

struct KeyEstimate {
    int tonic = 0;        // 0 = C ... 11 = B
    bool minor = false;
    double confidence = 0;  // 0..1 (how clearly this key beats the next best)
    std::string name() const;  // "A minor"
    // Ynot Tune settings for this key (key, scale index in tuneScales()).
    int tuneKey() const { return tonic; }
    int tuneScale() const { return minor ? 2 : 1; }
};
KeyEstimate detectKey(const Chroma& c);

struct ChordSpan {
    int64_t tick = 0, lengthTick = 0;
    std::string name;  // "C", "Am", "G7", "Fmaj7", "Bdim", "Dsus4"; "N" = no chord
};
// Best chord for one window of pitch-class energy, or "N" when it's too quiet/unclear.
std::string chordName(const Chroma& c);

// Song analysis (unmuted tracks; drum and chopped-sampler tracks are left out because
// percussion has no key). Audio comes from the MediaProvider.
Chroma songChroma(const Project& p, MediaProvider& media, int64_t fromTick, int64_t toTick);
KeyEstimate detectSongKey(const Project& p, MediaProvider& media);
// One chord per `stepTicks` (default: half a bar), merged where neighbours agree.
std::vector<ChordSpan> detectChords(const Project& p, MediaProvider& media, int64_t stepTicks = 0);

struct TempoEstimate {
    double bpm = 0;  // 0 = no steady beat found
    double confidence = 0;
};
TempoEstimate detectTempo(const AudioBuffer& a);

// Monophonic transcription. Notes are relative to the start of `a` in ticks, using the song's
// tempo map from `startTick` (where the audio sits in the song).
std::vector<MidiNote> transcribeMelody(const AudioBuffer& a, const Project& p, int64_t startTick);
// Turns an audio clip into a MIDI clip on a new instrument track (Ynot Synth) at the same
// place. Returns the new track id, or nothing if no notes were found.
std::optional<std::string> audioClipToMidi(Project& p, MediaProvider& media, const std::string& trackId,
                                           const std::string& clipId);

// Whether a track is percussion (left out of key/chord analysis).
bool isPercussionTrack(const Track& t);

}  // namespace ynot
