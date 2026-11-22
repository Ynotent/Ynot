#include "MediaFoundationDecoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace ynotapp {

namespace {
// Minimal owning COM pointer.
template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T** put() {
        if (p) {
            p->Release();
            p = nullptr;
        }
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// COM and Media Foundation for the duration of one decode (the caller may be any thread).
struct MfSession {
    bool com = false, mf = false;
    MfSession() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: already initialised as STA, which MF also accepts
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MfSession() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};
}  // namespace

bool decodeWithMediaFoundation(const std::wstring& path, int64_t maxFrames, ynot::AudioBuffer& out,
                               std::string& error) {
    MfSession session;
    if (!session.mf) {
        error = "Windows Media Foundation isn't available on this PC. On Windows \"N\" editions, install the "
                "Media Feature Pack from Windows Settings > Apps > Optional features.";
        return false;
    }
    Com<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put()))) {
        error = "Windows couldn't open this file.";
        return false;
    }
    // Only the first audio stream: video tracks are skipped entirely.
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
        error = "This file has no audio track.";
        return false;
    }
    Com<IMFMediaType> want;
    if (FAILED(MFCreateMediaType(want.put())) || FAILED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
        FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want.p))) {
        error = "Windows has no decoder for this file's audio.";
        return false;
    }
    Com<IMFMediaType> actual;
    UINT32 channels = 0, rate = 0, bits = 0;
    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), actual.put())) ||
        FAILED(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
        FAILED(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || channels == 0 || rate == 0) {
        error = "Windows couldn't read this file's audio format.";
        return false;
    }
    actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (bits != 0 && bits != 32) {
        error = "Windows returned an unexpected sample format.";
        return false;
    }

    out = {};
    out.sampleRate = static_cast<double>(rate);
    out.channels = static_cast<int>(channels);
    for (;;) {
        DWORD flags = 0;
        Com<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
                                              &flags, nullptr, sample.put());
        if (FAILED(hr)) {
            error = "Reading the file failed partway through.";
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            error = "The audio format changes partway through this file, which isn't supported.";
            return false;
        }
        if (!sample) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            continue;
        }
        Com<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            const size_t count = length / sizeof(float);
            const size_t old = out.samples.size();
            out.samples.resize(old + count);
            std::memcpy(out.samples.data() + old, data, count * sizeof(float));
            buffer->Unlock();
        }
        if (out.frames() > maxFrames) {
            error = "This file is longer than an hour; split it before importing.";
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    // Whole frames only.
    out.samples.resize(static_cast<size_t>(out.frames()) * channels);
    if (out.frames() <= 0) {
        error = "This file's audio track is empty.";
        return false;
    }
    // Windows' WMA decoder drops the final partial codec frame (up to ~46 ms at 44.1 kHz:
    // CI measured 20480 of 22050 frames). Pad with silence up to the file's stated duration
    // so the clip keeps its true length on the timeline; never pad by more than 250 ms, in
    // case a container's duration also counts other streams.
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                   &duration)) &&
        duration.vt == VT_UI8) {
        const auto expected =
            static_cast<int64_t>(static_cast<double>(duration.uhVal.QuadPart) * 1e-7 * static_cast<double>(rate) + 0.5);
        const int64_t missing = expected - out.frames();
        if (missing > 0 && missing <= static_cast<int64_t>(rate) / 4)
            out.samples.resize(static_cast<size_t>(expected) * channels, 0.f);
    }
    PropVariantClear(&duration);
    return true;
}

}  // namespace ynotapp
