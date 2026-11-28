#include "ynot/model.hpp"

#include <algorithm>
#include <random>

namespace ynot {

std::string newId() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int w = 0; w < 2; ++w) {
        uint64_t v = rng();
        for (int i = 0; i < 16; ++i) { s += hex[v & 0xF]; v >>= 4; }
    }
    return s;
}

double AutomationLane::valueAt(int64_t tick, double fallback) const {
    if (points.empty()) return fallback;
    if (tick <= points.front().tick) return points.front().value;
    if (tick >= points.back().tick) return points.back().value;
    auto it = std::upper_bound(points.begin(), points.end(), tick,
                               [](int64_t t, const AutomationPoint& p) { return t < p.tick; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    if (b.tick == a.tick) return b.value;
    double f = static_cast<double>(tick - a.tick) / static_cast<double>(b.tick - a.tick);
    return a.value + (b.value - a.value) * f;
}

const AutomationLane* Track::lane(const std::string& target) const {
    for (auto& l : automation)
        if (l.target == target) return &l;
    return nullptr;
}

Track* Project::findTrack(const std::string& tid) {
    for (auto& t : tracks)
        if (t.id == tid) return &t;
    return nullptr;
}
const Track* Project::findTrack(const std::string& tid) const {
    return const_cast<Project*>(this)->findTrack(tid);
}
const MediaRef* Project::findMedia(const std::string& mid) const {
    for (auto& m : media)
        if (m.id == mid) return &m;
    return nullptr;
}

std::vector<const Track*> Project::orderedTracks() const {
    std::vector<const Track*> v;
    for (auto& t : tracks) v.push_back(&t);
    std::stable_sort(v.begin(), v.end(), [](const Track* a, const Track* b) {
        if (a->order != b->order) return a->order < b->order;
        return a->id < b->id;
    });
    return v;
}

int64_t Project::endTick() const {
    int64_t end = 0;
    for (auto& t : tracks) {
        for (auto& c : t.audioClips) end = std::max(end, c.startTick + c.lengthTick);
        for (auto& c : t.midiClips) end = std::max(end, c.startTick + c.lengthTick);
    }
    return end;
}

TempoMap::TempoMap(const Project& p) {
    auto spt = [](double bpm) { return 60.0 / std::clamp(bpm, kMinBpm, kMaxBpm) / static_cast<double>(kTicksPerQuarter); };
    segs_.clear();
    segs_.push_back({0.0, 0.0, spt(p.tempoBpm)});
    for (const auto& c : p.tempoChanges) {
        if (c.tick <= 0) continue;
        const auto t = static_cast<double>(c.tick);
        auto& last = segs_.back();
        if (t <= last.tick) {  // unsorted or duplicate: the later entry wins
            if (t == last.tick) last.secondsPerTick = spt(c.bpm);
            continue;
        }
        segs_.push_back({t, last.seconds + (t - last.tick) * last.secondsPerTick, spt(c.bpm)});
    }
}

const TempoMap::Segment& TempoMap::segForTick(double tick) const {
    auto it = std::upper_bound(segs_.begin(), segs_.end(), tick,
                               [](double t, const Segment& s) { return t < s.tick; });
    return it == segs_.begin() ? segs_.front() : *(it - 1);
}

const TempoMap::Segment& TempoMap::segForSeconds(double seconds) const {
    auto it = std::upper_bound(segs_.begin(), segs_.end(), seconds,
                               [](double s, const Segment& seg) { return s < seg.seconds; });
    return it == segs_.begin() ? segs_.front() : *(it - 1);
}

double TempoMap::tickToSeconds(double tick) const {
    const auto& s = segForTick(tick);
    return s.seconds + (tick - s.tick) * s.secondsPerTick;  // before tick 0: the first tempo extends back
}

double TempoMap::secondsToTick(double seconds) const {
    const auto& s = segForSeconds(seconds);
    return s.tick + (seconds - s.seconds) / s.secondsPerTick;
}

double TempoMap::bpmAt(double tick) const {
    return 60.0 / segForTick(tick).secondsPerTick / static_cast<double>(kTicksPerQuarter);
}

int64_t TempoMap::tickToFrame(int64_t tick, double sampleRate) const {
    return static_cast<int64_t>(std::llround(tickToSeconds(static_cast<double>(tick)) * sampleRate));
}

int64_t TempoMap::frameToTick(int64_t frame, double sampleRate) const {
    return static_cast<int64_t>(std::llround(secondsToTick(static_cast<double>(frame) / sampleRate)));
}

double Project::tempoAt(int64_t tick) const { return TempoMap(*this).bpmAt(static_cast<double>(tick)); }
double Project::tickToSeconds(double tick) const { return TempoMap(*this).tickToSeconds(tick); }
double Project::secondsToTick(double seconds) const { return TempoMap(*this).secondsToTick(seconds); }

std::string Project::tempoSignature() const {
    std::string s = std::to_string(tempoBpm);
    for (const auto& c : tempoChanges) s += ";" + std::to_string(c.tick) + "@" + std::to_string(c.bpm);
    return s;
}

}  // namespace ynot
