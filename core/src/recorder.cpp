#include "ynot/recorder.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

#include "ynot/media.hpp"

namespace fs = std::filesystem;

namespace ynot {

// ---------------------------------------------------------------- ring

SpscFloatRing::SpscFloatRing(size_t capacity) : buf_(capacity + 1) {}

size_t SpscFloatRing::write(const float* data, size_t n) {
    const size_t size = buf_.size();
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_acquire);
    size_t free = (tail + size - head - 1) % size;
    n = std::min(n, free);
    size_t first = std::min(n, size - head);
    std::copy(data, data + first, buf_.begin() + static_cast<std::ptrdiff_t>(head));
    std::copy(data + first, data + n, buf_.begin());
    head_.store((head + n) % size, std::memory_order_release);
    return n;
}

size_t SpscFloatRing::read(float* data, size_t n) {
    const size_t size = buf_.size();
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);
    size_t avail = (head + size - tail) % size;
    n = std::min(n, avail);
    size_t first = std::min(n, size - tail);
    std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(tail), buf_.begin() + static_cast<std::ptrdiff_t>(tail + first), data);
    std::copy(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n - first), data + first);
    tail_.store((tail + n) % size, std::memory_order_release);
    return n;
}

size_t SpscFloatRing::available() const {
    const size_t size = buf_.size();
    return (head_.load(std::memory_order_acquire) + size - tail_.load(std::memory_order_acquire)) % size;
}

// ---------------------------------------------------------------- recorder

struct Recorder::Lane {
    RecordTarget target;
    int channels;
    std::unique_ptr<SpscFloatRing> ring;
    std::vector<float> interleave;  // audio-thread scratch, maxBlock * channels
    std::vector<float> drainBuf;    // drain-thread scratch
    WavFileWriter writer;
    std::string path;
    bool writeFailed = false;
};

Recorder::Recorder() = default;
Recorder::~Recorder() { cancel(); }

bool Recorder::start(const Config& cfg) {
    cancel();
    error_.clear();
    cfg_ = cfg;
    dropped_ = 0;
    captured_ = 0;
    startFrame_ = -1;
    if (cfg.targets.empty()) {
        error_ = "No tracks are armed for recording.";
        return false;
    }
    auto dir = fs::path(cfg.packageDir) / ".recording";
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (auto& t : cfg.targets) {
        auto lane = std::make_unique<Lane>();
        lane->target = t;
        lane->channels = std::clamp(static_cast<int>(t.inputChannels.size()), 1, 2);
        const size_t ringSamples =
            static_cast<size_t>(cfg.ringSeconds * cfg.sampleRate) * static_cast<size_t>(lane->channels);
        lane->ring = std::make_unique<SpscFloatRing>(ringSamples);
        lane->interleave.assign(static_cast<size_t>(cfg.maxBlock * lane->channels), 0.f);
        lane->drainBuf.assign(static_cast<size_t>(cfg.sampleRate / 10.0) * static_cast<size_t>(lane->channels), 0.f);
        lane->path = (dir / (newId() + ".wav")).string();
        if (!lane->writer.open(lane->path, cfg.sampleRate, lane->channels)) {
            error_ = "Can't write recordings to " + dir.string() + " (disk full or no permission?).";
            lanes_.clear();
            return false;
        }
        lanes_.push_back(std::move(lane));
    }
    recording_ = true;
    return true;
}

void Recorder::capture(const float* const* inputs, int numInputs, int frames, int64_t engineFrame) {
    if (!recording_.load(std::memory_order_acquire)) return;
    int64_t expected = -1;
    startFrame_.compare_exchange_strong(expected, engineFrame);
    captured_.fetch_add(frames, std::memory_order_relaxed);
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, cfg_.maxBlock);
        for (auto& lp : lanes_) {
            Lane& lane = *lp;
            float* dst = lane.interleave.data();
            for (int c = 0; c < lane.channels; ++c) {
                int in = lane.target.inputChannels.empty() ? c : lane.target.inputChannels[static_cast<size_t>(c)];
                const float* src = (in >= 0 && in < numInputs && inputs[in]) ? inputs[in] + done : nullptr;
                for (int i = 0; i < n; ++i) dst[i * lane.channels + c] = src ? src[i] : 0.f;
            }
            size_t want = static_cast<size_t>(n * lane.channels);
            size_t wrote = lane.ring->write(dst, want);
            if (wrote < want) dropped_.fetch_add(static_cast<int64_t>((want - wrote) / static_cast<size_t>(lane.channels)));
        }
        done += n;
    }
}

bool Recorder::drain() {
    bool ok = true;
    for (auto& lp : lanes_) {
        Lane& lane = *lp;
        while (true) {
            size_t got = lane.ring->read(lane.drainBuf.data(), lane.drainBuf.size());
            if (got == 0) break;
            if (!lane.writeFailed && !lane.writer.write(lane.drainBuf.data(), static_cast<int64_t>(got) / lane.channels)) {
                lane.writeFailed = true;
                error_ = "Recording stopped writing to disk (disk full or file too large).";
            }
        }
        ok &= !lane.writeFailed;
    }
    return ok;
}

int64_t Recorder::recordedFrames() const { return captured_.load(); }

