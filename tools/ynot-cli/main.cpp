// ynot — command-line front end to the shared core.
//
//   ynot demo   <package-dir>                 create a demo project
//   ynot info   <package-dir>                 summarize a project
//   ynot check  <package-dir> <platform>...   compatibility report (macos|windows|ios|android)
//   ynot render <package-dir> <out.wav> [rate] offline mixdown with builtin processors
//   ynot platforms                            platform capability matrix
#include <cstdio>
#include <iostream>

#include "ynot/ynot.hpp"

using namespace ynot;

static int usage() {
    std::cerr << "usage: ynot demo|info|check|render|platforms ...\n";
    return 2;
}

static int cmdInfo(const std::string& dir) {
    Project p = loadProjectPackage(dir);
    std::printf("%s  (%.1f BPM, %d/%d, format v%d)\n", p.name.c_str(), p.tempoBpm, p.timeSigNumerator,
                p.timeSigDenominator, p.formatVersion);
    for (auto* t : p.orderedTracks()) {
        std::printf("  [%s] %-10s %s clips:%zu midi:%zu inserts:%zu%s\n",
                    t->kind == TrackKind::Audio ? "audio" : "inst ", t->name.c_str(),
                    t->instrument ? ("<" + t->instrument->name + ">").c_str() : "",
                    t->audioClips.size(), t->midiClips.size(), t->inserts.size(),
                    t->frozen ? "  FROZEN" : "");
    }
    std::printf("  media: %zu file(s)\n", p.media.size());
    return 0;
}

static int cmdCheck(const std::string& dir, const std::vector<std::string>& targets) {
    Project p = loadProjectPackage(dir);
    int worst = 0;
    for (auto& name : targets) {
        auto pl = platformFromString(name);
        if (!pl) { std::cerr << "unknown platform " << name << "\n"; return 2; }
        CompatReport r = analyzeCompatibility(p, DeviceCatalog::builtinOnly(*pl));
        std::printf("\n== %s: %s\n", name.c_str(),
                    r.fullyCompatible() ? "fully compatible" : "needs attention");
        for (auto& i : r.issues) {
            std::printf("  [%s] %s\n", toString(i.severity).c_str(), i.message.c_str());
            for (auto o : i.options) std::printf("      -> %s\n", toString(o).c_str());
            worst = std::max(worst, static_cast<int>(i.severity));
        }
    }
    return worst == static_cast<int>(Severity::Blocking) ? 1 : 0;
}

static int cmdRender(const std::string& dir, const std::string& out, double rate) {
    Project p = loadProjectPackage(dir);
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    std::vector<EngineIssue> issues;
    AudioBuffer buf = renderProject(p, media, host, rate, 1.0, &issues);
    for (auto& i : issues) std::cerr << "warning: " << i.message << "\n";
    writeFileBytes(out, encodeWavFloat32(buf));
    std::printf("wrote %s (%.2f s @ %.0f Hz)\n", out.c_str(), static_cast<double>(buf.frames()) / rate, rate);
    return 0;
}

static int cmdPlatforms() {
    for (Platform p : allTargetPlatforms()) {
        auto& prof = profileFor(p);
        std::printf("%-8s audio:", toString(p).c_str());
        for (auto a : prof.audioApis) std::printf(" %s", toString(a).c_str());
        std::printf("  plugins:");
        for (auto f : prof.pluginFormats) std::printf(" %s", toString(f).c_str());
        std::printf("\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "platforms") return cmdPlatforms();
        if (argc < 3) return usage();
        std::string dir = argv[2];
        if (cmd == "demo") {
            Project p = makeDemoProject(dir);
            saveProjectPackage(p, dir);
            std::printf("created %s\n", dir.c_str());
            return 0;
        }
        if (cmd == "info") return cmdInfo(dir);
        if (cmd == "check") {
            std::vector<std::string> targets(argv + 3, argv + argc);
            if (targets.empty()) targets = {"macos", "windows", "ios", "android"};
            return cmdCheck(dir, targets);
        }
        if (cmd == "render" && argc >= 4) return cmdRender(dir, argv[3], argc >= 5 ? std::atof(argv[4]) : 48000);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
