// Media utilities: content hashing and a portable WAV reader/writer.
//
// Recordings are stored as immutable, content-addressed files so cloud sync
// only uploads a take once, and two devices can never disagree about what
// "the same" audio file contains.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ynot {

std::string sha256Hex(const void* data, size_t len);
std::string sha256Hex(const std::string& s);

// Interleaved 32-bit float audio.
struct AudioBuffer {
    double sampleRate = 48000;
    int channels = 2;
    std::vector<float> samples;  // interleaved
    int64_t frames() const { return channels ? static_cast<int64_t>(samples.size()) / channels : 0; }
};

// Writes 32-bit float WAV (lossless for anything the engine produces).
std::vector<uint8_t> encodeWavFloat32(const AudioBuffer& buf);
// Reads PCM 16/24/32-bit integer and 32-bit float WAV.
AudioBuffer decodeWav(const std::vector<uint8_t>& bytes);

std::vector<uint8_t> readFileBytes(const std::string& path);
void writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes);

}  // namespace ynot
