// Decodes audio files for import, using every decoder this platform offers:
//   everywhere: WAV, AIFF, FLAC, Ogg Vorbis, MP3
//   macOS/iOS:  + M4A/AAC/MP4/MOV/CAF (Core Audio — audio from video files too)
//   Windows:    + WMA/WMV/MP4/M4A (Windows Media)
// Decoded audio is stored in the project as lossless media, so a file imported on one
// platform plays on all of them afterwards.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "ynot/media.hpp"

namespace ynotapp {

struct DecodedAudio {
    bool ok = false;
    ynot::AudioBuffer audio;
    juce::String formatName;  // which decoder read it
    juce::String error;       // user-facing reason when !ok
};

DecodedAudio decodeAudioFile(const juce::File& file);

// File-chooser pattern, e.g. "*.wav;*.mp3;...".
juce::String importFilePatterns();
bool looksLikeImportableFile(const juce::File& f);

}  // namespace ynotapp
