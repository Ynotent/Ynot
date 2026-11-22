#include "AudioImporter.h"

#if JUCE_WINDOWS
#include "MediaFoundationDecoder.h"
#endif

namespace ynotapp {

namespace {
juce::AudioFormatManager& formats() {
    static std::unique_ptr<juce::AudioFormatManager> fm = [] {
        auto m = std::make_unique<juce::AudioFormatManager>();
        m->registerBasicFormats();  // WAV, AIFF, FLAC, Ogg, MP3, + Core Audio / Windows Media
        return m;
    }();
    return *fm;
}

const juce::StringArray kExtensions{"wav", "wave", "aif", "aiff", "flac", "ogg", "mp3", "m4a", "mp4",
                                    "aac", "mov", "caf", "wma", "wmv", "asf"};

constexpr juce::int64 kMaxFrames = 48000LL * 60 * 60;  // one hour

// Per-channel (left, right) gains for folding any channel layout down to stereo.
// `layout` may be empty/mismatched (no channel map in the file).
std::vector<std::pair<float, float>> downmixWeights(juce::AudioChannelSet layout, int n) {
    constexpr float kMinus3dB = 0.70710678f;
    std::vector<std::pair<float, float>> w(static_cast<size_t>(n), {0.f, 0.f});
    if (n <= 2) {
        if (n >= 1) w[0] = {1.f, 0.f};
        if (n == 2) w[1] = {0.f, 1.f};
        return w;
    }
    if (layout.size() != n) {
        // No channel map in the file: assume the standard SMPTE/WAV order
        // FL FR FC LFE BL BR [SL SR]; anything beyond is spread evenly.
        layout = n == 6 ? juce::AudioChannelSet::create5point1() : n == 8 ? juce::AudioChannelSet::create7point1()
                                                                            : juce::AudioChannelSet::discreteChannels(n);
    }
    for (int c = 0; c < n; ++c) {
        const auto type = layout.getTypeOfChannel(c);
        const auto name = juce::AudioChannelSet::getChannelTypeName(type);
        auto& g = w[static_cast<size_t>(c)];
        if (type == juce::AudioChannelSet::left) g = {1.f, 0.f};
        else if (type == juce::AudioChannelSet::right) g = {0.f, 1.f};
        else if (name.containsIgnoreCase("LFE")) g = {0.f, 0.f};
        else if (name.containsIgnoreCase("Centre") || name.containsIgnoreCase("Center")) {
            if (name.containsIgnoreCase("Left")) g = {kMinus3dB, 0.f};
            else if (name.containsIgnoreCase("Right")) g = {0.f, kMinus3dB};
            else g = {kMinus3dB, kMinus3dB};
        } else if (name.containsIgnoreCase("Left")) g = {kMinus3dB, 0.f};
        else if (name.containsIgnoreCase("Right")) g = {0.f, kMinus3dB};
        else g = {0.5f, 0.5f};  // unlabelled/discrete: spread evenly
    }
    return w;
}

#if JUCE_WINDOWS
// Formats Windows Media Foundation reads better than JUCE's own decoders: it finds the
// audio stream in video containers (WMV, MP4) and decodes AAC.
const juce::StringArray kMediaFoundationFirst{"wma", "wmv", "asf", "mp4", "m4a", "aac", "mov"};

// Interleaved buffer with any channel count -> mono/stereo (standard channel order).
ynot::AudioBuffer foldToStereo(ynot::AudioBuffer in) {
    const int n = in.channels;
    if (n <= 2) return in;
    const auto w = downmixWeights({}, n);
    ynot::AudioBuffer out;
    out.sampleRate = in.sampleRate;
    out.channels = 2;
    const auto frames = static_cast<size_t>(in.frames());
    out.samples.resize(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        float l = 0, r = 0;
        for (int c = 0; c < n; ++c) {
            const float v = in.samples[f * static_cast<size_t>(n) + static_cast<size_t>(c)];
            l += v * w[static_cast<size_t>(c)].first;
            r += v * w[static_cast<size_t>(c)].second;
        }
        out.samples[f * 2] = l;
        out.samples[f * 2 + 1] = r;
    }
    return out;
}
#endif

juce::String unsupportedReason(const juce::File& f) {
    const auto ext = f.getFileExtension().toLowerCase().trimCharactersAtStart(".");
#if JUCE_MAC || JUCE_IOS
    if (ext == "wmv" || ext == "wma" || ext == "asf")
        return "Windows Media files (." + ext + ") can't be read on Apple devices. Import it on a Windows "
               "PC (the audio then plays everywhere in the project), or convert it to MP3, M4A or WAV.";
#endif
#if !(JUCE_MAC || JUCE_IOS || JUCE_WINDOWS)
    if (ext == "mp4" || ext == "m4a" || ext == "aac" || ext == "mov")
        return "This device can't decode ." + ext + " (AAC) files yet. Import it on a Mac or Windows PC, or "
               "convert it to MP3 or WAV.";
    if (ext == "wmv" || ext == "wma" || ext == "asf")
        return "Windows Media files (." + ext + ") can only be read on Windows. Import it there, or convert "
               "it to MP3 or WAV.";
#endif
    return "Ynot couldn't read \"" + f.getFileName() +
           "\". It may be damaged, have no audio track, or use a format this device can't decode.";
}
}  // namespace

juce::String importFilePatterns() {
    juce::StringArray p;
    for (auto& e : kExtensions) p.add("*." + e);
    return p.joinIntoString(";");
}

bool looksLikeImportableFile(const juce::File& f) {
    return kExtensions.contains(f.getFileExtension().toLowerCase().trimCharactersAtStart("."));
}

DecodedAudio decodeAudioFile(const juce::File& file) {
    DecodedAudio out;
    if (!file.existsAsFile()) {
        out.error = "File not found: " + file.getFullPathName();
        return out;
    }
#if JUCE_WINDOWS
    juce::String mediaFoundationError;
    if (kMediaFoundationFirst.contains(file.getFileExtension().toLowerCase().trimCharactersAtStart("."))) {
        ynot::AudioBuffer raw;
        std::string err;
        if (decodeWithMediaFoundation(std::wstring(file.getFullPathName().toWideCharPointer()), kMaxFrames, raw, err)) {
            out.audio = foldToStereo(std::move(raw));
            out.formatName = "Windows Media Foundation";
            out.ok = true;
            return out;
        }
        mediaFoundationError = juce::String(err);  // try JUCE's decoders before giving up
    }
#endif
    auto& fm = formats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (!reader) {
        // Containers often have extensions a decoder doesn't claim (.wmv, .mp4 with video).
        // Only the OS media decoders (Core Audio, Windows Media) may sniff them: permissive
        // decoders such as MP3 would "decode" arbitrary data into noise.
        for (int i = 0; i < fm.getNumKnownFormats(); ++i) {
            auto* f = fm.getKnownFormat(i);
            const auto name = f->getFormatName();
            if (!name.containsIgnoreCase("CoreAudio") && !name.containsIgnoreCase("Core Audio") &&
                !name.containsIgnoreCase("Windows Media"))
                continue;
            std::unique_ptr<juce::InputStream> in(file.createInputStream());
            if (!in) break;
            if (auto* r = f->createReaderFor(in.release(), true)) {
                reader.reset(r);
                break;
            }
        }
    }
    if (!reader || reader->lengthInSamples <= 0 || reader->sampleRate <= 0) {
        out.error = unsupportedReason(file);
#if JUCE_WINDOWS
        if (mediaFoundationError.isNotEmpty())
            out.error = "Ynot couldn't read \"" + file.getFileName() + "\": " + mediaFoundationError;
#endif
        return out;
    }
    if (reader->lengthInSamples > kMaxFrames) {
        out.error = "\"" + file.getFileName() + "\" is longer than an hour; split it before importing.";
        return out;
    }
    const int inChannels = static_cast<int>(reader->numChannels);
    const int channels = inChannels >= 2 ? 2 : 1;
    const auto frames = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buf(std::max(2, inChannels), frames);
    if (!reader->read(&buf, 0, frames, 0, true, true)) {
        out.error = "Reading \"" + file.getFileName() + "\" failed partway through.";
        return out;
    }
    // Stereo/mono pass straight through. Surround is folded down to stereo with the usual
    // coefficients: centre and surrounds at -3 dB into their side(s), LFE left out — so a
    // film's dialogue (centre channel) isn't lost.
    const auto weights = downmixWeights(reader->getChannelLayout(), inChannels);
    out.audio.sampleRate = reader->sampleRate;
    out.audio.channels = channels;
    out.audio.samples.resize(static_cast<size_t>(frames) * static_cast<size_t>(channels));
    for (int i = 0; i < frames; ++i) {
        if (channels == 1) {
            out.audio.samples[static_cast<size_t>(i)] = buf.getSample(0, i);
            continue;
        }
        float l = 0, r = 0;
        for (int c = 0; c < inChannels; ++c) {
            const float v = buf.getSample(c, i);
            l += v * weights[static_cast<size_t>(c)].first;
            r += v * weights[static_cast<size_t>(c)].second;
        }
        out.audio.samples[static_cast<size_t>(i * 2)] = l;
        out.audio.samples[static_cast<size_t>(i * 2 + 1)] = r;
    }
    out.formatName = reader->getFormatName();
    out.ok = true;
    return out;
}

}  // namespace ynotapp
