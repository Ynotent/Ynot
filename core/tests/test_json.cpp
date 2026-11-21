#include "test.hpp"
#include "ynot/json.hpp"

using namespace ynot;

TEST("json: round-trips nested values and preserves key order") {
    Json j = Json::object();
    j["z"] = 1;
    j["a"] = JsonArray{Json(1.5), Json("x"), Json(true), Json()};
    j["m"]["inner"] = "v";
    std::string s = j.dump();
    CHECK_EQ(s, std::string(R"({"z":1,"a":[1.5,"x",true,null],"m":{"inner":"v"}})"));
    CHECK(Json::parse(s) == j);
    CHECK(Json::parse(j.dump(2)) == j);
}

TEST("json: unicode escapes and control characters") {
    Json j = Json::parse(R"("café 🎵 \n\t")");
    CHECK_EQ(j.asString(), std::string("caf\xC3\xA9 \xF0\x9F\x8E\xB5 \n\t"));
    Json back = Json::parse(Json(std::string("a\x01" "b")).dump());
    CHECK_EQ(back.asString(), std::string("a\x01" "b"));
}

TEST("json: copies have value semantics") {
    Json a = Json::object();
    a["k"] = 1;
    Json b = a;
    b["k"] = 2;
    CHECK_EQ(a.at("k").asInt(), 1);
    CHECK_EQ(b.at("k").asInt(), 2);
}

TEST("json: numbers round-trip exactly") {
    for (double d : {0.1, -3.0, 1e-9, 123456789.125, 6.02214076e23}) {
        CHECK(Json::parse(Json(d).dump()).asNumber() == d);
    }
}

TEST("json: rejects malformed input") {
    CHECK_THROWS(Json::parse("{"));
    CHECK_THROWS(Json::parse("[1,]"));
    CHECK_THROWS(Json::parse("{\"a\":1} x"));
    CHECK_THROWS(Json::parse(std::string(1000, '[')));
}
