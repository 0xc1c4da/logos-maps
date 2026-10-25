// Basemap descriptors, catalogue and raster expansion (§9.1, M9.2, M9.3, App. C).
#include <logos_test.h>

#include "engine/basemap.h"
#include "test_util.h"

using nlohmann::json;
using namespace maps::engine;

LOGOS_TEST(descriptor_fixture_validates_and_normalises) {
    const json d = mt::readJsonFixture("basemaps/openfreemap-bright.json");
    LOGOS_ASSERT_TRUE(validateDescriptor(d).empty());
    const json n = normalizeDescriptor(d);
    LOGOS_ASSERT_EQ(n["v"], json(1));
    LOGOS_ASSERT_FALSE(n["fingerprint"].get<std::string>().empty());
    // Fingerprint is a function of the descriptor content.
    json d2 = d; d2["style_url"] = "https://elsewhere/style.json";
    LOGOS_ASSERT_NE(normalizeDescriptor(d2)["fingerprint"], n["fingerprint"]);
    LOGOS_ASSERT_EQ(normalizeDescriptor(d)["fingerprint"], n["fingerprint"]);
    // An explicit fingerprint is kept.
    json d3 = d; d3["fingerprint"] = "pinned";
    LOGOS_ASSERT_EQ(normalizeDescriptor(d3)["fingerprint"], json("pinned"));
}

LOGOS_TEST(descriptor_validation_reports_schema_violations) {
    json d = mt::readJsonFixture("basemaps/openfreemap-bright.json");
    d["id"] = "Bad Id";
    d["format"] = "wms";
    d["light"] = "dusk";
    d["vehicle"] = json::array({"car", "boat"});
    d["anchors"] = json{{"labels", "x"}};
    d["zoom"] = json::array({5, 2});
    d["patch"] = json::array({json{{"op", "nuke"}}});
    d["bogus"] = 1;
    d.erase("attribution");
    const auto errs = validateDescriptor(d);
    std::vector<std::string> paths;
    for (const auto& e : errs) paths.push_back(e.path);
    const std::string joined = mt::join(paths);
    for (const char* p : {"/id", "/format", "/light", "/vehicle", "/anchors/routes", "/zoom", "/patch/0/op", "/bogus", "/attribution"})
        LOGOS_ASSERT_CONTAINS(joined, std::string(p));
    LOGOS_ASSERT_FALSE(validateDescriptor(json("x")).empty());
    LOGOS_ASSERT_FALSE(validateDescriptor(json{{"id", "a/b"}, {"name", "n"}, {"format", "mapbox-gl"}, {"attribution", json::object()}, {"anchors", {{"labels", "l"}, {"routes", "r"}}}}).empty()); // no style_url
}

LOGOS_TEST(raster_descriptor_expands_to_a_style_with_top_anchors) {
    const json d = mt::readJsonFixture("basemaps/osm-raster.json");
    LOGOS_ASSERT_TRUE(validateDescriptor(d).empty());
    Catalogue cat;
    std::string err;
    LOGOS_ASSERT_TRUE(cat.add(d, &err));
    const json* stored = cat.find("osm/standard");
    LOGOS_ASSERT_TRUE(stored != nullptr);
    LOGOS_ASSERT_EQ((*stored)["anchors"], json({{"labels", ""}, {"routes", ""}}));
    LOGOS_ASSERT_EQ((*stored)["tile_size"], json(256));
    const json style = rasterStyle(*stored);
    LOGOS_ASSERT_EQ(style["version"], json(8));
    LOGOS_ASSERT_EQ(style["sources"]["basemap"]["type"], json("raster"));
    LOGOS_ASSERT_EQ(style["sources"]["basemap"]["tiles"][0], json("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
    LOGOS_ASSERT_EQ(style["sources"]["basemap"]["tileSize"], json(256));
    LOGOS_ASSERT_EQ(style["sources"]["basemap"]["maxzoom"], json(19));
    LOGOS_ASSERT_EQ(style["layers"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(style["layers"][0]["type"], json("raster"));
    LOGOS_ASSERT_CONTAINS(style["sources"]["basemap"]["attribution"].get<std::string>(), std::string("openstreetmap.org/copyright"));
}

LOGOS_TEST(catalogue_add_remove_list_and_filter) {
    Catalogue cat;
    std::string err;
    LOGOS_ASSERT_TRUE(cat.add(mt::readJsonFixture("basemaps/openfreemap-bright.json"), &err));
    LOGOS_ASSERT_TRUE(cat.add(mt::readJsonFixture("basemaps/osm-raster.json"), &err));
    json night = mt::readJsonFixture("basemaps/openfreemap-bright.json");
    night["id"] = "openfreemap/dark";
    night["light"] = "night";
    night["profiles"] = json::array({"online", "offline"});
    LOGOS_ASSERT_TRUE(cat.add(night, &err));
    LOGOS_ASSERT_EQ(cat.size(), std::size_t(3));

    LOGOS_ASSERT_EQ(cat.list()["basemaps"].size(), std::size_t(3));
    LOGOS_ASSERT_EQ(cat.list(json{{"light", "night"}})["basemaps"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(cat.list(json{{"format", "raster"}})["basemaps"][0]["id"], json("osm/standard"));
    LOGOS_ASSERT_EQ(cat.list(json{{"profiles", "offline"}})["basemaps"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(cat.list(json{{"vehicle", json::array({"car", "foot"})}})["basemaps"].size(), std::size_t(3));
    LOGOS_ASSERT_EQ(cat.list(json{{"v", 1}, {"provider", "nobody"}})["basemaps"].size(), std::size_t(0));

    json bad = night; bad["format"] = "wms";
    LOGOS_ASSERT_FALSE(cat.add(bad, &err));
    LOGOS_ASSERT_CONTAINS(err, std::string("/format"));
    LOGOS_ASSERT_TRUE(cat.remove("osm/standard"));
    LOGOS_ASSERT_FALSE(cat.remove("osm/standard"));
    LOGOS_ASSERT_TRUE(cat.find("osm/standard") == nullptr);
}
