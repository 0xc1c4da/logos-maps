// Declarative style patching (M9.6, M9.7).
#include <logos_test.h>

#include "engine/canon.h"
#include "engine/stylepatch.h"
#include "test_util.h"

using nlohmann::json;
using namespace maps::engine;

namespace {
json style() { return mt::readJsonFixture("styles/mini-bright.json"); }
const json* layer(const json& s, const char* id) {
    for (const auto& l : s["layers"]) if (l["id"] == id) return &l;
    return nullptr;
}
json ids(const json& s) {
    json out = json::array();
    for (const auto& l : s["layers"]) out.push_back(l["id"]);
    return out;
}
}  // namespace

LOGOS_TEST(patch_glyphs_sprite_and_source_url) {
    json ops = json::array({
        mt::op("glyphs", {{"value", "logos://maps/glyphs/{fontstack}/{range}.pbf"}}),
        mt::op("sprite", {{"value", "asset://sprites/ofm"}}),
        mt::op("sourceUrl", {{"source", "openmaptiles"}, {"url", "pmtiles://planet.pmtiles"}}),
        mt::op("sourceUrl", {{"source", "hillshade"}, {"url", "logos://terrain/dem.json"}}),
        mt::op("sourceUrl", {{"source", "nope"}, {"url", "x"}}),
    });
    PatchReport rep;
    json out = applyStylePatch(style(), ops, &rep);
    LOGOS_ASSERT_EQ(out["glyphs"], json("logos://maps/glyphs/{fontstack}/{range}.pbf"));
    LOGOS_ASSERT_EQ(out["sprite"], json("asset://sprites/ofm"));
    LOGOS_ASSERT_EQ(out["sources"]["openmaptiles"]["url"], json("pmtiles://planet.pmtiles"));
    LOGOS_ASSERT_EQ(out["sources"]["hillshade"]["url"], json("logos://terrain/dem.json"));
    LOGOS_ASSERT_FALSE(out["sources"]["hillshade"].contains("tiles"));
    LOGOS_ASSERT_EQ(rep.warnings.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(ids(out), ids(style()));
}

LOGOS_TEST(patch_label_language_rewrites_name_fields_only) {
    json ops = json::array({mt::op("labelLanguage", {{"lang", "de"}})});
    json out = applyStylePatch(style(), ops);
    const json expect = labelExpression("de");
    LOGOS_ASSERT_EQ(expect, json::parse(R"(["coalesce",["get","name:de"],["get","name"]])"));
    LOGOS_ASSERT_EQ((*layer(out, "waterway_line_label"))["layout"]["text-field"], expect);  // "{name}" token form
    LOGOS_ASSERT_EQ((*layer(out, "place_label"))["layout"]["text-field"], expect);           // ["get","name"]
    LOGOS_ASSERT_EQ((*layer(out, "poi_label"))["layout"]["text-field"], expect);             // existing coalesce
    // icon-image / non-symbol layers untouched
    LOGOS_ASSERT_EQ((*layer(out, "poi_label"))["layout"]["icon-image"], json("{class}"));
    // "local" restores plain name; custom lang_key template is honoured.
    json back = applyStylePatch(out, json::array({mt::op("labelLanguage", {{"lang", "local"}})}));
    LOGOS_ASSERT_EQ((*layer(back, "place_label"))["layout"]["text-field"], json::parse(R"(["get","name"])"));
    LOGOS_ASSERT_EQ(labelExpression("en", "name_{lang}"), json::parse(R"(["coalesce",["get","name_en"],["get","name"]])"));
}

LOGOS_TEST(patch_font_stack_hide_layers_and_paint) {
    json ops = json::array({
        mt::op("fontStack", {{"from", json::array({"Noto Sans Italic"})}, {"to", json::array({"Open Sans Italic"})}}),
        mt::op("hideLayers", {{"ids", json::array({"building-3d"})}, {"prefixes", json::array({"hill"})}}),
        mt::op("setPaint", {{"layer", "water"}, {"props", {{"fill-color", "#0000ff"}}}}),
        mt::op("setLayout", {{"layers", json::array({"place_label", "poi_label"})}, {"props", {{"text-size", 14}}}}),
        mt::op("setPaint", {{"layer", "background"}, {"props", {{"background-color", nullptr}, {"background-opacity", 0.5}}}}),
    });
    PatchReport rep;
    json out = applyStylePatch(style(), ops, &rep);
    LOGOS_ASSERT_EQ((*layer(out, "waterway_line_label"))["layout"]["text-font"], json::array({"Open Sans Italic"}));
    LOGOS_ASSERT_EQ((*layer(out, "place_label"))["layout"]["text-font"], json::array({"Noto Sans Regular"}));  // not in `from`
    LOGOS_ASSERT_EQ((*layer(out, "building-3d"))["layout"]["visibility"], json("none"));
    LOGOS_ASSERT_FALSE((*layer(out, "building")).contains("layout"));
    LOGOS_ASSERT_EQ((*layer(out, "water"))["paint"]["fill-color"], json("#0000ff"));
    LOGOS_ASSERT_EQ((*layer(out, "poi_label"))["layout"]["text-size"], json(14));
    LOGOS_ASSERT_FALSE((*layer(out, "background"))["paint"].contains("background-color"));
    LOGOS_ASSERT_EQ((*layer(out, "background"))["paint"]["background-opacity"], json(0.5));
    LOGOS_ASSERT_EQ(ids(out), ids(style()));

    // Without `from`, every literal text-font is rewritten.
    json all = applyStylePatch(style(), json::array({mt::op("fontStack", {{"to", json::array({"X"})}})}));
    for (const auto& l : all["layers"])
        if (l.contains("layout") && l["layout"].contains("text-font")) LOGOS_ASSERT_EQ(l["layout"]["text-font"], json::array({"X"}));
}

LOGOS_TEST(patch_is_idempotent_and_keeps_layer_identity) {
    const json d = mt::readJsonFixture("basemaps/openfreemap-bright.json");
    json ops = d["patch"];
    ops.push_back(mt::op("labelLanguage", {{"lang", "fr"}}));
    ops.push_back(mt::op("hideLayers", {{"prefixes", json::array({"building"})}}));
    ops.push_back(mt::op("setPaint", {{"layer", "water"}, {"props", {{"fill-opacity", 0.8}}}}));
    const json once = applyStylePatch(style(), ops);
    const json twice = applyStylePatch(once, ops);
    LOGOS_ASSERT_EQ(canonical(once), canonical(twice));
    LOGOS_ASSERT_EQ(ids(once), ids(style()));
}

LOGOS_TEST(patch_json_patch_escape_hatch_cannot_touch_layer_identity) {
    json ok = json::array({mt::op("jsonPatch", {{"patch", json::array({{{"op", "replace"}, {"path", "/name"}, {"value", "Renamed"}},
                                                                        {{"op", "add"}, {"path", "/layers/1/minzoom"}, {"value", 4}}})}})});
    json out = applyStylePatch(style(), ok);
    LOGOS_ASSERT_EQ(out["name"], json("Renamed"));
    LOGOS_ASSERT_EQ(out["layers"][1]["minzoom"], json(4));

    json removes = json::array({mt::op("jsonPatch", {{"patch", json::array({{{"op", "remove"}, {"path", "/layers/0"}}})}})});
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), removes));
    json renames = json::array({mt::op("jsonPatch", {{"patch", json::array({{{"op", "replace"}, {"path", "/layers/0/id"}, {"value", "bg"}}})}})});
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), renames));
    json badPath = json::array({mt::op("jsonPatch", {{"patch", json::array({{{"op", "replace"}, {"path", "/nope/x"}, {"value", 1}}})}})});
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), badPath));
}

LOGOS_TEST(patch_rejects_malformed_ops) {
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), json::array({mt::op("glyphs")})));
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), json::array({mt::op("teleport")})));
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), json::array({mt::op("setPaint", {{"props", json::object()}})})));
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), json::array({mt::op("sourceUrl", {{"source", "openmaptiles"}})})));
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), json("not a style")));
    LOGOS_ASSERT_THROWS(applyStylePatch(style(), json{{"op", "glyphs"}}));
}
