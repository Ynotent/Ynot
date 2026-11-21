#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
const CompatIssue* issueFor(const CompatReport& r, const std::string& trackName) {
    for (auto& i : r.issues)
        if (i.trackName == trackName) return &i;
    return nullptr;
}
bool hasOption(const CompatIssue& i, Resolution r) {
    for (auto o : i.options)
        if (o == r) return true;
    return false;
}
}  // namespace

TEST("compat: desktop plugins are never assumed on iOS or Android") {
    auto dir = th::tempDir("c-mobile");
    Project p = makeDemoProject(dir);
    for (Platform mobile : {Platform::IOS, Platform::Android}) {
        // Even a (hypothetical) catalog claiming the VST3/AU is "installed" can't load it.
        DeviceCatalog cat{mobile, {}, true};
        cat.installed.insert({PluginFormat::VST3, "ACME-PROVERB-0001"});
        cat.installed.insert({PluginFormat::AudioUnit, "aumu:Orch:Acme"});
        CompatReport r = analyzeCompatibility(p, cat);
        CHECK(!r.fullyCompatible());
        auto* keys = issueFor(r, "Keys");
        auto* strings = issueFor(r, "Strings");
        CHECK(keys && strings);
        CHECK(keys->kind == IssueKind::FormatUnsupported);
        CHECK(keys->severity == Severity::Warning);  // effect: bypassed
        CHECK(strings->severity == Severity::Blocking);  // instrument: silent
        CHECK(hasOption(*strings, Resolution::FreezeOnSourceDevice));
        CHECK(hasOption(*strings, Resolution::RenderToAudio));
        CHECK(hasOption(*strings, Resolution::ReplaceWithBuiltin));
        CHECK(!hasOption(*strings, Resolution::InstallPlugin));
        CHECK(strings->message.find("last used on macOS") != std::string::npos);
        CHECK(issueFor(r, "Vocals") == nullptr);  // builtins work everywhere
    }
}

TEST("compat: Windows loads VST3 when installed but never AU") {
    auto dir = th::tempDir("c-win");
    Project p = makeDemoProject(dir);
    DeviceCatalog win{Platform::Windows, {}, false};
    CompatReport bare = analyzeCompatibility(p, win);
    auto* keys = issueFor(bare, "Keys");
    CHECK(keys && keys->kind == IssueKind::NotInstalled);
    CHECK(hasOption(*keys, Resolution::InstallPlugin));

    win.installed.insert({PluginFormat::VST3, "ACME-PROVERB-0001"});
    win.installed.insert({PluginFormat::AudioUnit, "aumu:Orch:Acme"});
    CompatReport r = analyzeCompatibility(p, win);
    CHECK(issueFor(r, "Keys") == nullptr);
    CHECK(issueFor(r, "Strings")->kind == IssueKind::FormatUnsupported);
}

TEST("compat: macOS with plugins installed is fully compatible") {
    auto dir = th::tempDir("c-mac");
    Project p = makeDemoProject(dir);
    CHECK(analyzeCompatibility(p, th::macWithAcme()).fullyCompatible());
}

TEST("compat: prepare-for-mobile freezes tracks so they play on iPhone and Android") {
    auto dir = th::tempDir("c-freeze");
    Project p = makeDemoProject(dir);
    auto here = th::macWithAcme();
    std::vector<DeviceCatalog> targets{DeviceCatalog::builtinOnly(Platform::IOS),
                                       DeviceCatalog::builtinOnly(Platform::Android)};
    auto toFreeze = tracksNeedingFreezeFor(p, here, targets);
    CHECK_EQ(toFreeze.size(), size_t(2));

    PackageMediaProvider media(dir);
    th::FakeDesktopHost macHost(Platform::MacOS);
    for (auto& id : toFreeze) freezeTrack(p, dir, id, media, macHost, Platform::MacOS);
    saveProjectPackage(p, dir);
    CHECK(tracksNeedingFreezeFor(p, here, targets).empty());

    // Open on "iPhone": reload from disk, only builtins available.
    Project onPhone = loadProjectPackage(dir);
    CompatReport r = analyzeCompatibility(onPhone, DeviceCatalog::builtinOnly(Platform::IOS));
    CHECK(r.fullyCompatible());
    CHECK(issueFor(r, "Strings")->kind == IssueKind::FrozenOK);

    // And it actually sounds: the AU-only Strings track plays from its freeze.
    for (auto& t : onPhone.tracks) t.solo = t.name == "Strings";
    PackageMediaProvider phoneMedia(dir);
    BuiltinPluginHost phoneHost;
    std::vector<EngineIssue> issues;
    AudioBuffer out = renderProject(onPhone, phoneMedia, phoneHost, 48000, 0.0, &issues);
    CHECK(th::rms(out, 0, 48000) > 0.05);
    CHECK(issues.empty());

    // Plugin state survives the trip so it can be unfrozen back on the Mac.
    CHECK_EQ(onPhone.tracks[2].instrument->stateBase64, std::string("AAECAwQ="));
}

TEST("compat: editing a frozen track marks the freeze stale") {
    auto dir = th::tempDir("c-stale");
    Project p = makeDemoProject(dir);
    PackageMediaProvider media(dir);
    th::FakeDesktopHost macHost(Platform::MacOS);
    std::string sid = p.tracks[2].id;
    freezeTrack(p, dir, sid, media, macHost, Platform::MacOS);
    p.findTrack(sid)->gainDb = -4;  // mix change: still fresh
    CHECK(analyzeCompatibility(p, DeviceCatalog::builtinOnly(Platform::Android)).count(Severity::Blocking) == 0);
    p.findTrack(sid)->midiClips[0].notes[0].pitch = 50;  // sound change: stale
    CompatReport r = analyzeCompatibility(p, DeviceCatalog::builtinOnly(Platform::Android));
    bool stale = false;
    for (auto& i : r.issues) stale |= i.kind == IssueKind::StaleFreeze;
    CHECK(stale);
    CHECK(r.count(Severity::Blocking) == 1);
}

TEST("compat: freezing requires the plugin on this device") {
    auto dir = th::tempDir("c-nofreeze");
    Project p = makeDemoProject(dir);
    PackageMediaProvider media(dir);
    BuiltinPluginHost phoneHost;
    CHECK_THROWS(freezeTrack(p, dir, p.tracks[2].id, media, phoneHost, Platform::IOS));
}
