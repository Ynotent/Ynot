#include "AudioExporter.h"

#include <cmath>

#if JUCE_WINDOWS
#include "MediaFoundationDecoder.h"
#endif
#if JUCE_MAC || JUCE_IOS
#include "AppleAacEncoder.h"
#endif

namespace ynotapp {

std::vector<ExportFormat> availableExportFormats() {
    std::vector<ExportFormat> f{ExportFormat::Wav, ExportFormat::Flac, ExportFormat::Ogg};
#if JUCE_MAC || JUCE_IOS
    f.push_back(ExportFormat::M4a);
#elif JUCE_WINDOWS
    f.push_back(ExportFormat::M4a);
    f.push_back(ExportFormat::Mp3);
#endif
    return f;
}

juce::String formatName(ExportFormat f) {
    switch (f) {
        case ExportFormat::Wav: return "WAV";
        case ExportFormat::Flac: return "FLAC (lossless, smaller)";
        case ExportFormat::Ogg: return "Ogg Vorbis";
        case ExportFormat::M4a: return "M4A (AAC)";
        case ExportFormat::Mp3: return "MP3";
    }
    return "WAV";
}

juce::String formatExtension(ExportFormat f) {
    switch (f) {
        case ExportFormat::Wav: return ".wav";
        case ExportFormat::Flac: return ".flac";
        case ExportFormat::Ogg: return ".ogg";
        case ExportFormat::M4a: return ".m4a";
        case ExportFormat::Mp3: return ".mp3";
    }
    return ".wav";
}

bool isLossy(ExportFormat f) { return f == ExportFormat::Ogg || f == ExportFormat::M4a || f == ExportFormat::Mp3; }

double exportRateFor(const ExportSettings& s) {
    if (!isLossy(s.format)) return s.sampleRate;
    return std::abs(s.sampleRate - 44100.0) < 1 || std::abs(s.sampleRate - 88200.0) < 1 ? 44100.0 : 48000.0;
}

ExportJob prepareExport(const ynot::Project& p, ynot::MediaProvider& media, ynot::PluginHost& host,
                        const ExportSettings& s) {
    ExportJob job;
    job.settings = s;
    job.rate = exportRateFor(s);
    job.engine = std::make_unique<ynot::Engine>();
    ynot::EngineOptions o;
    o.sampleRate = job.rate;
    o.maxBlock = 1024;
    job.engine->prepare(p, media, host, o);
    job.issues = job.engine->issues();
    const ynot::TempoMap tempo(p);
    if (s.loopOnly && p.loop.valid()) {
        job.startFrame = tempo.tickToFrame(p.loop.startTick, job.rate);
        job.endFrame = tempo.tickToFrame(p.loop.endTick, job.rate);
        job.autoTail = false;  // exact length, so the file loops seamlessly
    } else {
        job.startFrame = 0;
        job.endFrame = tempo.tickToFrame(p.endTick(), job.rate);
    }
    job.engine->seek(job.startFrame);
    return job;
}

namespace {
ynot::AudioBuffer render(ExportJob& job, const std::function<bool(double)>& progress, bool& cancelled) {
    ynot::AudioBuffer out;
    out.sampleRate = job.rate;
    out.channels = 2;
    const int block = job.engine->maxBlock();
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    const int64_t length = std::max<int64_t>(0, job.endFrame - job.startFrame);
    const auto maxTail = static_cast<int64_t>(10.0 * job.rate), silenceNeeded = static_cast<int64_t>(1.0 * job.rate);
    out.samples.reserve(static_cast<size_t>(length + (job.autoTail ? silenceNeeded : 0)) * 2);
    int64_t done = 0, silentRun = 0, lastAudible = 0;
    while (true) {
        const bool inSong = done < length;
        if (!inSong && (!job.autoTail || silentRun >= silenceNeeded || done - length >= maxTail)) break;
        const int n = inSong ? static_cast<int>(std::min<int64_t>(block, length - done)) : block;
        job.engine->process(l.data(), r.data(), n);
        for (int i = 0; i < n; ++i) {
            const float a = l[static_cast<size_t>(i)], b = r[static_cast<size_t>(i)];
            out.samples.push_back(a);
            out.samples.push_back(b);
            if (std::max(std::abs(a), std::abs(b)) > 1e-5f) {  // about -100 dB
                silentRun = 0;
                lastAudible = done + i + 1;
            } else {
                ++silentRun;
            }
        }
        done += n;
        if (progress && (done / block) % 32 == 0 && !progress(0.9 * static_cast<double>(std::min(done, length)) /
                                                               static_cast<double>(std::max<int64_t>(1, length)))) {
            cancelled = true;
            return out;
        }
    }
    // Tail: keep what was still audible plus a short fade-safe margin of silence.
    if (job.autoTail) {
        const int64_t keep = std::max(length, std::min(done, lastAudible + static_cast<int64_t>(0.1 * job.rate)));
        out.samples.resize(static_cast<size_t>(keep) * 2);
    }
    return out;
}

void normalizeTo(ynot::AudioBuffer& b, float targetPeak) {
    float peak = 0;
    for (float v : b.samples) peak = std::max(peak, std::abs(v));
    if (peak <= 1e-9f) return;
    const float g = targetPeak / peak;
    for (float& v : b.samples) v *= g;
}

// 16-bit: TPDF dither (+-1 LSB) instead of plain rounding, so quiet fades don't turn grainy.
void dither16(ynot::AudioBuffer& b) {
    juce::Random rng(0x5eed);
    constexpr float lsb = 1.0f / 32768.0f;
    for (float& v : b.samples) v += (rng.nextFloat() - rng.nextFloat()) * lsb;
}

juce::String writeWithJuce(juce::AudioFormat& format, const ynot::AudioBuffer& audio, const juce::File& file,
                           int bits, int quality) {
    file.deleteFile();
    auto stream = file.createOutputStream();
    if (!stream) return "Couldn't create \"" + file.getFullPathName() + "\". Check the folder and permissions.";
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.get(), audio.sampleRate, 2, bits, {}, quality));
    if (!writer) return format.getFormatName() + " can't write this combination of settings.";
    stream.release();  // the writer owns it now
    const auto frames = static_cast<int>(audio.frames());
    juce::AudioBuffer<float> planar(2, std::max(1, frames));
    for (int i = 0; i < frames; ++i) {
        planar.setSample(0, i, audio.samples[static_cast<size_t>(i) * 2]);
        planar.setSample(1, i, audio.samples[static_cast<size_t>(i) * 2 + 1]);
    }
    if (!writer->writeFromAudioSampleBuffer(planar, 0, frames)) return "Writing the file failed (disk full?).";
    return {};
}

