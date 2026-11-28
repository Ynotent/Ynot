#include "MidiControl.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ynotapp {

std::optional<TransportCommand> parseMmc(const juce::MidiMessage& m) {
    if (!m.isSysEx()) return std::nullopt;
    const auto* d = m.getSysExData();  // without F0/F7
    if (m.getSysExDataSize() < 4 || d[0] != 0x7F || d[2] != 0x06) return std::nullopt;
    switch (d[3]) {
        case 0x01: return TransportCommand::Stop;
        case 0x02:
        case 0x03: return TransportCommand::Play;
        case 0x04: return TransportCommand::FastForward;
        case 0x05: return TransportCommand::Rewind;
        case 0x06: return TransportCommand::Record;
        case 0x07: return TransportCommand::RecordExit;
        case 0x09: return TransportCommand::Pause;
        default: return std::nullopt;
    }
}

double ccToVolumeDb(int value) {
    if (value <= 0) return -96.0;
    const double g = std::pow(value / 100.0, 2.0);
    return std::max(-96.0, 20.0 * std::log10(g));
}

double ccToPan(int value) { return std::clamp((std::clamp(value, 0, 127) - 64) / 63.0, -1.0, 1.0); }

namespace {
const char* targetName(MidiTarget t) {
    switch (t) {
        case MidiTarget::TrackVolume: return "trackVolume";
        case MidiTarget::TrackPan: return "trackPan";
        case MidiTarget::MasterVolume: return "masterVolume";
        case MidiTarget::Play: return "play";
        case MidiTarget::Stop: return "stop";
        case MidiTarget::Record: return "record";
        case MidiTarget::Rewind: return "rewind";
        case MidiTarget::FastForward: return "fastForward";
    }
    return "";
}
bool perTrack(MidiTarget t) { return t == MidiTarget::TrackVolume || t == MidiTarget::TrackPan; }
bool sameTarget(const MidiMapping& m, MidiTarget t, int track) {
    return m.target == t && (!perTrack(t) || m.trackIndex == track);
}
}  // namespace

MidiControlMap::MidiControlMap(juce::File settingsFile) : file_(std::move(settingsFile)) { load(); }

MidiControlMap::~MidiControlMap() { cancelPendingUpdate(); }

void MidiControlMap::useSettingsFileForTesting(const juce::File& f) {
    {
        const juce::ScopedLock l(lock_);
        maps_.clear();
        pending_.clear();
        file_ = f;
        clockOut_ = false;
    }
    load();
}

void MidiControlMap::startLearning(MidiTarget target, int trackIndex) {
    const juce::ScopedLock l(lock_);
    learnTarget_ = target;
    learnTrack_ = trackIndex;
    learning_ = true;
}

void MidiControlMap::cancelLearning() { learning_ = false; }

bool MidiControlMap::isLearning(MidiTarget target, int trackIndex) const {
    const juce::ScopedLock l(lock_);
    return learning_.load() && learnTarget_ == target && (!perTrack(target) || learnTrack_ == trackIndex);
}

void MidiControlMap::forget(MidiTarget target, int trackIndex) {
    {
        const juce::ScopedLock l(lock_);
        for (size_t i = maps_.size(); i-- > 0;)
            if (sameTarget(maps_[i], target, trackIndex)) {
                maps_.erase(maps_.begin() + static_cast<long>(i));
                pending_.erase(pending_.begin() + static_cast<long>(i));
            }
    }
    save();
}

void MidiControlMap::clear() {
    {
        const juce::ScopedLock l(lock_);
        maps_.clear();
        pending_.clear();
    }
    save();
}

std::optional<MidiMapping> MidiControlMap::mappingFor(MidiTarget target, int trackIndex) const {
    const juce::ScopedLock l(lock_);
    for (const auto& m : maps_)
        if (sameTarget(m, target, trackIndex)) return m;
    return std::nullopt;
}

std::vector<MidiMapping> MidiControlMap::mappings() const {
    const juce::ScopedLock l(lock_);
    return maps_;
}

void MidiControlMap::setClockOut(bool on) {
    clockOut_ = on;
    save();
}

