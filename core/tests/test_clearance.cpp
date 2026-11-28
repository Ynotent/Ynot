#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
AudioBuffer noise(double seconds, uint32_t seed) {
    AudioBuffer a;
    a.sampleRate = 48000;
    a.channels = 1;
    for (int i = 0; i < static_cast<int>(seconds * a.sampleRate); ++i) {
        seed = seed * 1664525u + 1013904223u;
        a.samples.push_back(0.2f * (static_cast<float>(seed >> 8) / 16777216.f - 0.5f));
    }
    return a;
}
}  // namespace

TEST("clearance: imports need clearing; recordings and Ynot sounds don't; status and owners are saved") {
    auto dir = th::tempDir("clearance");
    Project p;
    p.id = "p";
    p.name = "My Song";
    p.tempoBpm = 120;
    Track a;
    a.id = "a";
    a.name = "Samples";
    a.kind = TrackKind::Audio;
    p.tracks = {a};
    const auto clip = importAudioClip(p, dir, "a", noise(2.0, 1), kTicksPerQuarter * 4);  // at 2 s
    const auto mediaId = p.tracks[0].audioClips[0].mediaId;
    noteImportedFile(p, mediaId, "soul-loop.wav");
    auto report = clearanceReport(p);
    CHECK_EQ(report.size(), size_t(1));
    CHECK_EQ(report[0].name, std::string("soul-loop.wav"));
    CHECK(report[0].status == ClearanceStatus::Unknown);
    CHECK_NEAR(report[0].secondsUsed, 2.0, 1e-6);
    CHECK_EQ(report[0].firstTick, kTicksPerQuarter * 4);
    CHECK_EQ(unclearedCount(p), 1);

    // The same sample sent to a sampler is the same source (its credit follows it).
    PackageMediaProvider media(dir);
    const auto chops = addSamplerTrack(p, dir, clipAudio(p, media, "a", clip), "Chops");
    inheritCredit(p, samplerOf(p, chops)->params.getString("mediaId", ""), mediaId);
    // The whole clip is the same audio, so it's the same source, now used on two tracks.
    CHECK_EQ(unclearedCount(p), 1);
    CHECK_EQ(clearanceReport(p)[0].trackNames.size(), size_t(2));

    // Mark it: owners filled in, cleared.
    SampleCredit c = *findCredit(p, mediaId);
    c.title = "Old Soul Song";
    c.artist = "The Originals";
    c.masterOwner = "Big Label";
    c.publisher = "Song Publishing Co";
    c.sourceTime = "0:12-0:16";
    c.status = ClearanceStatus::Cleared;
    setCredit(p, c);
    CHECK_EQ(clearanceReport(p)[0].name, std::string("Old Soul Song - The Originals"));
    CHECK_EQ(unclearedCount(p), 0);
    // A part of it sent to the sampler is a new file that inherits the credit.
    CHECK(trimClipStart(p, "a", clip, kTicksPerQuarter * 5));
    const auto part = addSamplerTrack(p, dir, clipAudio(p, media, "a", clip), "Part");
    const auto partMedia = samplerOf(p, part)->params.getString("mediaId", "");
    CHECK(partMedia != mediaId);
    inheritCredit(p, partMedia, mediaId);
    CHECK(findCredit(p, partMedia)->status == ClearanceStatus::Cleared);
    CHECK_EQ(findCredit(p, partMedia)->title, std::string("Old Soul Song"));

    // Stretched clips are traced back to the original.
    CHECK(stretchClip(p, dir, media, "a", clip, 1.5, StretchMode::Poly));
    CHECK_EQ(clearanceReport(p).size(), size_t(2));
    CHECK(clearanceReport(p)[0].status == ClearanceStatus::Cleared);
    CHECK_EQ(clearanceReport(p)[0].mediaId, mediaId);

    // Own recordings are original.
    MediaRef rec;
    rec.id = "rec";
    rec.origin = "recording";
    CHECK(defaultClearance(rec) == ClearanceStatus::Original);

    // Saved and loaded.
    saveProjectPackage(p, dir);
    Project q = loadProjectPackage(dir);
    CHECK(projectToJson(q) == projectToJson(p));
    CHECK_EQ(findCredit(q, mediaId)->publisher, std::string("Song Publishing Co"));

    // The sheet lists both, with owners and status.
    const auto csv = clearanceSheetCsv(q);
    CHECK(csv.find("Old Soul Song") != std::string::npos);
    CHECK(csv.find("Big Label") != std::string::npos);
    CHECK(csv.find("Cleared") != std::string::npos);
    CHECK(csv.find("Not cleared yet") == std::string::npos);
    CHECK(csv.find("0:02") != std::string::npos);  // first heard at 2 s
}
