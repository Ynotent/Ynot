// Windows only: decodes the first audio stream of any file Windows Media Foundation can
// open — WMA, WMV/ASF (audio from video too), MP4/M4A/AAC, MP3 — to 32-bit float.
// Kept free of JUCE headers so the Windows SDK headers it needs don't clash.
#pragma once

#include <cstdint>
#include <string>

#include "ynot/media.hpp"

namespace ynotapp {

// `out` keeps the file's channel count (surround is folded down by the caller).
// Returns false with a user-facing `error` if the file can't be decoded.
bool decodeWithMediaFoundation(const std::wstring& path, int64_t maxFrames, ynot::AudioBuffer& out,
                               std::string& error);

// Writes stereo float audio as M4A (AAC) or MP3 with Windows' own encoders (Windows 8+).
bool encodeWithMediaFoundation(const std::wstring& path, const ynot::AudioBuffer& stereo, bool mp3, int bitrateKbps,
                               std::string& error);

}  // namespace ynotapp
