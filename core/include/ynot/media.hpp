// Media utilities: content hashing and a portable WAV reader/writer.
//
// Recordings are stored as immutable, content-addressed files so cloud sync
// only uploads a take once, and two devices can never disagree about what
// "the same" audio file contains.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ynot {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string finalHex();  // call once

private:
    uint32_t h_[8];
    uint8_t buf_[64] = {};
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

std::string sha256Hex(const void* data, size_t len);
std::string sha256File(const std::string& path);  // streaming, for large recordings
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

// Streams 32-bit float WAV to disk (recording). Not realtime-safe: call from
// the recorder's drain thread, never the audio callback.
class WavFileWriter {
public:
    ~WavFileWriter();
    bool open(const std::string& path, double sampleRate, int channels);
    bool write(const float* interleaved, int64_t frames);  // false on I/O error or size limit
    void close();
    int64_t frames() const { return frames_; }

private:
    std::ofstream out_;
    int channels_ = 2;
    int64_t frames_ = 0;
};

std::vector<uint8_t> readFileBytes(const std::string& path);
void writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes);

}  // namespace ynot
