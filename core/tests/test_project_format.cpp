#include <filesystem>

#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

TEST("format: package save/load is lossless") {
    auto dir = th::tempDir("roundtrip");
    Project p = makeDemoProject(dir);
    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK(projectToJson(p) == projectToJson(q));
    CHECK_EQ(q.tracks.size(), size_t(3));
    auto* keys = q.findTrack(p.tracks[1].id);
    CHECK(keys != nullptr);
    CHECK_EQ(keys->midiClips[0].notes.size(), size_t(6));
    CHECK_EQ(keys->inserts[0].stateBase64, std::string("AAECAwQ="));
    CHECK(keys->inserts[0].seenOn.count(Platform::Windows) == 1);
    CHECK_EQ(q.tracks[0].automation[0].points.size(), size_t(2));
}

TEST("format: media is content-addressed and verified") {
    auto dir = th::tempDir("media");
    Project p = makeDemoProject(dir);
    const MediaRef& m = p.media[0];
    CHECK_EQ(m.fileName, "media/" + m.sha256 + ".wav");
    AudioBuffer b = loadMedia(m, dir);
    CHECK_EQ(b.frames(), int64_t(88200));
    CHECK_EQ(b.sampleRate, 44100.0);
    // Adding identical audio does not duplicate it.
    size_t before = p.media.size();
    addMedia(p, dir, b, "import");
    CHECK_EQ(p.media.size(), before);
    // Corruption (e.g. a half-synced download) is detected.
    auto path = std::filesystem::path(dir) / m.fileName;
    auto bytes = readFileBytes(path.string());
    bytes[100] ^= 0xFF;
    writeFileBytes(path.string(), bytes);
    CHECK_THROWS(loadMedia(m, dir));
}

TEST("format: unknown fields from newer apps are preserved") {
    auto dir = th::tempDir("extras");
    Project p = makeDemoProject(dir);
    Json j = projectToJson(p);
    j["futureProjectFeature"] = "keep me";
    j["tracks"].asArray()[0]["spatialAudio"] = Json::parse(R"({"azimuth":30})");
    Project q = projectFromJson(j);
    Json again = projectToJson(q);
    CHECK_EQ(again.at("futureProjectFeature").asString(), std::string("keep me"));
    CHECK_EQ(again.at("tracks").asArray()[0].at("spatialAudio").getInt("azimuth", 0), int64_t(30));
}

TEST("format: migrates v0 and rejects newer major versions") {
    Json v0 = Json::parse(R"({"format":"ynot.project","id":"p","name":"Old","bpm":95,"tracks":[]})");
    Project p = projectFromJson(v0);
    CHECK_EQ(p.tempoBpm, 95.0);
    Json future = Json::parse(R"({"format":"ynot.project","formatVersion":99,"tracks":[]})");
    CHECK_THROWS(projectFromJson(future));
}

TEST("format: rejects media paths that escape the package") {
    Json j = Json::parse(
        R"({"format":"ynot.project","formatVersion":1,"media":[{"id":"m","file":"../../etc/passwd"}]})");
    CHECK_THROWS(projectFromJson(j));
}

TEST("format: track fingerprint tracks sound, not mix") {
    auto dir = th::tempDir("fp");
    Project p = makeDemoProject(dir);
    Track t = p.tracks[1];
    std::string fp = trackFingerprint(t);
    t.gainDb = -12;
    t.pan = 0.5;
    t.name = "Renamed";
    t.inserts[0].seenOn.insert(Platform::IOS);
    CHECK_EQ(trackFingerprint(t), fp);
    t.midiClips[0].notes[0].pitch += 1;
    CHECK(trackFingerprint(t) != fp);
}
