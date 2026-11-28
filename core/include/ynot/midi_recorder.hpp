// MIDI recording, shared by every platform.
//
// The audio thread appends the live note events it plays (from a controller or
// an on-screen keyboard) into preallocated storage; after recording stops,
// buildMidiClip() turns them into a MIDI clip. Timing is the engine frame at
// which each event was rendered, so the recorded notes land exactly where the
// player heard them, on any platform and buffer size.
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "ynot/engine.hpp"
#include "ynot/model.hpp"
#include "ynot/quantize.hpp"

namespace ynot {

struct TimedMidiEvent {
    int64_t frame;  // engine frame
    bool noteOn;
    int pitch;
    int velocity;
};

class MidiTakeRecorder {
public:
    // Message thread. Only events in [startAtFrame, stopAtFrame) are kept (count-in / punch).
    void start(int64_t startAtFrame, int64_t stopAtFrame = std::numeric_limits<int64_t>::max(),
               size_t capacity = 1 << 16);
    // Audio thread: events with offsets relative to `blockStartFrame`. No allocation.
    void capture(const MidiEvent* events, int numEvents, int64_t blockStartFrame);
    // Message thread, after the audio thread has stopped calling capture().
    std::vector<TimedMidiEvent> finish();
    bool isRecording() const { return recording_.load(); }
    int64_t droppedEvents() const { return dropped_.load(); }
    int64_t startFrame() const { return startAt_; }

private:
    std::vector<TimedMidiEvent> events_;
    std::atomic<size_t> count_{0};
    std::atomic<bool> recording_{false};
    std::atomic<int64_t> dropped_{0};
    int64_t startAt_ = 0;
    int64_t stopAt_ = std::numeric_limits<int64_t>::max();
};

// Pairs note-ons with note-offs and builds a clip spanning [startFrame, stopFrame).
// Notes still held at stop end at the stop point. With `quantize`, notes are
// quantized as they're recorded (grid, swing, strength; see quantize.hpp).
// Returns nullopt if no notes were played.
std::optional<MidiClip> buildMidiClip(const Project& p, const std::vector<TimedMidiEvent>& events,
                                      int64_t startFrame, int64_t stopFrame, double sampleRate,
                                      const QuantizeSettings* quantize = nullptr);

// Cycle recording: `events` are timed on a continuous clock that began before the loop and
// kept running across loop jumps (pass k starts at loopStartFrame + k * loop length).
// Notes from every pass are merged into one clip covering the loop; notes held over the
// loop end are cut there, and notes played before the loop start (count-in) are dropped.
std::optional<MidiClip> buildCycleMidiClip(const Project& p, const std::vector<TimedMidiEvent>& events,
                                           int64_t loopStartFrame, int64_t loopEndFrame, int64_t stopFrame,
                                           double sampleRate, const QuantizeSettings* quantize = nullptr);

}  // namespace ynot
