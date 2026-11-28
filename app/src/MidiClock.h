// MIDI clock out: 24 pulses per quarter note plus Start/Stop and Song Position, so hardware such
// as an Akai MPK's arpeggiator and note repeat follows Ynot's tempo. The audio thread queues
// timestamped messages (lock-free); a sender thread delivers them to every open MIDI output.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace ynotapp {

class MidiClockSender : private juce::Thread {
public:
    MidiClockSender();
    ~MidiClockSender() override;

    // Message thread: open (or close) all MIDI outputs on this device.
    void setEnabled(bool on);
    bool isEnabled() const { return enabled_.load(); }

    // Audio thread, once per block. `running` = transport playing; positionTicks at block start.
    void processBlock(bool running, double bpm, double sampleRate, int frames, double positionTicks, double ppq);

    // Tests: drain queued messages without waiting for their time.
    void holdForTesting(bool hold) { hold_ = hold; }  // the sender thread leaves the queue alone
    void drainForTesting(const std::function<void(const juce::MidiMessage&)>& sink);

private:
    struct Item {
        double timeMs;
        std::array<juce::uint8, 3> bytes;
        int size;
    };
    void push(double timeMs, std::initializer_list<juce::uint8> bytes);
    void run() override;

    std::atomic<bool> enabled_{false}, hold_{false};
    juce::CriticalSection outputsLock_;
    std::vector<std::unique_ptr<juce::MidiOutput>> outputs_;
    juce::AbstractFifo fifo_{4096};
    std::vector<Item> items_ = std::vector<Item>(4096);
    bool wasRunning_ = false;
    double samplesToNextPulse_ = 0;
};

}  // namespace ynotapp
