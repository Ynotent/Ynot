#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

TEST("packs: every sound renders; loops are exactly 4 bars at their tempo") {
    CHECK(packSounds().size() >= 25);
    for (const auto& s : packSounds()) {
        auto a = renderPackSound(s.id, 44100);
        CHECK(a.frames() > 0);
        CHECK(th::rms(a) > 1e-3);
        if (s.kind == "loop") CHECK_EQ(a.frames(), int64_t(std::llround(s.bars * 4 * 60.0 / s.bpm * 44100)));
    }
    CHECK(findPackSound("nope") == nullptr);
}

TEST("packs: pack sounds are credited royalty-free; sampler and clip use") {
    auto dir = th::tempDir("packs");
    Project p;
    p.id = "p";
    p.tempoBpm = 84;
    const auto sampler = addPackSoundToSampler(p, dir, "breaks/lofi-84", 48000);
    CHECK(!sampler.empty());
    CHECK_EQ(samplerSlices(*samplerOf(p, sampler)).size(), size_t(16));
    const auto keys = addPackSoundToSampler(p, dir, "drums/808", 48000);
    CHECK_EQ(samplerOf(p, keys)->params.getString("mode", ""), std::string("keys"));
    const auto clip = addPackSoundAsClip(p, dir, "loops/late-night-keys", "", 0, 48000);
    CHECK(!clip.empty());
    // At its own tempo the 4-bar loop is exactly 4 bars long.
    CHECK_EQ(p.tracks.back().audioClips[0].lengthTick, 16 * kTicksPerQuarter);
    CHECK_EQ(unclearedCount(p), 0);
    for (const auto& u : clearanceReport(p)) CHECK(u.status == ClearanceStatus::RoyaltyFree);
    PackageMediaProvider media(dir);
    BuiltinPluginHost host;
    CHECK(th::rms(renderProject(p, media, host, 48000, 0.2)) > 0.001);
}
