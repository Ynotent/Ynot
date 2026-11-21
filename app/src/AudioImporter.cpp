#include "AudioImporter.h"

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
        return out;
    }
    constexpr juce::int64 kMaxFrames = 48000LL * 60 * 60;  // one hour
    if (reader->lengthInSamples > kMaxFrames) {
        out.error = "\"" + file.getFileName() + "\" is longer than an hour; split it before importing.";
        return out;
    }
    const int channels = reader->numChannels >= 2 ? 2 : 1;  // surround: keep the front pair
    const auto frames = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buf(static_cast<int>(std::max(2u, reader->numChannels)), frames);
    if (!reader->read(&buf, 0, frames, 0, true, true)) {
        out.error = "Reading \"" + file.getFileName() + "\" failed partway through.";
        return out;
    }
    out.audio.sampleRate = reader->sampleRate;
    out.audio.channels = channels;
    out.audio.samples.resize(static_cast<size_t>(frames) * static_cast<size_t>(channels));
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            out.audio.samples[static_cast<size_t>(i * channels + c)] = buf.getSample(c, i);
    out.formatName = reader->getFormatName();
    out.ok = true;
    return out;
}

}  // namespace ynotapp
