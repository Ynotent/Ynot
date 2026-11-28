// The open project on this device: model + media + plugin host + live engine.
//
// Edits happen on the message thread. After an edit, rebuild() prepares a new
// ynot::Engine off the audio thread and swaps it in atomically; the audio
// callback never locks or allocates.
#pragma once

#include <algorithm>
#include <atomic>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <thread>

#include "AudioExporter.h"
#include "AudioImporter.h"
#include "JuceAudioDevice.h"
#include "JucePluginHost.h"
#include "ynot/ynot.hpp"

namespace ynotapp {

class Session : private juce::MidiInputCallback {
public:
    explicit Session(JucePluginHost& host);
    ~Session();

    // Project lifecycle.
    void newDemo(const juce::File& packageDir);
    void open(const juce::File& packageDir);
    void save();
    bool hasProject() const { return loaded_; }
    const ynot::Project& project() const { return project_; }
    juce::File packageDir() const { return dir_; }

    // Edits (message thread). Each one rebuilds the engine.
    void edit(const std::function<void(ynot::Project&)>& fn);
    void freeze(const std::string& trackId);
    void reload() { rebuild(); }  // re-resolve plugins/media without an undo step
    bool undo();
    bool redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // Playhead (message thread). Seeks are handed to the audio thread lock-free.
    void seekToTick(int64_t tick);
    int64_t positionTick() const;
    // Decoded audio for drawing waveforms (message thread; cached).
    const ynot::AudioBuffer* mediaAudio(const std::string& mediaId);
    void unfreeze(const std::string& trackId);

    // Audio.
    bool startAudio(const std::string& deviceId = {});
    void stopAudio();
    JuceAudioDevice& device() { return device_; }
    void play();
    void stop();
    bool isPlaying() const { return playing_.load(); }
    double positionSeconds() const;

    // Sample rate. The project remembers its rate; opening it switches this device to that
    // rate when the device supports it. Audio at other rates is resampled on playback.
    double sampleRate() const { return sampleRate_; }
    static const std::vector<double>& standardSampleRates();  // 44.1, 48, 88.2, 96, 192 kHz
    std::vector<double> availableSampleRates() const;          // what this device offers
    juce::String setSampleRate(double rate);                   // error message, or empty

    // Recording (armed audio tracks). Input channel choices are device-local and
    // never saved into the project, because another device has different inputs.
    bool startRecording(std::string& error);
    struct RecordResult {
        std::vector<std::string> newClips;
        int64_t droppedFrames = 0;
        int64_t droppedMidiEvents = 0;
        std::string error;
    };
    RecordResult stopRecording();
    bool isRecording() const { return recording_.load(); }
    double recordingSeconds() const;
    int64_t recordStartTick() const { return ynot::frameToTick(project_, recordStart_, sampleRate_); }
    int64_t recordEndTick() const {
        return recordEnd_ == std::numeric_limits<int64_t>::max() ? std::numeric_limits<int64_t>::max()
                                                                  : ynot::frameToTick(project_, recordEnd_, sampleRate_);
    }

    // Import an audio file (any format this device can decode) as a clip. Decoding runs on a
    // background thread; `done(error)` is called on the message thread (empty error = success).
    // trackId empty or not an audio track => a new audio track named after the file.
    void importAudioFile(const juce::File& file, const std::string& trackId, int64_t startTick,
                         std::function<void(const juce::String& error)> done);
    // Same, synchronously on the calling (message) thread. Returns an error, or empty.
    juce::String importAudioFileNow(const juce::File& file, const std::string& trackId, int64_t startTick);

    // Time stretch an audio clip. `ratio` is relative to the clip's original length (1 = reset).
    // Rendering runs on a background thread; `done(error)` runs on the message thread.
    void stretchClip(const std::string& trackId, const std::string& clipId, double ratio, ynot::StretchMode mode,
                     std::function<void(const juce::String& error)> done);
    juce::String stretchClipNow(const std::string& trackId, const std::string& clipId, double ratio,
                                ynot::StretchMode mode);
    bool isStretching() const { return stretchJobs_ > 0; }
    // Mode used for clips that haven't been stretched yet (user preference).
    void setDefaultStretchMode(ynot::StretchMode m) { defaultStretchMode_ = m; }
    ynot::StretchMode defaultStretchMode() const { return defaultStretchMode_; }

