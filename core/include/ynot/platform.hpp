// Platform capability profiles.
//
// Every platform difference the shared core needs to reason about (plugin
// formats, audio APIs, input methods, form factors) is described here as data,
// so compatibility checks, UI layout and the roadmap all use one source of truth.
// Nothing in this file claims a platform is *shipped*: see PlatformStatus.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ynot {

enum class Platform { MacOS, Windows, IOS, Android, Linux /* developer/CI host only */ };

enum class PluginFormat {
    Builtin,    // Ynot's own processors: portable C++, available everywhere
    VST3,       // macOS + Windows desktop hosts
    AudioUnit,  // AUv2, macOS only
    AUv3,       // iOS/iPadOS, and macOS (AUv3 app extensions)
    AndroidAAP, // "Audio Plugins for Android" — experimental, evaluation only
};

enum class AudioApi { CoreAudio, AVAudioSession, ASIO, WASAPI, AAudio, OpenSLES, Null };

enum class FormFactor { Phone, Tablet, Desktop };

enum class InputMode { Touch, Pencil, Mouse, Keyboard, MidiController };

// Delivery state of a platform. The core never reports a platform as
// supported unless it has reached RealDeviceVerified (see docs/ROADMAP.md).
enum class PlatformStatus {
    Planned,            // in the architecture, no native shell yet
    CoreCompiles,       // shared core builds for this target in CI
    NativeShellBuilds,  // native app shell builds in CI
    RealDeviceVerified, // passed the real-device test matrix for its milestone
};

struct PlatformProfile {
    Platform platform;
    std::vector<PluginFormat> pluginFormats;  // formats the host can load
    std::vector<AudioApi> audioApis;          // preferred first
    std::vector<FormFactor> formFactors;
    std::vector<InputMode> inputModes;
    bool supportsBackgroundAudio;
    bool supportsMultiChannelInterfaces;      // class-compliant / pro interfaces
    int recommendedMaxTracks;                 // UI guidance, not a hard limit
};

const PlatformProfile& profileFor(Platform p);
const std::vector<Platform>& allTargetPlatforms();  // the four shipping targets

bool platformLoadsFormat(Platform p, PluginFormat f);

std::string toString(Platform p);
std::string toString(PluginFormat f);
std::string toString(AudioApi a);
std::string toString(PlatformStatus s);
std::optional<Platform> platformFromString(const std::string& s);
std::optional<PluginFormat> pluginFormatFromString(const std::string& s);

// Compile-time host platform of the running binary.
Platform currentPlatform();

}  // namespace ynot
