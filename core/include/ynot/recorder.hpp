// Audio recording, shared by every platform.
//
// Threading model (identical on CoreAudio, ASIO/WASAPI, iOS and Oboe):
//   audio thread  -> Recorder::capture()  copies input into lock-free ring buffers
//                                         (no allocation, no locks, no file I/O)
//   drain thread  -> Recorder::drain()    moves ring data into streaming WAV files
//   message thread-> start() / finish() / commitTakes()
//
// Takes are written inside the project package (`.recording/`), then hashed and
// moved into the content-addressed media store, so a take is never uploaded or
// stored twice. If the drain thread falls behind, dropped frames are counted
// and reported to the user, never hidden.
#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "ynot/model.hpp"

namespace ynot {

// Single-producer / single-consumer float ring buffer.
class SpscFloatRing {
public:
    explicit SpscFloatRing(size_t capacity);
    size_t write(const float* data, size_t n);  // producer; returns samples written
    size_t read(float* data, size_t n);         // consumer; returns samples read
    size_t available() const;
    size_t capacity() const { return buf_.size() - 1; }

private:
    std::vector<float> buf_;
    std::atomic<size_t> head_{0}, tail_{0};
};

struct RecordTarget {
    std::string trackId;
    std::vector<int> inputChannels;  // 1 (mono) or 2 (stereo) physical input indices
};

struct RecordedTake {
    std::string trackId;
    std::string tempPath;    // WAV inside <package>/.recording/
    int64_t startFrame = 0;  // engine frame at which capture began
    int64_t frames = 0;
    int channels = 1;
    double sampleRate = 48000;
};

class Recorder {
public:
    struct Config {
        double sampleRate = 48000;
        int maxBlock = 512;
        std::vector<RecordTarget> targets;
        std::string packageDir;
        double ringSeconds = 4.0;  // headroom if the drain thread stalls
        // Input before this engine frame is ignored (count-in / pre-roll).
        int64_t startAtFrame = std::numeric_limits<int64_t>::min();
        // Input at or after this engine frame is ignored (punch-out).
        int64_t stopAtFrame = std::numeric_limits<int64_t>::max();
    };

    Recorder();
    ~Recorder();

    // Message thread. Allocates rings and opens files. Returns false with error().
    bool start(const Config& cfg);
    // Audio thread. `engineFrame` is the engine position at the start of this block.
    void capture(const float* const* inputs, int numInputs, int frames, int64_t engineFrame);
    // Drain thread (or any non-realtime thread). Returns false on disk error.
    bool drain();
    // Message thread. Stops capture, drains the remainder, closes files.
    std::vector<RecordedTake> finish();
    // Discards everything recorded since start().
    void cancel();

    bool isRecording() const { return recording_.load(); }
    int64_t droppedFrames() const { return dropped_.load(); }
    int64_t recordedFrames() const;
    std::string error() const { return error_; }

private:
    struct Lane;
    std::vector<std::unique_ptr<Lane>> lanes_;
    Config cfg_;
    std::atomic<bool> recording_{false};
    std::atomic<int64_t> startFrame_{-1};
    std::atomic<int64_t> dropped_{0};
    std::atomic<int64_t> captured_{0};
    std::string error_;
};

// Adds recorded takes to the project as clips on their tracks.
//  * Moves each take into the media store (content-addressed).
//  * Compensates input+output latency by offsetting into the take, so what was
//    played lines up with what the performer heard.
//  * New clips replace overlapping parts of existing clips (classic punch-in).
// Returns the ids of the new clips.
std::vector<std::string> commitTakes(Project& p, const std::string& packageDir,
                                     const std::vector<RecordedTake>& takes, int latencyFrames);

// Places `clip` on `track`, trimming/splitting/removing overlapped clips.
void placeClipReplacing(const Project& p, Track& track, AudioClip clip);

// Registers an existing WAV file as content-addressed media (moving it into media/).
const MediaRef& addMediaFile(Project& p, const std::string& packageDir, const std::string& wavPath,
                             double sampleRate, int channels, int64_t frames, const std::string& origin);

int64_t frameToTick(const Project& p, int64_t frame, double sampleRate);
int64_t tickToFrame(const Project& p, int64_t tick, double sampleRate);

}  // namespace ynot
