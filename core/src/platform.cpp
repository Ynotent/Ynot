#include "ynot/platform.hpp"

#include <algorithm>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ynot {

namespace {
const PlatformProfile kMac{
    Platform::MacOS,
    {PluginFormat::Builtin, PluginFormat::AudioUnit, PluginFormat::AUv3, PluginFormat::VST3},
    {AudioApi::CoreAudio},
    {FormFactor::Desktop},
    {InputMode::Mouse, InputMode::Keyboard, InputMode::MidiController},
    true, true, 256};

const PlatformProfile kWindows{
    Platform::Windows,
    {PluginFormat::Builtin, PluginFormat::VST3},
    {AudioApi::ASIO, AudioApi::WASAPI},
    {FormFactor::Desktop},
    {InputMode::Mouse, InputMode::Keyboard, InputMode::Touch, InputMode::MidiController},
    true, true, 256};

const PlatformProfile kIOS{
    Platform::IOS,
    // Desktop VST3 / AUv2 binaries cannot load on iOS. Only AUv3 extensions.
    {PluginFormat::Builtin, PluginFormat::AUv3},
    {AudioApi::AVAudioSession, AudioApi::CoreAudio},
    {FormFactor::Phone, FormFactor::Tablet},
    {InputMode::Touch, InputMode::Pencil, InputMode::Keyboard, InputMode::MidiController},
    true, true, 48};

const PlatformProfile kAndroid{
    Platform::Android,
    // No VST3/AU on Android. AAP is experimental; it is listed so projects can
    // round-trip such plugins, but the host gates loading behind a feature flag.
    {PluginFormat::Builtin, PluginFormat::AndroidAAP},
    {AudioApi::AAudio, AudioApi::OpenSLES},
    {FormFactor::Phone, FormFactor::Tablet},
    {InputMode::Touch, InputMode::Keyboard, InputMode::MidiController},
    true, true, 32};

const PlatformProfile kLinux{
    Platform::Linux,
    {PluginFormat::Builtin},
    {AudioApi::Null},
    {FormFactor::Desktop},
    {InputMode::Mouse, InputMode::Keyboard},
    false, false, 256};
}  // namespace

const PlatformProfile& profileFor(Platform p) {
    switch (p) {
        case Platform::MacOS: return kMac;
        case Platform::Windows: return kWindows;
        case Platform::IOS: return kIOS;
        case Platform::Android: return kAndroid;
        case Platform::Linux: return kLinux;
    }
    return kLinux;
}

const std::vector<Platform>& allTargetPlatforms() {
    static const std::vector<Platform> v{Platform::MacOS, Platform::Windows, Platform::IOS,
                                         Platform::Android};
    return v;
}

bool platformLoadsFormat(Platform p, PluginFormat f) {
    auto& fmts = profileFor(p).pluginFormats;
    return std::find(fmts.begin(), fmts.end(), f) != fmts.end();
}

std::string toString(Platform p) {
    switch (p) {
        case Platform::MacOS: return "macos";
        case Platform::Windows: return "windows";
        case Platform::IOS: return "ios";
        case Platform::Android: return "android";
        case Platform::Linux: return "linux";
    }
    return "unknown";
}

std::string toString(PluginFormat f) {
    switch (f) {
        case PluginFormat::Builtin: return "builtin";
        case PluginFormat::VST3: return "vst3";
        case PluginFormat::AudioUnit: return "au";
        case PluginFormat::AUv3: return "auv3";
        case PluginFormat::AndroidAAP: return "aap";
    }
    return "unknown";
}

std::string toString(AudioApi a) {
    switch (a) {
        case AudioApi::CoreAudio: return "CoreAudio";
        case AudioApi::AVAudioSession: return "AVAudioSession";
        case AudioApi::ASIO: return "ASIO";
        case AudioApi::WASAPI: return "WASAPI";
        case AudioApi::AAudio: return "AAudio";
        case AudioApi::OpenSLES: return "OpenSL ES";
        case AudioApi::Null: return "Null";
    }
    return "unknown";
}

std::string toString(PlatformStatus s) {
    switch (s) {
        case PlatformStatus::Planned: return "planned";
        case PlatformStatus::CoreCompiles: return "core-compiles";
        case PlatformStatus::NativeShellBuilds: return "native-shell-builds";
        case PlatformStatus::RealDeviceVerified: return "real-device-verified";
    }
    return "unknown";
}

std::optional<Platform> platformFromString(const std::string& s) {
    for (Platform p : {Platform::MacOS, Platform::Windows, Platform::IOS, Platform::Android,
                       Platform::Linux})
        if (toString(p) == s) return p;
    return std::nullopt;
}

std::optional<PluginFormat> pluginFormatFromString(const std::string& s) {
    for (PluginFormat f : {PluginFormat::Builtin, PluginFormat::VST3, PluginFormat::AudioUnit,
                           PluginFormat::AUv3, PluginFormat::AndroidAAP})
        if (toString(f) == s) return f;
    return std::nullopt;
}

Platform currentPlatform() {
#if defined(__ANDROID__)
    return Platform::Android;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return Platform::IOS;
#else
    return Platform::MacOS;
#endif
#elif defined(_WIN32)
    return Platform::Windows;
#else
    return Platform::Linux;
#endif
}

}  // namespace ynot
