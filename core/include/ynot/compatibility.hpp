// Cross-device compatibility analysis.
//
// Answers, before a project is opened or handed off: "what will not work on
// that device, and what can the user do about it?" Desktop VST3/AU plugins are
// never assumed to exist on iOS/Android (see platform.hpp), and a plugin of a
// supported format still has to be installed on the specific device.
#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ynot/engine.hpp"
#include "ynot/model.hpp"
#include "ynot/platform.hpp"

namespace ynot {

// What is installed on one concrete device. Builtins are always present.
struct DeviceCatalog {
    Platform platform = Platform::Linux;
    std::set<std::pair<PluginFormat, std::string>> installed;  // (format, uid)
    bool aapEnabled = false;  // Android AAP hosting behind a feature flag

    bool has(const PluginInstance& p) const;
    static DeviceCatalog builtinOnly(Platform p) { return DeviceCatalog{p, {}, false}; }
};

enum class Severity { Info, Warning, Blocking };

enum class Resolution {
    UseExistingFreeze,     // track already has an up-to-date freeze: plays exactly as rendered
    FreezeOnSourceDevice,  // freeze (reversible) on a device where the plugin loads
    RenderToAudio,         // commit/bounce: replace the track source with audio permanently
    ReplaceWithBuiltin,    // swap in a portable builtin instrument/effect
    BypassPlugin,          // continue without the effect
    InstallPlugin,         // a compatible build of the plugin exists for this platform's format
    UpdateApp,             // project needs a newer app version
};

enum class IssueKind { FormatUnsupported, NotInstalled, StaleFreeze, MissingMedia, FrozenOK };

struct CompatIssue {
    Severity severity;
    IssueKind kind;
    std::string trackId;
    std::string trackName;
    std::string pluginId;    // empty for non-plugin issues
    std::string pluginName;
    std::string message;     // user-facing text
    std::vector<Resolution> options;  // ordered best-first
};

struct CompatReport {
    Platform target;
    std::vector<CompatIssue> issues;
    bool fullyCompatible() const;      // nothing above Info
    int count(Severity s) const;
};

CompatReport analyzeCompatibility(const Project& p, const DeviceCatalog& target);

// Tracks that must be frozen on *this* device (where the plugins load) so the
// project plays correctly on all of `targets` — the "Prepare for iPhone/Android" action.
std::vector<std::string> tracksNeedingFreezeFor(const Project& p, const DeviceCatalog& here,
                                                const std::vector<DeviceCatalog>& targets);

// Freezes a track: renders it pre-fader with `host`, stores the audio in the
// package media store, and marks it frozen. The plugin state is kept so the
// track can be unfrozen later on a device that has the plugin.
void freezeTrack(Project& p, const std::string& packageDir, const std::string& trackId,
                 MediaProvider& media, PluginHost& host, Platform renderedOn, double sampleRate = 48000);
void unfreezeTrack(Project& p, const std::string& trackId);

std::string toString(Resolution r);
std::string toString(Severity s);

}  // namespace ynot