    // Export: builds the offline render (message thread); run it with runExport() on any thread.
    ExportJob prepareExport(const ExportSettings& s);

    // Quantize (device/user preference, not stored in the project).
    void setRecordQuantize(const std::string& presetId);  // "" = off
    const std::string& recordQuantizeId() const { return recordQuantizeId_; }
    void setQuantizeStrength(double s) { quantizeStrength_ = std::clamp(s, 0.0, 1.0); }
    double quantizeStrength() const { return quantizeStrength_; }
    bool quantizeClip(const std::string& trackId, const std::string& clipId, const std::string& presetId);
    // Time signature (stored in the project; undoable).
    void setTimeSignature(int numerator, int denominator);

    // Mixer: volume, pan, mute and solo change the sound immediately (no engine rebuild).
    // Call beginMixChange() once at the start of a fader/knob drag so the drag is one undo step;
    // mute/solo record their own undo step.
    void beginMixChange();
    void setTrackGain(const std::string& trackId, double gainDb);
    void setTrackPan(const std::string& trackId, double pan);
    void setTrackMute(const std::string& trackId, bool on);
    void setTrackSolo(const std::string& trackId, bool on);
    void setMasterGain(double gainDb);
    struct Meter { float left = 0, right = 0; };
    Meter takeMeter(const std::string& trackId);  // empty id = master; peak since the last call

    // Loop (cycle): playback repeats the loop; recording records a take per pass (audio) or
    // merges every pass (MIDI). Stored in the project. Takes effect immediately, no rebuild.
    void setLoopRange(int64_t startTick, int64_t endTick);
    void setLoopEnabled(bool on);
    bool loopEnabled() const { return project_.loop.enabled && project_.loop.valid(); }
    int64_t loopStart() const { return project_.loop.startTick; }
    int64_t loopEnd() const { return project_.loop.endTick; }
    bool selectTake(const std::string& trackId, const std::string& clipId, int index);

    // Punch recording: with punch on, Record pre-rolls a bar and records only between in and out.
    void setPunchEnabled(bool on) { punchEnabled_ = on; if (onChange) onChange(); }
    bool punchEnabled() const { return punchEnabled_; }
    void setPunchRange(int64_t inTick, int64_t outTick);
    int64_t punchIn() const { return punchIn_; }
    int64_t punchOut() const { return punchOut_; }
    int numInputChannels() const;
    // Inputs a track can record from: each channel alone, then stereo pairs, labelled with the
    // interface's own channel names ("In 3 (Guitar DI)").
    struct InputChoice {
        std::vector<int> channels;
        std::string label;
    };
    std::vector<InputChoice> inputChoices() const;
    void setInput(const std::string& trackId, const std::vector<int>& channels);
    std::string inputChannelName(int channel) const;
    float takeInputPeak(int channel);  // live input level since the last call (any track)

    // Outputs: the song plays through one stereo pair of the interface (device-local setting).
    int numOutputPairs() const;
    int mainOutputPair() const { return mainOutputPair_.load(); }
    void setMainOutputPair(int pair);
    std::string outputPairName(int pair) const;

    // Buffer size (latency vs. CPU safety). Device-local, remembered with the device.
    int bufferSize() const { return blockSize_; }
    std::vector<int> availableBufferSizes() const;
    juce::String setBufferSize(int frames);  // error message, or empty
    std::vector<int> inputFor(const std::string& trackId) const;
    void cycleInput(const std::string& trackId);
    std::string inputLabel(const std::string& trackId) const;

    // Hear armed tracks' inputs through their effects and fader (device-local setting).
    void setMonitoring(bool on);
    bool monitoring() const { return monitoring_.load(); }
    double monitoringLatencyMs() const;  // input + output + buffer, as the driver reports it
    // Bars of click before recording starts (0 = off).
    void setCountInBars(int bars) { countInBars_ = std::clamp(bars, 0, 4); }
    int countInBars() const { return countInBars_; }

    // MIDI: hardware inputs and the on-screen keyboard play armed instrument tracks.
    juce::MidiKeyboardState& keyboardState() { return keyboardState_; }
    int numMidiInputs() const;

    void setMetronome(bool on);
    bool metronome() const { return metronomeOn_.load(); }

