#include "AppleAacEncoder.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <vector>

namespace ynotapp {

bool encodeAacWithCoreAudio(const std::string& path, const ynot::AudioBuffer& audio, int bitrateKbps,
                            std::string& error) {
    if (audio.channels != 2 || audio.frames() <= 0) {
        error = "Nothing to encode.";
        return false;
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), false);
    if (!url) {
        error = "Invalid file path.";
        return false;
    }
    AudioStreamBasicDescription aac{};
    aac.mSampleRate = audio.sampleRate;
    aac.mFormatID = kAudioFormatMPEG4AAC;
    aac.mChannelsPerFrame = 2;
    aac.mFramesPerPacket = 1024;
    ExtAudioFileRef file = nullptr;
    OSStatus st = ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &aac, nullptr, kAudioFileFlags_EraseFile, &file);
    CFRelease(url);
    if (st != noErr || !file) {
        error = "Couldn't create the M4A file (error " + std::to_string(static_cast<int>(st)) + ").";
        return false;
    }
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = audio.sampleRate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    pcm.mBitsPerChannel = 32;
    pcm.mChannelsPerFrame = 2;
    pcm.mFramesPerPacket = 1;
    pcm.mBytesPerFrame = 8;
    pcm.mBytesPerPacket = 8;
    st = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm);
    if (st == noErr) {
        // Bit rate (optional: the encoder's default is used if this is refused).
        AudioConverterRef conv = nullptr;
        UInt32 size = sizeof(conv);
        if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_AudioConverter, &size, &conv) == noErr && conv) {
            UInt32 bitrate = static_cast<UInt32>(std::clamp(bitrateKbps, 64, 320)) * 1000;
            if (AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate) == noErr) {
                CFArrayRef config = nullptr;
                ExtAudioFileSetProperty(file, kExtAudioFileProperty_ConverterConfig, sizeof(config), &config);
            }
        }
    }
    if (st != noErr) {
        ExtAudioFileDispose(file);
        error = "This device's AAC encoder refused the audio format (error " + std::to_string(static_cast<int>(st)) + ").";
        return false;
    }
    const int64_t frames = audio.frames();
    constexpr int64_t kChunk = 4096;
    std::vector<float> chunk(static_cast<size_t>(kChunk) * 2);
    for (int64_t done = 0; done < frames && st == noErr;) {
        const auto n = static_cast<UInt32>(std::min<int64_t>(kChunk, frames - done));
        std::copy(audio.samples.begin() + static_cast<std::ptrdiff_t>(done * 2),
                  audio.samples.begin() + static_cast<std::ptrdiff_t>((done + n) * 2), chunk.begin());
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 2;
        list.mBuffers[0].mDataByteSize = n * 8;
        list.mBuffers[0].mData = chunk.data();
        st = ExtAudioFileWrite(file, n, &list);
        done += n;
    }
    const OSStatus closed = ExtAudioFileDispose(file);
    if (st != noErr || closed != noErr) {
        error = "Encoding to M4A failed (error " + std::to_string(static_cast<int>(st != noErr ? st : closed)) + ").";
        return false;
    }
    return true;
}

}  // namespace ynotapp
