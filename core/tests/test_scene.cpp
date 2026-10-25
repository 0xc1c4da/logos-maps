// Scene model: ids, sessions, placements (M6.1, M6.3).
#include <logos_test.h>

#include "engine/scene.h"

using nlohmann::json;
using namespace maps::engine;

LOGOS_TEST(session_names_are_module_slash_name) {
    LOGOS_ASSERT_TRUE(isValidSessionName("routing/main"));
    LOGOS_ASSERT_TRUE(isValidSessionName("maps/builtin"));
    LOGOS_ASSERT_TRUE(isValidSessionName("my_mod.v2/trip-01"));
    LOGOS_ASSERT_FALSE(isValidSessionName("routing"));
    LOGOS_ASSERT_FALSE(isValidSessionName("/main"));
    LOGOS_ASSERT_FALSE(isValidSessionName("routing/"));
    LOGOS_ASSERT_FALSE(isValidSessionName("a/b/c"));
    LOGOS_ASSERT_FALSE(isValidSessionName("rout ing/main"));
    LOGOS_ASSERT_FALSE(isValidSessionName("routing:x/main"));
}

LOGOS_TEST(local_ids_have_no_colon_or_whitespace) {
    LOGOS_ASSERT_TRUE(isValidLocalId("route"));
    LOGOS_ASSERT_TRUE(isValidLocalId("route/alt-1.v2"));
    LOGOS_ASSERT_FALSE(isValidLocalId(""));
    LOGOS_ASSERT_FALSE(isValidLocalId("a:b"));
    LOGOS_ASSERT_FALSE(isValidLocalId("a b"));
}

LOGOS_TEST(global_ids_prefix_session) {
    LOGOS_ASSERT_EQ(globalId("routing/main", "route"), std::string("routing/main:route"));
    LOGOS_ASSERT_EQ(sessionOf("routing/main:route"), std::string("routing/main"));
    LOGOS_ASSERT_EQ(sessionOf("noprefix"), std::string(""));
}

LOGOS_TEST(placement_parses_exactly_one_kind) {
    const std::string s = "routing/main";
    LOGOS_ASSERT_TRUE(Placement::fromJson(json(), s) == Placement::top());
    LOGOS_ASSERT_TRUE(Placement::fromJson(json{{"top", true}}, s) == Placement::top());
    Placement a = Placement::fromJson(json{{"anchor", "labels"}}, s);
    LOGOS_ASSERT_TRUE(a.kind == Placement::Kind::Anchor);
    LOGOS_ASSERT_EQ(a.ref, std::string("labels"));
    // before/above take a local id and qualify it with the session...
    Placement b = Placement::fromJson(json{{"before", "casing"}}, s);
    LOGOS_ASSERT_TRUE(b.kind == Placement::Kind::Before);
    LOGOS_ASSERT_EQ(b.ref, std::string("routing/main:casing"));
    // ...or a global id of another session verbatim.
    Placement c = Placement::fromJson(json{{"above", "other/sess:base"}}, s);
    LOGOS_ASSERT_EQ(c.ref, std::string("other/sess:base"));

    LOGOS_ASSERT_THROWS(Placement::fromJson(json{{"anchor", "labels"}, {"top", true}}, s));
    LOGOS_ASSERT_THROWS(Placement::fromJson(json{{"anchor", ""}}, s));
    LOGOS_ASSERT_THROWS(Placement::fromJson(json{{"bogus", 1}}, s));
    LOGOS_ASSERT_THROWS(Placement::fromJson(json::array(), s));
}

LOGOS_TEST(placement_round_trips_through_json) {
    for (const json j : {json{{"top", true}}, json{{"anchor", "routes"}}, json{{"before", "x/y:z"}}, json{{"above", "x/y:z"}}}) {
        Placement p = Placement::fromJson(j, "x/y");
        LOGOS_ASSERT_EQ(p.toJson(), j);
    }
}

LOGOS_TEST(scene_find_helpers_follow_session_prefix) {
    Scene scene;
    Session& s = scene.sessions["a/b"];
    s.name = "a/b";
    Source src; src.id = "a/b:s"; src.type = "geojson";
    s.sources[src.id] = src;
    Layer l; l.id = "a/b:l"; l.type = "circle"; l.source = "a/b:s";
    s.layers.push_back(l);
    Image img; img.id = "a/b:i";
    s.images[img.id] = img;

    LOGOS_ASSERT_TRUE(scene.findSource("a/b:s") != nullptr);
    LOGOS_ASSERT_TRUE(scene.findLayer("a/b:l") != nullptr);
    LOGOS_ASSERT_TRUE(scene.findImage("a/b:i") != nullptr);
    LOGOS_ASSERT_TRUE(scene.findSource("a/c:s") == nullptr);
    LOGOS_ASSERT_TRUE(scene.findLayer("nope") == nullptr);
    const json j = scene.toJson();
    LOGOS_ASSERT_EQ(j["v"], json(1));
    LOGOS_ASSERT_EQ(j["sessions"]["a/b"]["layers"].size(), std::size_t(1));
}
