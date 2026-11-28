// macOS / iOS only: writes stereo float audio as an M4A (AAC) file with Apple's encoder.
// Kept free of JUCE headers.
#pragma once

#include <string>

#include "ynot/media.hpp"

namespace ynotapp {

bool encodeAacWithCoreAudio(const std::string& utf8Path, const ynot::AudioBuffer& stereo, int bitrateKbps,
                            std::string& error);

}  // namespace ynotapp