int oggQualityIndex(juce::OggVorbisAudioFormat& ogg, int kbps) {
    const auto options = ogg.getQualityOptions();
    int best = 0, bestDiff = 1 << 30;
    for (int i = 0; i < options.size(); ++i) {
        const int diff = std::abs(options[i].getIntValue() - kbps);
        if (diff < bestDiff) bestDiff = diff, best = i;
    }
    return best;
}
}  // namespace

juce::String runExport(ExportJob& job, const juce::File& file, const std::function<bool(double)>& progress) {
    if (!job.engine) return "Nothing to export.";
    if (job.endFrame <= job.startFrame) return "The song is empty: record or import something first.";
    bool cancelled = false;
    auto audio = render(job, progress, cancelled);
    if (cancelled) return "Export cancelled.";
    const auto& s = job.settings;
    if (s.normalize) normalizeTo(audio, 0.891f);  // -1 dBFS
    if (progress) progress(0.92);
    file.getParentDirectory().createDirectory();
    juce::String err;
    switch (s.format) {
        case ExportFormat::Wav: {
            const int bits = s.bitDepth == 16 ? 16 : s.bitDepth == 32 ? 32 : 24;
            if (bits == 16) dither16(audio);
            juce::WavAudioFormat wav;
            err = writeWithJuce(wav, audio, file, bits, 0);
            break;
        }
        case ExportFormat::Flac: {
            const int bits = s.bitDepth == 16 ? 16 : 24;
            if (bits == 16) dither16(audio);
            juce::FlacAudioFormat flac;
            err = writeWithJuce(flac, audio, file, bits, 0);
            break;
        }
        case ExportFormat::Ogg: {
            juce::OggVorbisAudioFormat ogg;
            err = writeWithJuce(ogg, audio, file, 16, oggQualityIndex(ogg, s.bitrateKbps));
            break;
        }
        case ExportFormat::M4a:
        case ExportFormat::Mp3: {
            std::string e;
            bool ok = false;
#if JUCE_WINDOWS
            ok = encodeWithMediaFoundation(std::wstring(file.getFullPathName().toWideCharPointer()), audio,
                                           s.format == ExportFormat::Mp3, s.bitrateKbps, e);
#elif JUCE_MAC || JUCE_IOS
            if (s.format == ExportFormat::M4a)
                ok = encodeAacWithCoreAudio(file.getFullPathName().toStdString(), audio, s.bitrateKbps, e);
            else
                e = "MP3 export isn't available on Apple devices yet. M4A (AAC) sounds as good or better at the "
                    "same size and plays on every phone, computer and streaming service.";
#else
            e = formatName(s.format).toStdString() + " export isn't available on this device. Use WAV, FLAC or Ogg.";
#endif
            if (!ok) err = e.empty() ? juce::String("Encoding failed.") : juce::String(e);
            break;
        }
    }
    if (progress) progress(1.0);
    if (err.isNotEmpty()) file.deleteFile();
    return err;
}

juce::String exportSong(const ynot::Project& p, ynot::MediaProvider& media, ynot::PluginHost& host,
                        const ExportSettings& s, const juce::File& file) {
    auto job = prepareExport(p, media, host, s);
    return runExport(job, file);
}

}  // namespace ynotapp
