#include <algorithm>
#include <random>

#include "helpers.hpp"
#include "test.hpp"

using namespace ynot;

namespace {
int64_t gNow = 1000;
int64_t fakeClock() { return gNow; }
int64_t skewedClock() { return gNow - 500; }  // a device whose clock is behind

Json baseDoc() {
    Project p;
    p.id = "proj";
    p.name = "Collab";
    Track t;
    t.id = "t1";
    t.name = "Drums";
    p.tracks.push_back(t);
    return projectToJson(p);
}
}  // namespace

TEST("collab: replicas converge regardless of delivery order") {
    Json base = baseDoc();
    Replica mac("mac", base, fakeClock), win("win", base, fakeClock), ios("ios", base, fakeClock),
        android("android", base, skewedClock);
    std::vector<Operation> all;
    gNow = 2000;
    all.push_back(mac.set(trackPath("t1"), "gainDb", -3.0));
    all.push_back(win.set(trackPath("t1"), "gainDb", -9.0));  // concurrent with mac
    Track vox;
    vox.id = "t2";
    vox.name = "Vox";
    all.push_back(ios.insert({}, "tracks", trackToJson(vox)));
    gNow = 2001;
    all.push_back(android.set({}, "tempoBpm", 98.0));
    all.push_back(ios.set(trackPath("t2"), "pan", -0.25));
    all.push_back(mac.set(trackPath("t1"), "name", "Drum Bus"));

    std::mt19937 rng(7);
    std::vector<Replica*> replicas{&mac, &win, &ios, &android};
    for (auto* r : replicas) {
        auto shuffled = all;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        for (auto& op : shuffled) r->receive(op);
        r->receive(shuffled.front());  // duplicate delivery is harmless
    }
    for (auto* r : replicas) CHECK(r->document() == mac.document());

    Project p = mac.project();
    CHECK_EQ(p.findTrack("t1")->gainDb, -9.0);  // "win" sorts after "mac" at equal time
    CHECK_EQ(p.findTrack("t1")->name, std::string("Drum Bus"));
    CHECK_EQ(p.findTrack("t2")->pan, -0.25);
    CHECK_EQ(p.tempoBpm, 98.0);
}

TEST("collab: a device with a slow clock still orders after what it has seen") {
    Json base = baseDoc();
    gNow = 5000;
    Replica mac("mac", base, fakeClock), android("android", base, skewedClock);
    auto a = mac.set(trackPath("t1"), "gainDb", -1.0);
    android.receive(a);
    auto b = android.set(trackPath("t1"), "gainDb", -2.0);  // causally after `a`
    CHECK(a.ts < b.ts);
    mac.receive(b);
    CHECK_EQ(mac.project().findTrack("t1")->gainDb, -2.0);
}

TEST("collab: remove wins over concurrent edits") {
    Json base = baseDoc();
    gNow = 3000;
    Replica a("a", base, fakeClock), b("b", base, fakeClock);
    auto rm = a.remove({}, "tracks", "t1");
    gNow = 3001;
    auto edit = b.set(trackPath("t1"), "mute", true);  // b hasn't seen the remove
    a.receive(edit);
    b.receive(rm);
    CHECK(a.document() == b.document());
    CHECK(a.project().findTrack("t1") == nullptr);
}

TEST("collab: operations serialize for the wire and compaction keeps state") {
    Json base = baseDoc();
    gNow = 4000;
    Replica a("a", base, fakeClock), b("b", base, fakeClock);
    auto op = a.insert(trackPath("t1"), "midiClips", Json::parse(R"({"id":"c1","startTick":0,"lengthTick":960,"notes":[[0,480,60,100]]})"));
    Operation wire = Operation::fromJson(Json::parse(op.toJson().dump()));
    b.receive(wire);
    CHECK(a.document() == b.document());
    CHECK_EQ(b.project().findTrack("t1")->midiClips[0].notes[0].pitch, 60);

    Json before = b.document();
    b.compact();
    CHECK_EQ(b.opCount(), size_t(0));
    CHECK(b.document() == before);
    b.receive(wire);  // late re-delivery of an already-compacted op is ignored
    CHECK(b.document() == before);
}

TEST("collab: edits cannot change ids or the format header") {
    Json base = baseDoc();
    Replica a("a", base, fakeClock);
    a.set(trackPath("t1"), "id", "hijack");
    a.set({}, "formatVersion", 999);
    CHECK(a.project().findTrack("t1") != nullptr);
    CHECK_EQ(a.document().getInt("formatVersion", 0), int64_t(1));
}