    // Device-free operation for smoke tests and CI: declare simulated inputs,
    // then run audio callbacks directly.
    void simulateInputsForTesting(int numInputs) { simulatedInputs_ = numInputs; }
    void simulateOutputsForTesting(int numOutputs) { simulatedOutputs_ = numOutputs; }
    void processForTesting(const float* const* ins, int numIns, float* const* outs, int numOuts, int frames);

    // Compatibility for a target platform, using what's installed *here* for this device.
    ynot::CompatReport compatibilityFor(ynot::Platform target) const;
    std::vector<ynot::EngineIssue> engineIssues() const { return issues_; }
    std::vector<std::string> tracksToPrepareForMobile() const;

    std::function<void()> onChange;  // UI refresh hook

private:
    void rebuild();
    void deviceChanged();                            // device reconfigured (any source)
    void applyDeviceFormat(double rate, int block);  // adopt a new rate/block size
    void applyProjectRate();                         // switch the device to the project's rate
    juce::String commitImport(const juce::File& file, const DecodedAudio& decoded, const std::string& trackId,
                              int64_t startTick);
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void audioCallback(const float* const* ins, int numIns, float* const* outs, int numOuts, int frames);

    JucePluginHost& host_;
    JuceAudioDevice device_;
    ynot::Project project_;
    juce::File dir_;
    bool loaded_ = false;
    std::unique_ptr<ynot::PackageMediaProvider> media_;
    std::vector<ynot::EngineIssue> issues_;

    double sampleRate_ = 48000;
    int blockSize_ = 512;
    std::atomic<ynot::Engine*> active_{nullptr};
    std::atomic<bool> inCallback_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int64_t> position_{0};
    static constexpr int kScratchFrames = 16384;
    std::vector<float> scratchL_, scratchR_;

    ynot::Recorder recorder_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> draining_{false};
    std::thread drainThread_;
    std::map<std::string, std::vector<int>> inputs_;
    std::atomic<bool> metronomeOn_{false};
    std::atomic<bool> monitoring_{false};
    int countInBars_ = 0;
    int64_t forcedClickUntil_ = std::numeric_limits<int64_t>::min();
    ynot::History history_;

    static constexpr int kMaxLiveMidi = 512;
    juce::MidiMessageCollector collector_;
    juce::MidiKeyboardState keyboardState_;
    juce::MidiBuffer midiBuffer_;
    std::vector<ynot::MidiEvent> liveMidi_, chunkMidi_;
    std::atomic<bool> liveMidiActive_{false};
    ynot::MidiTakeRecorder midiRecorder_;
    std::atomic<bool> recordingAudio_{false}, recordingMidi_{false};
    int64_t recordStart_ = 0;
    // Continuous clock for recording: like the playhead, but it keeps running across loop
    // jumps, so pass k of a cycle recording is at loop start + k * loop length.
    std::atomic<int64_t> recordClock_{0};
    bool cycleActive_ = false;  // this recording is a cycle recording
    void applyLoopToEngine(ynot::Engine& e) const;
    void pushMixToEngine();
    int64_t recordEnd_ = std::numeric_limits<int64_t>::max();
    const ynot::QuantizeSettings* recordQuantize();
    std::string recordQuantizeId_;
    ynot::QuantizeSettings recordQuantizeSettings_;
    double quantizeStrength_ = 1.0;
    bool punchEnabled_ = false;
    bool punchActive_ = false;
    int64_t punchIn_ = ynot::kTicksPerQuarter * 4;   // bar 2
    int64_t punchOut_ = ynot::kTicksPerQuarter * 8;  // bar 3
    static constexpr int64_t kNoSeek = std::numeric_limits<int64_t>::min();
    std::atomic<int64_t> pendingSeek_{kNoSeek};
    int simulatedInputs_ = 0, simulatedOutputs_ = 2;
    static constexpr int kMaxMeteredInputs = 64;
    std::atomic<float> inputPeaks_[kMaxMeteredInputs] = {};
    std::atomic<int> mainOutputPair_{0};
    juce::File ioSettingsFile() const;
    int stretchJobs_ = 0;
    ynot::StretchMode defaultStretchMode_ = ynot::StretchMode::Poly;
};

}  // namespace ynotapp