std::vector<RecordedTake> Recorder::finish() {
    recording_ = false;
    drain();
    std::vector<RecordedTake> takes;
    for (auto& lp : lanes_) {
        lp->writer.close();
        RecordedTake t;
        t.trackId = lp->target.trackId;
        t.tempPath = lp->path;
        t.startFrame = std::max<int64_t>(0, startFrame_.load());
        t.frames = lp->writer.frames();
        t.channels = lp->channels;
        t.sampleRate = cfg_.sampleRate;
        if (t.frames > 0 && startFrame_.load() >= 0) takes.push_back(t);
        else { std::error_code ec; fs::remove(lp->path, ec); }
    }
    lanes_.clear();
    return takes;
}

void Recorder::cancel() {
    recording_ = false;
    for (auto& lp : lanes_) {
        lp->writer.close();
        std::error_code ec;
        fs::remove(lp->path, ec);
    }
    lanes_.clear();
}

// ---------------------------------------------------------------- commit

int64_t frameToTick(const Project& p, int64_t frame, double sr) {
    return static_cast<int64_t>(std::llround(static_cast<double>(frame) / sr / p.secondsPerTick()));
}

int64_t tickToFrame(const Project& p, int64_t tick, double sr) {
    return static_cast<int64_t>(std::llround(static_cast<double>(tick) * p.secondsPerTick() * sr));
}

const MediaRef& addMediaFile(Project& p, const std::string& packageDir, const std::string& wavPath,
                             double sampleRate, int channels, int64_t frames, const std::string& origin) {
    std::string hash = sha256File(wavPath);
    for (auto& m : p.media)
        if (m.sha256 == hash) {
            std::error_code ec;
            fs::remove(wavPath, ec);
            return m;
        }
    MediaRef m;
    m.id = newId();
    m.sha256 = hash;
    m.fileName = "media/" + hash + ".wav";
    m.sampleRate = sampleRate;
    m.channels = channels;
    m.frames = frames;
    m.origin = origin;
    auto dest = fs::path(packageDir) / m.fileName;
    fs::create_directories(dest.parent_path());
    std::error_code ec;
    if (fs::exists(dest)) fs::remove(wavPath, ec);
    else {
        fs::rename(wavPath, dest, ec);
        if (ec) {  // e.g. different volume: copy then remove
            fs::copy_file(wavPath, dest, fs::copy_options::overwrite_existing);
            fs::remove(wavPath, ec);
        }
    }
    p.media.push_back(m);
    return p.media.back();
}

void placeClipReplacing(const Project& p, Track& track, AudioClip clip) {
    const int64_t s = clip.startTick, e = clip.startTick + clip.lengthTick;
    std::vector<AudioClip> kept;
    for (auto& c : track.audioClips) {
        const int64_t cs = c.startTick, ce = c.startTick + c.lengthTick;
        if (ce <= s || cs >= e) { kept.push_back(c); continue; }
        const MediaRef* m = p.findMedia(c.mediaId);
        const double rate = m ? m->sampleRate : 48000.0;
        auto framesFor = [&](int64_t ticks) { return tickToFrame(p, ticks, rate); };
        if (cs >= s && ce <= e) continue;  // fully covered: removed
        if (cs < s && ce > e) {            // new clip lands inside: split around it
            AudioClip left = c, right = c;
            left.lengthTick = s - cs;
            right.id = newId();
            right.startTick = e;
            right.lengthTick = ce - e;
            right.sourceOffsetFrames = c.sourceOffsetFrames + framesFor(e - cs);
            kept.push_back(left);
            kept.push_back(right);
        } else if (cs < s) {               // overlaps the new clip's start: trim tail
            AudioClip left = c;
            left.lengthTick = s - cs;
            kept.push_back(left);
        } else {                           // overlaps the new clip's end: trim head
            AudioClip right = c;
            right.startTick = e;
            right.lengthTick = ce - e;
            right.sourceOffsetFrames = c.sourceOffsetFrames + framesFor(e - cs);
            kept.push_back(right);
        }
    }
    kept.push_back(std::move(clip));
    std::sort(kept.begin(), kept.end(), [](const AudioClip& a, const AudioClip& b) { return a.startTick < b.startTick; });
    track.audioClips = std::move(kept);
}

std::vector<std::string> commitTakes(Project& p, const std::string& packageDir,
                                     const std::vector<RecordedTake>& takes, int latencyFrames) {
    std::vector<std::string> ids;
    for (auto& take : takes) {
        Track* track = p.findTrack(take.trackId);
        // Instrument tracks record MIDI (later milestone); audio takes only go on audio tracks.
        if (!track || track->kind != TrackKind::Audio || take.frames <= latencyFrames) {
            std::error_code ec;
            fs::remove(take.tempPath, ec);
            continue;
        }
        std::string mediaId =
            addMediaFile(p, packageDir, take.tempPath, take.sampleRate, take.channels, take.frames, "recording").id;
        track = p.findTrack(take.trackId);  // media push may not move tracks, but stay safe
        AudioClip clip;
        clip.id = newId();
        clip.mediaId = mediaId;
        // Sample i of the take was heard by the performer at engine frame
        // startFrame + i - latency, so skip the first `latency` samples.
        clip.sourceOffsetFrames = latencyFrames;
        clip.startTick = frameToTick(p, take.startFrame, take.sampleRate);
        clip.lengthTick = frameToTick(p, take.frames - latencyFrames, take.sampleRate);
        if (clip.lengthTick <= 0) continue;
        ids.push_back(clip.id);
        placeClipReplacing(p, *track, std::move(clip));
    }
    std::error_code ec;
    fs::remove(fs::path(packageDir) / ".recording", ec);  // only succeeds once empty
    return ids;
}

}  // namespace ynot
