#include "MidiClock.h"

#include <algorithm>

namespace ynotapp {

MidiClockSender::MidiClockSender() : juce::Thread("Ynot MIDI clock") { startThread(juce::Thread::Priority::high); }

MidiClockSender::~MidiClockSender() { stopThread(500); }

void MidiClockSender::setEnabled(bool on) {
    {
        const juce::ScopedLock lock(outputsLock_);
        outputs_.clear();
        if (on)
            for (const auto& d : juce::MidiOutput::getAvailableDevices())
                if (auto out = juce::MidiOutput::openDevice(d.identifier)) outputs_.push_back(std::move(out));
    }
    enabled_ = on;
}

void MidiClockSender::push(double timeMs, std::initializer_list<juce::uint8> bytes) {
    int s1, n1, s2, n2;
    fifo_.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 + n2 < 1) return;  // full: drop (the sender thread has stalled)
    Item& it = items_[static_cast<size_t>(n1 > 0 ? s1 : s2)];
    it.timeMs = timeMs;
    it.size = 0;
    for (auto b : bytes) it.bytes[static_cast<size_t>(it.size++)] = b;
    fifo_.finishedWrite(1);
}

void MidiClockSender::processBlock(bool running, double bpm, double sampleRate, int frames, double positionTicks,
                                   double ppq) {
    if (!enabled_.load(std::memory_order_relaxed) || bpm <= 0 || sampleRate <= 0) {
        wasRunning_ = false;
        return;
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (running && !wasRunning_) {
        // Song Position Pointer (in 16th notes) then Start, so devices begin in the right place.
        const auto sixteenths = static_cast<int>(std::max(0.0, positionTicks) / (ppq / 4.0));
        push(now, {0xF2, static_cast<juce::uint8>(sixteenths & 0x7F), static_cast<juce::uint8>((sixteenths >> 7) & 0x7F)});
        push(now, {static_cast<juce::uint8>(positionTicks <= 0 ? 0xFA : 0xFB)});  // Start / Continue
        samplesToNextPulse_ = 0;
    } else if (!running && wasRunning_) {
        push(now, {0xFC});  // Stop
    }
    wasRunning_ = running;
    if (!running) return;
    const double samplesPerPulse = sampleRate * 60.0 / (bpm * 24.0);
    while (samplesToNextPulse_ < frames) {
        push(now + samplesToNextPulse_ / sampleRate * 1000.0, {0xF8});
        samplesToNextPulse_ += samplesPerPulse;
    }
    samplesToNextPulse_ -= frames;
}

void MidiClockSender::run() {
    while (!threadShouldExit()) {
        if (hold_.load()) {
            wait(1);
            continue;
        }
        int s1, n1, s2, n2;
        fifo_.prepareToRead(fifo_.getNumReady(), s1, n1, s2, n2);
        int done = 0;
        const double now = juce::Time::getMillisecondCounterHiRes();
        auto deliver = [&](int start, int count) {
            for (int i = 0; i < count; ++i) {
                const auto& it = items_[static_cast<size_t>(start + i)];
                if (it.timeMs > now + 0.5) return false;  // not due yet: keep order, stop here
                const juce::MidiMessage m(it.bytes.data(), it.size);
                const juce::ScopedLock lock(outputsLock_);
                for (auto& out : outputs_) out->sendMessageNow(m);
                ++done;
            }
            return true;
        };
        if (deliver(s1, n1)) deliver(s2, n2);
        fifo_.finishedRead(done);
        wait(1);
    }
}

void MidiClockSender::drainForTesting(const std::function<void(const juce::MidiMessage&)>& sink) {
    int s1, n1, s2, n2;
    fifo_.prepareToRead(fifo_.getNumReady(), s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) sink(juce::MidiMessage(items_[static_cast<size_t>(s1 + i)].bytes.data(), items_[static_cast<size_t>(s1 + i)].size));
    for (int i = 0; i < n2; ++i) sink(juce::MidiMessage(items_[static_cast<size_t>(s2 + i)].bytes.data(), items_[static_cast<size_t>(s2 + i)].size));
    fifo_.finishedRead(n1 + n2);
}

}  // namespace ynotapp
