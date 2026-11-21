#include "ynot/demo.hpp"

#include <cmath>

#include "ynot/engine.hpp"
#include "ynot/project_format.hpp"

namespace ynot {

PluginInstance makeBuiltin(const std::string& uid, const std::string& name) {
    PluginInstance p;
    p.id = newId();
    p.name = name;
    p.vendor = "Ynot";
    p.format = PluginFormat::Builtin;
    p.uid = uid;
    p.version = "1";
    return p;
}

PluginInstance makeExternal(PluginFormat f, const std::string& uid, const std::string& name,
                            const std::string& vendor) {
    PluginInstance p;
    p.id = newId();
    p.name = name;
    p.vendor = vendor;
    p.format = f;
    p.uid = uid;
    p.version = "1.0.0";
    p.stateBase64 = "AAECAwQ=";  // opaque chunk carried untouched
    return p;
}

Project makeDemoProject(const std::string& packageDir) {
    Project p;
    p.id = newId();
    p.name = "Demo Song";
    p.tempoBpm = 120;

    // "Recorded" vocal take: 2 s, 44.1 kHz (as if captured on a desktop interface).
    AudioBuffer take;
    take.sampleRate = 44100;
    take.channels = 1;
    for (int i = 0; i < 88200; ++i)
        take.samples.push_back(static_cast<float>(0.3 * std::sin(2 * 3.14159265358979 * 220.0 * i / 44100.0)));
    std::string mediaId = addMedia(p, packageDir, take, "recording").id;

    Track vox;
    vox.id = newId();
    vox.name = "Vocals";
    vox.kind = TrackKind::Audio;
    vox.order = 1;
    vox.audioClips.push_back({newId(), mediaId, 0, 4 * kTicksPerQuarter, 0, 0.0, {}, 0, 1.0, {}});
    auto gain = makeBuiltin(kBuiltinGain, "Gain");
    gain.params["gainDb"] = -3.0;
    vox.inserts.push_back(gain);
    vox.automation.push_back({"track.gainDb", {{0, -6.0}, {4 * kTicksPerQuarter, 0.0}}});
    p.tracks.push_back(vox);

    Track keys;
    keys.id = newId();
    keys.name = "Keys";
    keys.kind = TrackKind::Instrument;
    keys.order = 2;
    keys.color = "#2BD9A8";
    keys.instrument = makeBuiltin(kBuiltinSynth, "Ynot Synth");
    MidiClip mc{newId(), 0, 8 * kTicksPerQuarter, {}};
    int chord[] = {60, 64, 67};
    for (int bar = 0; bar < 2; ++bar)
        for (int n : chord) mc.notes.push_back({bar * 4 * kTicksPerQuarter, 3 * kTicksPerQuarter, n + bar * 5, 96});
    keys.midiClips.push_back(mc);
    auto verb = makeExternal(PluginFormat::VST3, "ACME-PROVERB-0001", "Acme ProVerb", "Acme Audio");
    verb.seenOn = {Platform::Windows, Platform::MacOS};
    keys.inserts.push_back(verb);
    p.tracks.push_back(keys);

    Track strings;
    strings.id = newId();
    strings.name = "Strings";
    strings.kind = TrackKind::Instrument;
    strings.order = 3;
    strings.color = "#FFB547";
    strings.instrument = makeExternal(PluginFormat::AudioUnit, "aumu:Orch:Acme", "Acme Orchestra", "Acme Audio");
    strings.instrument->seenOn = {Platform::MacOS};
    MidiClip sc{newId(), 0, 8 * kTicksPerQuarter, {{0, 8 * kTicksPerQuarter, 48, 80}}};
    strings.midiClips.push_back(sc);
    p.tracks.push_back(strings);

    return p;
}

}  // namespace ynot
