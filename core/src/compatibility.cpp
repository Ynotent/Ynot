#include "ynot/compatibility.hpp"

#include <chrono>
#include <ctime>
#include <stdexcept>

#include "ynot/project_format.hpp"

namespace ynot {

bool DeviceCatalog::has(const PluginInstance& p) const {
    if (p.format == PluginFormat::Builtin) return BuiltinPluginHost{}.isAvailable(p);
    if (!platformLoadsFormat(platform, p.format)) return false;
    if (p.format == PluginFormat::AndroidAAP && !aapEnabled) return false;
    return installed.count({p.format, p.uid}) > 0;
}

bool CompatReport::fullyCompatible() const {
    for (auto& i : issues)
        if (i.severity != Severity::Info) return false;
    return true;
}

int CompatReport::count(Severity s) const {
    int n = 0;
    for (auto& i : issues) n += i.severity == s;
    return n;
}

bool hasFreshFreeze(const Project& p, const Track& t) {
    // A freeze is audio: it goes stale when the track or the song's tempo changes.
    return t.freeze && t.freeze->sourceFingerprint == trackFingerprint(t) &&
           (t.freeze->tempoSignature.empty() || t.freeze->tempoSignature == p.tempoSignature());
}

namespace {

std::vector<const PluginInstance*> activePlugins(const Track& t) {
    std::vector<const PluginInstance*> v;
    if (t.instrument && !t.instrument->bypassed) v.push_back(&*t.instrument);
    for (auto& p : t.inserts)
        if (!p.bypassed) v.push_back(&p);
    return v;
}


std::string platformLabel(Platform p) {
    switch (p) {
        case Platform::MacOS: return "macOS";
        case Platform::Windows: return "Windows";
        case Platform::IOS: return "iPhone/iPad";
        case Platform::Android: return "Android";
        case Platform::Linux: return "Linux";
    }
    return "this device";
}

std::string seenOnText(const PluginInstance& p) {
    if (p.seenOn.empty()) return "";
    std::string s = " It was last used on ";
    bool first = true;
    for (Platform pl : p.seenOn) {
        if (!first) s += ", ";
        s += platformLabel(pl);
        first = false;
    }
    return s + ".";
}

std::string formatLabel(PluginFormat f) {
    switch (f) {
        case PluginFormat::VST3: return "VST3";
        case PluginFormat::AudioUnit: return "Audio Unit";
        case PluginFormat::AUv3: return "AUv3";
        case PluginFormat::AndroidAAP: return "Android AAP";
        case PluginFormat::Builtin: return "built-in";
    }
    return "plugin";
}

}  // namespace

CompatReport analyzeCompatibility(const Project& p, const DeviceCatalog& target) {
    CompatReport report{target.platform, {}};
    for (const Track* tp : p.orderedTracks()) {
        const Track& t = *tp;
        std::vector<const PluginInstance*> missing;
        for (auto* pl : activePlugins(t))
            if (!target.has(*pl)) missing.push_back(pl);

        bool fresh = hasFreshFreeze(p, t);
        if (t.freeze && !fresh) {
            report.issues.push_back(
                {Severity::Warning, IssueKind::StaleFreeze, t.id, t.name, "", "",
                 "The frozen audio for '" + t.name + "' was rendered before later edits.",
                 {Resolution::FreezeOnSourceDevice, Resolution::UseExistingFreeze}});
        }
        if (missing.empty()) continue;

        if (fresh) {
            report.issues.push_back(
                {Severity::Info, IssueKind::FrozenOK, t.id, t.name, "", "",
                 "'" + t.name + "' uses plugins that aren't available on " + platformLabel(target.platform) +
                     ", but it is frozen and will play exactly as rendered. Editing its sound requires a "
                     "device with the plugins.",
                 {Resolution::UseExistingFreeze}});
            continue;
        }

        for (auto* pl : missing) {
            bool isInstrument = t.instrument && pl == &*t.instrument;
            bool formatOk = platformLoadsFormat(target.platform, pl->format);
            CompatIssue issue;
            issue.kind = formatOk ? IssueKind::NotInstalled : IssueKind::FormatUnsupported;
            issue.severity = isInstrument ? Severity::Blocking : Severity::Warning;
            issue.trackId = t.id;
            issue.trackName = t.name;
            issue.pluginId = pl->id;
            issue.pluginName = pl->name;
            if (formatOk)
                issue.message = "'" + pl->name + "' (" + formatLabel(pl->format) +
                                ") is not installed on this device.";
            else
                issue.message = "'" + pl->name + "' is " +
                                (formatLabel(pl->format)[0] == 'A' ? "an " : "a ") +
                                formatLabel(pl->format) + " plugin. " +
                                formatLabel(pl->format) + " plugins can't run on " +
                                platformLabel(target.platform) + ".";
            issue.message += seenOnText(*pl);
            issue.message += isInstrument ? " Without it, '" + t.name + "' will be silent."
                                          : " It will be bypassed.";
            if (formatOk) issue.options.push_back(Resolution::InstallPlugin);
            issue.options.push_back(Resolution::FreezeOnSourceDevice);
            issue.options.push_back(Resolution::RenderToAudio);
            if (isInstrument) issue.options.push_back(Resolution::ReplaceWithBuiltin);
            else issue.options.push_back(Resolution::BypassPlugin);
            report.issues.push_back(std::move(issue));
        }
    }
    return report;
}

std::vector<std::string> tracksNeedingFreezeFor(const Project& p, const DeviceCatalog& here,
                                                const std::vector<DeviceCatalog>& targets) {
    std::vector<std::string> out;
    for (const Track* t : p.orderedTracks()) {
        if (hasFreshFreeze(p, *t)) continue;
        auto plugins = activePlugins(*t);
        bool loadsHere = true, missingSomewhere = false;
        for (auto* pl : plugins) {
            loadsHere &= here.has(*pl);
            for (auto& tg : targets) missingSomewhere |= !tg.has(*pl);
        }
        if (missingSomewhere && loadsHere) out.push_back(t->id);
    }
    return out;
}

static std::string isoNow() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void freezeTrack(Project& p, const std::string& packageDir, const std::string& trackId,
                 MediaProvider& media, PluginHost& host, Platform renderedOn, double sampleRate) {
    Track* t = p.findTrack(trackId);
    if (!t) throw std::runtime_error("freeze: no such track");
    for (auto* pl : activePlugins(*t))
        if (!host.isAvailable(*pl))
            throw std::runtime_error("freeze: '" + pl->name + "' is not available on this device");
    AudioBuffer audio = renderTrackPreFader(p, trackId, media, host, sampleRate);
    std::string mediaId = addMedia(p, packageDir, audio, "freeze").id;
    t = p.findTrack(trackId);
    FreezeInfo fi;
    fi.mediaId = mediaId;
    fi.sourceFingerprint = trackFingerprint(*t);
    fi.tempoSignature = p.tempoSignature();
    fi.renderedOn = renderedOn;
    fi.renderedAt = isoNow();
    t->freeze = fi;
    t->frozen = true;
    for (auto* pl : activePlugins(*t)) const_cast<PluginInstance*>(pl)->seenOn.insert(renderedOn);
}

void unfreezeTrack(Project& p, const std::string& trackId) {
    if (Track* t = p.findTrack(trackId)) t->frozen = false;  // keep render for quick refreeze/handoff
}

std::string toString(Resolution r) {
    switch (r) {
        case Resolution::UseExistingFreeze: return "Play frozen audio";
        case Resolution::FreezeOnSourceDevice: return "Freeze on a device that has the plugin";
        case Resolution::RenderToAudio: return "Render track to audio (permanent)";
        case Resolution::ReplaceWithBuiltin: return "Replace with a Ynot built-in instrument";
        case Resolution::BypassPlugin: return "Continue without this effect";
        case Resolution::InstallPlugin: return "Install the plugin on this device";
        case Resolution::UpdateApp: return "Update Ynot on this device";
    }
    return "";
}

std::string toString(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Blocking: return "blocking";
    }
    return "";
}

}  // namespace ynot
