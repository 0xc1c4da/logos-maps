// View and provider registries (§12.1 views, §12.5 tileProvider).
#include <logos_test.h>

#include "engine/registry.h"
#include "test_util.h"

using nlohmann::json;
using namespace maps::engine;

LOGOS_TEST(views_open_close_and_track_revisions) {
    Views v;
    LOGOS_ASSERT_TRUE(v.open("basecamp/main", "tok1", json{{"render", {{"terrain", true}}}}));
    LOGOS_ASSERT_FALSE(v.open("basecamp/main", "tok2", json::object()));
    LOGOS_ASSERT_FALSE(v.open("", "tok3", json::object()));
    LOGOS_ASSERT_TRUE(v.open("basecamp/side", "tok3", json::object()));
    LOGOS_ASSERT_EQ(v.size(), std::size_t(2));
    LOGOS_ASSERT_EQ(mt::join(v.ids()), std::string("basecamp/main,basecamp/side"));

    LOGOS_ASSERT_TRUE(v.ack("basecamp/main", 3));
    LOGOS_ASSERT_FALSE(v.ack("basecamp/main", 3));
    LOGOS_ASSERT_FALSE(v.ack("basecamp/main", 2));
    LOGOS_ASSERT_FALSE(v.ack("nope", 9));
    LOGOS_ASSERT_EQ(v.minApplied(), std::uint64_t(0));
    LOGOS_ASSERT_TRUE(v.ack("basecamp/side", 5));
    LOGOS_ASSERT_EQ(v.minApplied(), std::uint64_t(3));
    LOGOS_ASSERT_TRUE(v.rendered("basecamp/main", 3));
    LOGOS_ASSERT_EQ(v.find("basecamp/main")->renderedRevision, std::uint64_t(3));

    const json l = v.list();
    LOGOS_ASSERT_EQ(l["views"].size(), std::size_t(2));
    LOGOS_ASSERT_EQ(l["views"][0]["surfaceToken"], json("tok1"));
    LOGOS_ASSERT_EQ(l["views"][0]["appliedRevision"], json(3));
    LOGOS_ASSERT_EQ(v.capabilities().size(), std::size_t(2));

    LOGOS_ASSERT_TRUE(v.close("basecamp/main"));
    LOGOS_ASSERT_FALSE(v.close("basecamp/main"));
    LOGOS_ASSERT_EQ(v.minApplied(), std::uint64_t(5));
}

LOGOS_TEST(providers_register_resolve_longest_prefix_and_drop_with_session) {
    Providers p;
    LOGOS_ASSERT_EQ(p.registerProvider("tiles/main", "logos", json{{"module", "tile_store"}, {"prefix", ""}}), std::string(""));
    LOGOS_ASSERT_EQ(p.registerProvider("tiles/main", "logos", json{{"module", "tile_store"}, {"prefix", "/glyphs/"}}), std::string(""));
    LOGOS_ASSERT_EQ(p.registerProvider("other/s", "logos", json{{"module", "cdn_cache"}, {"prefix", "planet"}}), std::string(""));
    LOGOS_ASSERT_FALSE(p.registerProvider("tiles/main", "logos", json{{"module", "tile_store"}, {"prefix", "glyphs/"}}).empty());  // duplicate
    LOGOS_ASSERT_FALSE(p.registerProvider("tiles/main", "http", json{{"module", "x"}}).empty());
    LOGOS_ASSERT_FALSE(p.registerProvider("tiles/main", "logos", json{{"prefix", "x"}}).empty());
    LOGOS_ASSERT_FALSE(p.registerProvider("tiles/main", "logos", json{{"module", "a/b"}}).empty());
    LOGOS_ASSERT_FALSE(p.registerProvider("", "logos", json{{"module", "a"}}).empty());
    LOGOS_ASSERT_EQ(p.size(), std::size_t(3));

    const ProviderRecord* r = p.resolve("logos", "tile_store", "glyphs/Noto/0-255.pbf");
    LOGOS_ASSERT_TRUE(r != nullptr);
    LOGOS_ASSERT_EQ(r->prefix, std::string("glyphs/"));
    r = p.resolve("logos", "tile_store", "tiles/1/2/3.pbf");
    LOGOS_ASSERT_TRUE(r != nullptr);
    LOGOS_ASSERT_EQ(r->prefix, std::string(""));
    LOGOS_ASSERT_TRUE(p.resolve("logos", "cdn_cache", "other/x") == nullptr);
    LOGOS_ASSERT_TRUE(p.resolve("logos", "unknown", "x") == nullptr);
    LOGOS_ASSERT_EQ(mt::join(p.schemes()), std::string("logos"));
    LOGOS_ASSERT_EQ(p.list()["providers"].size(), std::size_t(3));

    LOGOS_ASSERT_EQ(p.dropSession("tiles/main"), std::size_t(2));
    LOGOS_ASSERT_TRUE(p.unregisterProvider("other/s", "logos"));
    LOGOS_ASSERT_FALSE(p.unregisterProvider("other/s", "logos"));
    LOGOS_ASSERT_EQ(p.size(), std::size_t(0));
}