bool MidiControlMap::handle(const juce::MidiMessage& m) {
    if (auto cmd = parseMmc(m)) {
        {
            const juce::ScopedLock l(lock_);
            pendingTransport_.push_back(*cmd);
        }
        triggerAsyncUpdate();
        return true;
    }
    if (!m.isController()) return false;
    const int ch = m.getChannel(), cc = m.getControllerNumber(), v = m.getControllerValue();
    // Leave the performance controls to the instruments: mod wheel, sustain, all-notes-off.
    const bool performance = cc == 1 || cc == 64 || cc >= 120;
    const juce::ScopedLock l(lock_);
    if (learning_.load() && !performance) {
        // One control does one job: drop this CC's old mapping and the target's old CC.
        for (size_t i = maps_.size(); i-- > 0;)
            if ((maps_[i].channel == ch && maps_[i].cc == cc) || sameTarget(maps_[i], learnTarget_, learnTrack_)) {
                maps_.erase(maps_.begin() + static_cast<long>(i));
                pending_.erase(pending_.begin() + static_cast<long>(i));
            }
        maps_.push_back({learnTarget_, perTrack(learnTarget_) ? learnTrack_ : 0, ch, cc});
        pending_.push_back(v);
        learning_ = false;
        learnedPending_ = true;
        triggerAsyncUpdate();
        return true;
    }
    for (size_t i = 0; i < maps_.size(); ++i)
        if (maps_[i].channel == ch && maps_[i].cc == cc) {
            pending_[i] = v;
            triggerAsyncUpdate();
            return true;
        }
    return false;
}

void MidiControlMap::handleAsyncUpdate() {
    std::vector<std::pair<MidiMapping, int>> moved;
    std::vector<TransportCommand> transport;
    bool learned = false;
    {
        const juce::ScopedLock l(lock_);
        for (size_t i = 0; i < maps_.size(); ++i)
            if (pending_[i] >= 0) {
                moved.emplace_back(maps_[i], pending_[i]);
                pending_[i] = -1;
            }
        transport.swap(pendingTransport_);
        learned = std::exchange(learnedPending_, false);
    }
    if (learned) save();
    for (auto& [map, v] : moved) {
        const bool button = map.target != MidiTarget::TrackVolume && map.target != MidiTarget::TrackPan &&
                            map.target != MidiTarget::MasterVolume;
        if (button) {
            if (v < 64 || !onTransport) continue;  // buttons act on press
            const TransportCommand cmd = map.target == MidiTarget::Play     ? TransportCommand::Play
                                         : map.target == MidiTarget::Stop   ? TransportCommand::Stop
                                         : map.target == MidiTarget::Record ? TransportCommand::Record
                                         : map.target == MidiTarget::Rewind ? TransportCommand::Rewind
                                                                            : TransportCommand::FastForward;
            onTransport(cmd);
        } else if (onControl) {
            onControl(map, v);
        }
    }
    if (onTransport)
        for (auto c : transport) onTransport(c);
    if (learned && onLearned) onLearned();
}

void MidiControlMap::save() const {
    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> list;
    {
        const juce::ScopedLock l(lock_);
        for (const auto& m : maps_) {
            auto* o = new juce::DynamicObject();
            o->setProperty("target", targetName(m.target));
            o->setProperty("track", m.trackIndex);
            o->setProperty("channel", m.channel);
            o->setProperty("cc", m.cc);
            list.add(juce::var(o));
        }
    }
    root->setProperty("mappings", list);
    root->setProperty("clockOut", clockOut_);
    if (file_ == juce::File()) return;
    file_.getParentDirectory().createDirectory();
    file_.replaceWithText(juce::JSON::toString(juce::var(root)));
}

void MidiControlMap::load() {
    if (!file_.existsAsFile()) return;
    const auto json = juce::JSON::parse(file_);
    clockOut_ = static_cast<bool>(json.getProperty("clockOut", false));
    if (auto* arr = json.getProperty("mappings", {}).getArray())
        for (const auto& v : *arr) {
            const auto name = v.getProperty("target", "").toString();
            for (int t = 0; t <= static_cast<int>(MidiTarget::FastForward); ++t)
                if (name == targetName(static_cast<MidiTarget>(t))) {
                    maps_.push_back({static_cast<MidiTarget>(t), static_cast<int>(v.getProperty("track", 0)),
                                     std::clamp(static_cast<int>(v.getProperty("channel", 1)), 1, 16),
                                     std::clamp(static_cast<int>(v.getProperty("cc", 0)), 0, 127)});
                    pending_.push_back(-1);
                }
        }
}

}  // namespace ynotapp
