// Layer order resolution against basemap anchors (M6.10).
#include <logos_test.h>

#include "engine/anchors.h"
#include "engine/store.h"
#include "test_util.h"

#include <algorithm>

using nlohmann::json;
using namespace maps::engine;
using mt::op;

namespace {

const json kAnchors = {{"routes", "waterway_line_label"}, {"labels", "place_label"}};

std::string orderString(const ResolvedOrder& o) {
    std::vector<std::string> v;
    for (const auto& l : o.layers) v.push_back(l.layerId + "@" + (l.before.empty() ? "top" : l.before));
    return mt::join(v, " ");
}

Store sceneWithTwoSessions() {
    Store st;
    st.applyBatch("routing/trip", json::array({
        op("putSource", {{"id", "r"}, {"type", "geojson"}}),
        op("putLayer", {{"id", "casing"}, {"type", "line"}, {"source", "r"}, {"placement", {{"anchor", "routes"}}}}),
        op("putLayer", {{"id", "line"}, {"type", "line"}, {"source", "r"}, {"placement", {{"above", "casing"}}}}),
        op("putLayer", {{"id", "arrows"}, {"type", "symbol"}, {"source", "r"}, {"placement", {{"anchor", "labels"}}}}),
    }), "routing");
    st.applyBatch("pois/near", json::array({
        op("putSource", {{"id", "p"}, {"type", "geojson"}}),
        op("putLayer", {{"id", "pins"}, {"type", "symbol"}, {"source", "p"}, {"placement", {{"anchor", "labels"}}}}),
        op("putLayer", {{"id", "halo"}, {"type", "circle"}, {"source", "p"}, {"placement", {{"before", "pins"}}}}),
        op("putLayer", {{"id", "debug"}, {"type", "circle"}, {"source", "p"}}),
    }), "pois");
    return st;
}

}  // namespace

LOGOS_TEST(anchors_group_by_anchor_then_session_priority_then_insertion) {
    Store st = sceneWithTwoSessions();
    ResolvedOrder o = resolveOrder(st.scene(), kAnchors);
    LOGOS_ASSERT_TRUE(o.missingAnchors.empty());
    LOGOS_ASSERT_TRUE(o.danglingRefs.empty());
    // anchors iterate in key order (labels, routes); within labels: pois (priority 0,
    // name "pois/near" < "routing/trip") before routing; halo before pins (before:pins).
    LOGOS_ASSERT_EQ(orderString(o), std::string(
        "pois/near:halo@place_label pois/near:pins@place_label routing/trip:arrows@place_label "
        "routing/trip:casing@waterway_line_label routing/trip:line@waterway_line_label "
        "pois/near:debug@top"));
}

LOGOS_TEST(anchors_session_priority_reorders_within_group) {
    Store st = sceneWithTwoSessions();
    st.setSessionPriority("pois/near", 10);  // higher = further up
    ResolvedOrder o = resolveOrder(st.scene(), kAnchors);
    LOGOS_ASSERT_EQ(orderString(o), std::string(
        "routing/trip:arrows@place_label pois/near:halo@place_label pois/near:pins@place_label "
        "routing/trip:casing@waterway_line_label routing/trip:line@waterway_line_label "
        "pois/near:debug@top"));
}

LOGOS_TEST(anchors_missing_anchor_falls_back_to_top_and_is_reported) {
    Store st = sceneWithTwoSessions();
    ResolvedOrder o = resolveOrder(st.scene(), json{{"labels", "place_label"}});  // no routes
    LOGOS_ASSERT_EQ(mt::join(o.missingAnchors), std::string("routes"));
    for (const auto& l : o.layers)
        if (l.layerId == "routing/trip:casing" || l.layerId == "routing/trip:line") {
            LOGOS_ASSERT_EQ(l.before, std::string(""));
            LOGOS_ASSERT_EQ(l.anchor, std::string("top"));
        }
    // `above casing` follows casing into the top group and stays right after it.
    std::vector<std::string> ids;
    for (const auto& l : o.layers) ids.push_back(l.layerId);
    auto c = std::find(ids.begin(), ids.end(), "routing/trip:casing");
    LOGOS_ASSERT_TRUE(c != ids.end() && std::next(c) != ids.end());
    LOGOS_ASSERT_EQ(*std::next(c), std::string("routing/trip:line"));
}

LOGOS_TEST(anchors_empty_string_anchor_is_known_top) {
    // Raster basemaps (M9.2): anchors point at top, nothing is "missing".
    Store st = sceneWithTwoSessions();
    ResolvedOrder o = resolveOrder(st.scene(), json{{"labels", ""}, {"routes", ""}});
    LOGOS_ASSERT_TRUE(o.missingAnchors.empty());
    for (const auto& l : o.layers) LOGOS_ASSERT_EQ(l.before, std::string(""));
    LOGOS_ASSERT_EQ(o.layers.size(), std::size_t(6));
}

LOGOS_TEST(anchors_dangling_before_and_cycles_fall_back) {
    Store st;
    st.applyBatch("a/s", json::array({
        op("putSource", {{"id", "src"}, {"type", "geojson"}}),
        op("putLayer", {{"id", "x"}, {"type", "circle"}, {"source", "src"}, {"placement", {{"before", "y"}}}}),
        op("putLayer", {{"id", "y"}, {"type", "circle"}, {"source", "src"}, {"placement", {{"before", "x"}}}}),
        op("putLayer", {{"id", "z"}, {"type", "circle"}, {"source", "src"}, {"placement", {{"above", "other/sess:gone"}}}}),
    }), "a");
    ResolvedOrder o = resolveOrder(st.scene(), kAnchors);
    LOGOS_ASSERT_EQ(o.layers.size(), std::size_t(3));
    LOGOS_ASSERT_FALSE(o.danglingRefs.empty());
    LOGOS_ASSERT_TRUE(std::find(o.danglingRefs.begin(), o.danglingRefs.end(), "other/sess:gone") != o.danglingRefs.end());
    for (const auto& l : o.layers) LOGOS_ASSERT_EQ(l.anchor, std::string("top"));
    const json j = o.toJson();
    LOGOS_ASSERT_EQ(j["v"], json(1));
    LOGOS_ASSERT_EQ(j["layers"].size(), std::size_t(3));
}

LOGOS_TEST(anchors_cross_session_before_inherits_target_group) {
    Store st = sceneWithTwoSessions();
    st.applyBatch("ext/x", json::array({
        op("putSource", {{"id", "s"}, {"type", "geojson"}}),
        op("putLayer", {{"id", "under"}, {"type", "circle"}, {"source", "s"}, {"placement", {{"before", "routing/trip:casing"}}}}),
    }), "ext");
    ResolvedOrder o = resolveOrder(st.scene(), kAnchors);
    std::vector<std::string> ids;
    for (const auto& l : o.layers) ids.push_back(l.layerId);
    auto u = std::find(ids.begin(), ids.end(), "ext/x:under");
    LOGOS_ASSERT_TRUE(u != ids.end());
    LOGOS_ASSERT_EQ(*std::next(u), std::string("routing/trip:casing"));
    LOGOS_ASSERT_EQ(o.layers[std::distance(ids.begin(), u)].before, std::string("waterway_line_label"));
}
