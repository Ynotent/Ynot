#include "ynot/project_format.hpp"

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ynot {

namespace {

// Keys this version understands, per object kind. Anything else is preserved
// verbatim in `extras` and written back on save.
const std::vector<std::string> kProjectKeys = {
    "format", "formatVersion", "id", "name", "tempoBpm", "tempoChanges", "timeSignature",
    "sampleRateHint", "tracks", "media"};
const std::vector<std::string> kTrackKeys = {
    "id", "name", "kind", "order", "gainDb", "pan", "mute", "solo", "armed", "color",
    "instrument", "inserts", "audioClips", "midiClips", "automation", "freeze", "frozen"};

bool known(const std::vector<std::string>& keys, const std::string& k) {
    for (auto& x : keys)
        if (x == k) return true;
    return false;
}

Json collectExtras(const Json& j, const std::vector<std::string>& keys) {
    Json extras = Json::object();
    for (auto& [k, v] : j.asObject())
        if (!known(keys, k)) extras[k] = v;
    return extras;
}

void appendExtras(Json& j, const Json& extras) {
    if (!extras.isObject()) return;
    for (auto& [k, v] : extras.asObject())
        if (!j.contains(k)) j[k] = v;
}

Json pluginToJson(const PluginInstance& p) {
    Json j = Json::object();
    j["id"] = p.id;
    j["name"] = p.name;
    j["vendor"] = p.vendor;
    j["format"] = toString(p.format);
    j["uid"] = p.uid;
    j["version"] = p.version;
    j["bypassed"] = p.bypassed;
    j["params"] = p.params;
    j["state"] = p.stateBase64;
    Json seen = Json::array();
    for (Platform pl : p.seenOn) seen.push(toString(pl));
    j["seenOn"] = seen;
    return j;
}

PluginInstance pluginFromJson(const Json& j) {
    PluginInstance p;
    p.id = j.getString("id", newId());
    p.name = j.getString("name", "");
    p.vendor = j.getString("vendor", "");
    auto fmt = pluginFormatFromString(j.getString("format", "builtin"));
    // Unknown future formats are kept as non-loadable rather than dropped.
    p.format = fmt.value_or(PluginFormat::AndroidAAP);
    p.uid = j.getString("uid", "");
    p.version = j.getString("version", "");
    p.bypassed = j.getBool("bypassed", false);
    if (auto* params = j.find("params"); params && params->isObject()) p.params = *params;
    p.stateBase64 = j.getString("state", "");
    if (auto* seen = j.find("seenOn"); seen && seen->isArray())
        for (auto& s : seen->asArray())
            if (s.isString())
                if (auto pl = platformFromString(s.asString())) p.seenOn.insert(*pl);
    return p;
}

Json mediaToJson(const MediaRef& m) {
    Json j = Json::object();
    j["id"] = m.id;
    j["sha256"] = m.sha256;
    j["file"] = m.fileName;
    j["sampleRate"] = m.sampleRate;
    j["channels"] = m.channels;
    j["frames"] = m.frames;
    j["origin"] = m.origin;
    return j;
}

MediaRef mediaFromJson(const Json& j) {
    MediaRef m;
    m.id = j.at("id").asString();
    m.sha256 = j.getString("sha256", "");
    m.fileName = j.getString("file", "");
    m.sampleRate = j.getNumber("sampleRate", 48000);
    m.channels = static_cast<int>(j.getInt("channels", 2));
    m.frames = j.getInt("frames", 0);
    m.origin = j.getString("origin", "import");
    // Reject path traversal: media must live inside the package.
    if (m.fileName.find("..") != std::string::npos || (!m.fileName.empty() && m.fileName[0] == '/'))
        throw std::runtime_error("project: invalid media path '" + m.fileName + "'");
    return m;
}

// Format migrations: each step upgrades a document by one version in place.
// v0 (pre-release prototypes) stored tempo as "bpm" and positions in seconds.
void migrate(Json& j) {
    int v = static_cast<int>(j.getInt("formatVersion", 0));
    if (v > kCurrentFormatVersion)
        throw std::runtime_error("project: saved by a newer Ynot (format v" + std::to_string(v) +
                                 "); please update the app on this device");
    if (v == 0) {
        if (auto* bpm = j.find("bpm")) { j["tempoBpm"] = *bpm; j.erase("bpm"); }
        j["formatVersion"] = 1;
    }
}

}  // namespace

Json trackToJson(const Track& t) {
    Json j = Json::object();
    j["id"] = t.id;
    j["name"] = t.name;
    j["kind"] = t.kind == TrackKind::Audio ? "audio" : "instrument";
    j["order"] = t.order;
    j["gainDb"] = t.gainDb;
    j["pan"] = t.pan;
    j["mute"] = t.mute;
    j["solo"] = t.solo;
    j["armed"] = t.armed;
    j["color"] = t.color;
    j["instrument"] = t.instrument ? pluginToJson(*t.instrument) : Json();
    Json inserts = Json::array();
    for (auto& p : t.inserts) inserts.push(pluginToJson(p));
    j["inserts"] = inserts;
    Json ac = Json::array();
    for (auto& c : t.audioClips) {
        Json cj = Json::object();
        cj["id"] = c.id;
        cj["mediaId"] = c.mediaId;
        cj["startTick"] = c.startTick;
        cj["lengthTick"] = c.lengthTick;
        cj["sourceOffsetFrames"] = c.sourceOffsetFrames;
        cj["gainDb"] = c.gainDb;
        if (!c.stretchSourceMediaId.empty()) {
            Json st = Json::object();
            st["sourceMediaId"] = c.stretchSourceMediaId;
            st["sourceOffsetFrames"] = c.stretchSourceOffsetFrames;
            st["ratio"] = c.stretchRatio;
            st["mode"] = c.stretchMode;
            cj["stretch"] = st;
        }
        ac.push(cj);
    }
    j["audioClips"] = ac;
    Json mc = Json::array();
    for (auto& c : t.midiClips) {
        Json cj = Json::object();
        cj["id"] = c.id;
        cj["startTick"] = c.startTick;
        cj["lengthTick"] = c.lengthTick;
        Json notes = Json::array();
        for (auto& n : c.notes) {
            // Compact [tick, length, pitch, velocity] tuples keep MIDI-heavy files small.
            notes.push(JsonArray{Json(n.tick), Json(n.lengthTick), Json(n.pitch), Json(n.velocity)});
        }
        cj["notes"] = notes;
        mc.push(cj);
    }
    j["midiClips"] = mc;
    Json autos = Json::array();
    for (auto& l : t.automation) {
        Json lj = Json::object();
        lj["target"] = l.target;
        Json pts = Json::array();
        for (auto& p : l.points) pts.push(JsonArray{Json(p.tick), Json(p.value)});
        lj["points"] = pts;
        autos.push(lj);
    }
    j["automation"] = autos;
    if (t.freeze) {
        Json f = Json::object();
        f["mediaId"] = t.freeze->mediaId;
        f["sourceFingerprint"] = t.freeze->sourceFingerprint;
        if (!t.freeze->tempoSignature.empty()) f["tempoSignature"] = t.freeze->tempoSignature;
        f["renderedOn"] = toString(t.freeze->renderedOn);
        f["renderedAt"] = t.freeze->renderedAt;
        j["freeze"] = f;
    } else {
        j["freeze"] = Json();
    }
    j["frozen"] = t.frozen;
    appendExtras(j, t.extras);
    return j;
}

Track trackFromJson(const Json& j) {
    Track t;
    t.id = j.at("id").asString();
    t.name = j.getString("name", "Track");
    t.kind = j.getString("kind", "audio") == "instrument" ? TrackKind::Instrument : TrackKind::Audio;
    t.order = j.getNumber("order", 0);
    t.gainDb = j.getNumber("gainDb", 0);
    t.pan = j.getNumber("pan", 0);
    t.mute = j.getBool("mute", false);
    t.solo = j.getBool("solo", false);
    t.armed = j.getBool("armed", false);
    t.color = j.getString("color", "#7C5CFF");
    if (auto* inst = j.find("instrument"); inst && inst->isObject()) t.instrument = pluginFromJson(*inst);
    if (auto* ins = j.find("inserts"); ins && ins->isArray())
        for (auto& p : ins->asArray()) t.inserts.push_back(pluginFromJson(p));
    if (auto* ac = j.find("audioClips"); ac && ac->isArray())
        for (auto& cj : ac->asArray()) {
            AudioClip c;
            c.id = cj.at("id").asString();
            c.mediaId = cj.at("mediaId").asString();
            c.startTick = cj.getInt("startTick", 0);
            c.lengthTick = cj.getInt("lengthTick", 0);
            c.sourceOffsetFrames = cj.getInt("sourceOffsetFrames", 0);
            c.gainDb = cj.getNumber("gainDb", 0);
            if (auto* st = cj.find("stretch"); st && st->isObject()) {
                c.stretchSourceMediaId = st->getString("sourceMediaId", "");
                c.stretchSourceOffsetFrames = st->getInt("sourceOffsetFrames", 0);
                c.stretchRatio = st->getNumber("ratio", 1.0);
                c.stretchMode = st->getString("mode", "poly");
            }
            t.audioClips.push_back(c);
        }
    if (auto* mc = j.find("midiClips"); mc && mc->isArray())
        for (auto& cj : mc->asArray()) {
            MidiClip c;
            c.id = cj.at("id").asString();
            c.startTick = cj.getInt("startTick", 0);
            c.lengthTick = cj.getInt("lengthTick", 0);
            if (auto* notes = cj.find("notes"); notes && notes->isArray())
                for (auto& n : notes->asArray()) {
                    auto& a = n.asArray();
                    if (a.size() < 4) continue;
                    c.notes.push_back({a[0].asInt(), a[1].asInt(), static_cast<int>(a[2].asInt()),
                                       static_cast<int>(a[3].asInt())});
                }
            t.midiClips.push_back(c);
        }
    if (auto* au = j.find("automation"); au && au->isArray())
        for (auto& lj : au->asArray()) {
            AutomationLane l;
            l.target = lj.getString("target", "");
            if (auto* pts = lj.find("points"); pts && pts->isArray())
                for (auto& pj : pts->asArray()) {
                    auto& a = pj.asArray();
                    if (a.size() >= 2) l.points.push_back({a[0].asInt(), a[1].asNumber()});
                }
            t.automation.push_back(l);
        }
    if (auto* f = j.find("freeze"); f && f->isObject()) {
        FreezeInfo fi;
        fi.mediaId = f->getString("mediaId", "");
        fi.sourceFingerprint = f->getString("sourceFingerprint", "");
        fi.tempoSignature = f->getString("tempoSignature", "");
        fi.renderedOn = platformFromString(f->getString("renderedOn", "")).value_or(Platform::Linux);
        fi.renderedAt = f->getString("renderedAt", "");
        t.freeze = fi;
    }
    t.frozen = j.getBool("frozen", false);
    t.extras = collectExtras(j, kTrackKeys);
    return t;
}

Json projectToJson(const Project& p) {
    Json j = Json::object();
    j["format"] = kFormatMagic;
    j["formatVersion"] = kCurrentFormatVersion;
    j["id"] = p.id;
    j["name"] = p.name;
    j["tempoBpm"] = p.tempoBpm;
    if (!p.tempoChanges.empty()) {
        Json tm = Json::array();
        for (const auto& c : p.tempoChanges) {
            Json cj = Json::object();
            cj["tick"] = c.tick;
            cj["bpm"] = c.bpm;
            tm.push(cj);
        }
        j["tempoChanges"] = tm;
    }
    j["timeSignature"] = JsonArray{Json(p.timeSigNumerator), Json(p.timeSigDenominator)};
    j["sampleRateHint"] = p.sampleRateHint;
    Json tracks = Json::array();
    for (auto* t : p.orderedTracks()) tracks.push(trackToJson(*t));
    j["tracks"] = tracks;
    Json media = Json::array();
    for (auto& m : p.media) media.push(mediaToJson(m));
    j["media"] = media;
    appendExtras(j, p.extras);
    return j;
}

Project projectFromJson(const Json& input) {
    Json j = input;
    if (!j.isObject()) throw std::runtime_error("project: root must be an object");
    if (j.getString("format", kFormatMagic) != std::string(kFormatMagic))
        throw std::runtime_error("project: not a Ynot project");
    migrate(j);
    Project p;
    p.formatVersion = kCurrentFormatVersion;
    p.id = j.getString("id", newId());
    p.name = j.getString("name", "Untitled");
    p.tempoBpm = j.getNumber("tempoBpm", 120);
    if (p.tempoBpm < 10 || p.tempoBpm > 999) p.tempoBpm = 120;
    if (auto* tm = j.find("tempoChanges"); tm && tm->isArray()) {
        for (auto& cj : tm->asArray()) {
            TempoChange c;
            c.tick = cj.getInt("tick", 0);
            c.bpm = std::clamp(cj.getNumber("bpm", p.tempoBpm), kMinBpm, kMaxBpm);
            if (c.tick > 0) p.tempoChanges.push_back(c);
        }
        std::stable_sort(p.tempoChanges.begin(), p.tempoChanges.end(),
                         [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    }
    if (auto* ts = j.find("timeSignature"); ts && ts->isArray() && ts->size() == 2) {
        p.timeSigNumerator = static_cast<int>(ts->asArray()[0].asInt());
        p.timeSigDenominator = static_cast<int>(ts->asArray()[1].asInt());
    }
    p.sampleRateHint = j.getNumber("sampleRateHint", 48000);
    if (auto* tr = j.find("tracks"); tr && tr->isArray())
        for (auto& tj : tr->asArray()) p.tracks.push_back(trackFromJson(tj));
    if (auto* md = j.find("media"); md && md->isArray())
        for (auto& mj : md->asArray()) p.media.push_back(mediaFromJson(mj));
    p.extras = collectExtras(j, kProjectKeys);
    return p;
}

std::string trackFingerprint(const Track& t) {
    Json j = trackToJson(t);
    for (const char* k : {"name", "order", "gainDb", "pan", "mute", "solo", "armed", "color",
                          "freeze", "frozen"})
        j.erase(k);
    // Where a plugin was last loaded is bookkeeping, not sound.
    if (auto* inst = j.find("instrument"); inst && inst->isObject()) j["instrument"].erase("seenOn");
    if (j.find("inserts")) {
        for (auto& p : j["inserts"].asArray()) p.erase("seenOn");
    }
    // Mix automation is applied after the freeze point, so it is not part of the sound.
    if (auto* autos = j.find("automation"); autos && autos->isArray()) {
        Json kept = Json::array();
        for (auto& l : autos->asArray()) {
            auto target = l.getString("target", "");
            if (target.rfind("track.", 0) != 0) kept.push(l);
        }
        j["automation"] = kept;
    }
    return sha256Hex(j.dump());
}

void saveProjectPackage(const Project& p, const std::string& dir) {
    fs::create_directories(fs::path(dir) / "media");
    std::string text = projectToJson(p).dump(2) + "\n";
    // Write-then-rename so a crash or sync conflict never leaves a half-written project.
    auto tmp = fs::path(dir) / "project.json.tmp";
    writeFileBytes(tmp.string(), std::vector<uint8_t>(text.begin(), text.end()));
    fs::rename(tmp, fs::path(dir) / "project.json");
}

Project loadProjectPackage(const std::string& dir) {
    auto bytes = readFileBytes((fs::path(dir) / "project.json").string());
    return projectFromJson(Json::parse(std::string(bytes.begin(), bytes.end())));
}

const MediaRef& addMedia(Project& p, const std::string& packageDir, const AudioBuffer& audio,
                         const std::string& origin) {
    auto bytes = encodeWavFloat32(audio);
    std::string hash = sha256Hex(bytes.data(), bytes.size());
    for (auto& m : p.media)
        if (m.sha256 == hash) return m;  // identical audio already stored
    MediaRef m;
    m.id = newId();
    m.sha256 = hash;
    m.fileName = "media/" + hash + ".wav";
    m.sampleRate = audio.sampleRate;
    m.channels = audio.channels;
    m.frames = audio.frames();
    m.origin = origin;
    fs::create_directories(fs::path(packageDir) / "media");
    auto path = fs::path(packageDir) / m.fileName;
    if (!fs::exists(path)) writeFileBytes(path.string(), bytes);
    p.media.push_back(m);
    return p.media.back();
}

AudioBuffer loadMedia(const MediaRef& m, const std::string& packageDir) {
    auto bytes = readFileBytes((fs::path(packageDir) / m.fileName).string());
    if (!m.sha256.empty() && sha256Hex(bytes.data(), bytes.size()) != m.sha256)
        throw std::runtime_error("media " + m.fileName + " is corrupt or incomplete (hash mismatch)");
    return decodeWav(bytes);
}

}  // namespace ynot
