#include "ynot/midi_recorder.hpp"

#include <algorithm>
#include <map>

#include "ynot/edit.hpp"
#include "ynot/recorder.hpp"

namespace ynot {

void MidiTakeRecorder::start(int64_t startAtFrame, int64_t stopAtFrame, size_t capacity) {
    recording_ = false;
    events_.assign(capacity, TimedMidiEvent{0, false, 0, 0});
    count_ = 0;
    dropped_ = 0;
    startAt_ = startAtFrame;
    stopAt_ = stopAtFrame;
    recording_.store(true, std::memory_order_release);
}

void MidiTakeRecorder::capture(const MidiEvent* events, int numEvents, int64_t blockStartFrame) {
    if (!recording_.load(std::memory_order_acquire)) return;
    size_t n = count_.load(std::memory_order_relaxed);
    for (int i = 0; i < numEvents; ++i) {
        const int64_t frame = blockStartFrame + events[i].frameOffset;
        // Note-offs during the count-in still matter if the note continues into the take,
        // but a note that starts in the count-in isn't part of it, so drop both kinds there.
        // Note-offs after punch-out are kept so notes played inside the range get their ends;
        // buildMidiClip trims them to the clip.
        if (frame < startAt_ || (frame >= stopAt_ && events[i].noteOn)) continue;
        if (n >= events_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        events_[n++] = {frame, events[i].noteOn && events[i].velocity > 0, events[i].pitch, events[i].velocity};
    }
    count_.store(n, std::memory_order_release);
}

std::vector<TimedMidiEvent> MidiTakeRecorder::finish() {
    recording_ = false;
    std::vector<TimedMidiEvent> out(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(count_.load()));
    events_.clear();
    events_.shrink_to_fit();
    count_ = 0;
    return out;
}

std::optional<MidiClip> buildMidiClip(const Project& p, const std::vector<TimedMidiEvent>& events,
                                      int64_t startFrame, int64_t stopFrame, double sampleRate,
                                      const QuantizeSettings* quantize) {
    const int64_t clipStart = frameToTick(p, startFrame, sampleRate);
    const int64_t clipEnd = std::max(clipStart + kMinClipTicks, frameToTick(p, stopFrame, sampleRate));
    MidiClip clip;
    clip.id = newId();
    clip.startTick = clipStart;
    clip.lengthTick = clipEnd - clipStart;

    std::map<int, std::pair<int64_t, int>> held;  // pitch -> (on tick, velocity)
    auto close = [&](int pitch, int64_t offTick) {
        auto it = held.find(pitch);
        if (it == held.end()) return;
        const int64_t on = it->second.first;
        const int64_t length = std::max<int64_t>(1, offTick - on);
        if (on < clipEnd)
            clip.notes.push_back({on - clipStart, std::min(length, clipEnd - on), pitch, it->second.second});
        held.erase(it);
    };
    std::vector<TimedMidiEvent> sorted = events;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const TimedMidiEvent& a, const TimedMidiEvent& b) { return a.frame < b.frame; });
    for (const auto& e : sorted) {
        const int64_t tick = frameToTick(p, e.frame, sampleRate);
        if (e.noteOn) {
            close(e.pitch, tick);  // re-struck note: end the previous one here
            held[e.pitch] = {tick, std::clamp(e.velocity, 1, 127)};
        } else {
            close(e.pitch, tick);  // note-off without a note-on (held from before) is ignored
        }
    }
    while (!held.empty()) close(held.begin()->first, clipEnd);  // still held at stop
    if (clip.notes.empty()) return std::nullopt;
    if (quantize) quantizeClip(clip, *quantize);
    std::sort(clip.notes.begin(), clip.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
    });
    return clip;
}

std::optional<MidiClip> buildCycleMidiClip(const Project& p, const std::vector<TimedMidiEvent>& events,
                                           int64_t loopStartFrame, int64_t loopEndFrame, int64_t stopFrame,
                                           double sampleRate, const QuantizeSettings* quantize) {
    const int64_t len = loopEndFrame - loopStartFrame;
    if (len <= 0) return buildMidiClip(p, events, loopStartFrame, stopFrame, sampleRate, quantize);
    std::vector<TimedMidiEvent> sorted = events;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const TimedMidiEvent& a, const TimedMidiEvent& b) { return a.frame < b.frame; });
    // Pair notes on the continuous clock, then fold each note into the loop.
    std::vector<TimedMidiEvent> folded;
    std::map<int, std::pair<int64_t, int>> held;  // pitch -> (on frame, velocity)
    auto addNote = [&](int pitch, int64_t on, int64_t off, int velocity) {
        if (on < loopStartFrame) return;  // played during the count-in
        const int64_t at = loopStartFrame + (on - loopStartFrame) % len;
        folded.push_back({at, true, pitch, velocity});
        folded.push_back({std::min(at + std::max<int64_t>(1, off - on), loopEndFrame), false, pitch, 0});
    };
    for (const auto& e : sorted) {
        auto it = held.find(e.pitch);
        if (it != held.end()) {  // note-off, or a re-struck note ending the previous one
            addNote(e.pitch, it->second.first, e.frame, it->second.second);
            held.erase(it);
        }
        if (e.noteOn) held[e.pitch] = {e.frame, e.velocity};
    }
    for (const auto& [pitch, h] : held) addNote(pitch, h.first, stopFrame, h.second);  // still held at stop
    // Same frame: note-offs first, so a note ending where another starts doesn't cut it.
    std::stable_sort(folded.begin(), folded.end(), [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        return !a.noteOn && b.noteOn;
    });
    return buildMidiClip(p, folded, loopStartFrame, loopEndFrame, sampleRate, quantize);
}

}  // namespace ynot
