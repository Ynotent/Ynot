// Hardware controller support (e.g. Akai MPK25): MIDI Learn for knobs/faders, MMC transport
// buttons, and the parsing helpers. Mappings are device-local (they describe this computer's
// controller), saved in the app data folder, and target mixer channels by position so they
// keep working in every song.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace ynotapp {

enum class MidiTarget { TrackVolume, TrackPan, MasterVolume, Play, Stop, Record, Rewind, FastForward };

struct MidiMapping {
    MidiTarget target = MidiTarget::TrackVolume;
    int trackIndex = 0;  // mixer channel (0 = first track); unused for master/transport
    int channel = 1;     // MIDI channel 1..16
    int cc = 0;
};

enum class TransportCommand { Play, Stop, Record, RecordExit, Pause, Rewind, FastForward };

// MIDI Machine Control (F0 7F <device> 06 <cmd> F7), what the MPK25's transport buttons send.
std::optional<TransportCommand> parseMmc(const juce::MidiMessage& m);

// CC value -> mixer value. Volume: 100 = 0 dB (gain = (v/100)^2), 127 = about +4 dB, 0 = off.
double ccToVolumeDb(int value);
double ccToPan(int value);  // 0 = left, 64 = centre, 127 = right

class MidiControlMap : private juce::AsyncUpdater {
public:
    explicit MidiControlMap(juce::File settingsFile);
    ~MidiControlMap() override;

    // Message thread.
    void startLearning(MidiTarget target, int trackIndex);  // the next CC moved is mapped
    void cancelLearning();
    bool isLearning() const { return learning_.load(); }
    bool isLearning(MidiTarget target, int trackIndex) const;
    void forget(MidiTarget target, int trackIndex);
    void clear();
    std::optional<MidiMapping> mappingFor(MidiTarget target, int trackIndex) const;
    std::vector<MidiMapping> mappings() const;
    bool clockOut() const { return clockOut_; }
    void setClockOut(bool on);

    // MIDI thread: true if the message was used (learned or mapped) and must not play notes.
    bool handle(const juce::MidiMessage& m);

    // Tests: deliver pending updates now; use another settings file (reloads from it).
    void flushForTesting() { handleUpdateNowIfNeeded(); }
    void useSettingsFileForTesting(const juce::File& f);

    // Called on the message thread with the latest value of each moved control (coalesced).
    std::function<void(const MidiMapping&, int value)> onControl;
    std::function<void(TransportCommand)> onTransport;
    std::function<void()> onLearned;

private:
    void handleAsyncUpdate() override;
    void save() const;
    void load();

    juce::File file_;
    mutable juce::CriticalSection lock_;
    std::vector<MidiMapping> maps_;
    std::vector<int> pending_;  // latest value per mapping, -1 = none (guarded by lock_)
    std::vector<TransportCommand> pendingTransport_;
    std::atomic<bool> learning_{false};
    MidiTarget learnTarget_ = MidiTarget::TrackVolume;
    int learnTrack_ = 0;
    bool learnedPending_ = false;
    bool clockOut_ = false;
};

}  // namespace ynotapp
