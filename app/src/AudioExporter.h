// Export ("bounce"): renders the song, or the loop, offline through the same engine the
// app plays with (plugins, automation, mixer) and writes an audio file.
//   everywhere:     WAV (16-bit dithered / 24-bit / 32-bit float), FLAC, Ogg Vorbis
//   macOS / iOS:    + M4A (AAC, Apple's encoder)
//   Windows:        + M4A (AAC) and MP3 (Windows' encoders)
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <memory>

#include "ynot/ynot.hpp"

namespace ynotapp {

enum class ExportFormat { Wav, Flac, Ogg, M4a, Mp3 };

struct ExportSettings {
    ExportFormat format = ExportFormat::Wav;
    int bitDepth = 24;          // WAV: 16, 24, 32 (float). FLAC: 16, 24.
    int bitrateKbps = 256;      // Ogg, M4A, MP3
    double sampleRate = 48000;  // lossy formats use 44.1 or 48 kHz
    bool loopOnly = false;      // just the loop range, exact length (for loops/samples)
    bool normalize = false;     // peak at -1 dBFS
};

std::vector<ExportFormat> availableExportFormats();  // on this platform
juce::String formatName(ExportFormat f);             // "WAV", "M4A (AAC)", ...
juce::String formatExtension(ExportFormat f);        // ".wav", ...
bool isLossy(ExportFormat f);
// Lossy encoders only take some rates: the nearest supported one.
double exportRateFor(const ExportSettings& s);

// Step 1 (message thread): build the render (plugins load on the message thread).
struct ExportJob {
    std::unique_ptr<ynot::Engine> engine;
    ExportSettings settings;
    double rate = 48000;
    int64_t startFrame = 0, endFrame = 0;
    bool autoTail = true;  // keep rendering until reverbs and delays have died away (max 10 s)
    std::vector<ynot::EngineIssue> issues;
};
ExportJob prepareExport(const ynot::Project& p, ynot::MediaProvider& media, ynot::PluginHost& host,
                        const ExportSettings& s);

// Step 2 (any thread): render and encode. `progress` gets 0..1; return false from it to cancel.
// Returns an error message, or empty on success.
juce::String runExport(ExportJob& job, const juce::File& file, const std::function<bool(double)>& progress = {});

// Both steps, on the calling thread (tests).
juce::String exportSong(const ynot::Project& p, ynot::MediaProvider& media, ynot::PluginHost& host,
                        const ExportSettings& s, const juce::File& file);

}  // namespace ynotapp
