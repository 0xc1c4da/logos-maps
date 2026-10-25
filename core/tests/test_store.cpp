// SceneBatch semantics: validation, per-op rejection, coalescing, identity
// gate, atomicity, revisions, ownership (M6.4–M6.7, M11.2, M12.1).
#include <logos_test.h>

#include "engine/store.h"
#include "test_util.h"

using nlohmann::json;
using namespace maps::engine;
using mt::op;
using mt::points;

namespace {
const std::string S = "routing/main";

json basicBatch() {
    return json::array({
        op("putSource", {{"id", "route"}, {"type", "geojson"}, {"data", points(2)}}),
        op("putLayer", {{"id", "line"}, {"type", "line"}, {"source", "route"},
                        {"paint", {{"line-color", "#f00"}}}, {"placement", {{"anchor", "routes"}}}}),
    });
}
}  // namespace

LOGOS_TEST(batch_accepts_array_or_versioned_object) {
    Store st;
    auto r1 = st.applyBatch(S, basicBatch(), "routing");
    LOGOS_ASSERT_EQ(r1.revision, std::uint64_t(1));
    LOGOS_ASSERT_TRUE(r1.published);
    LOGOS_ASSERT_EQ(r1.accepted.size(), std::size_t(2));
    LOGOS_ASSERT_TRUE(r1.rejected.empty());

    Store st2;
    auto r2 = st2.applyBatch(S, json{{"v", 1}, {"ops", basicBatch()}}, "routing");
    LOGOS_ASSERT_EQ(r2.ops, r1.ops);
}

LOGOS_TEST(batch_rejects_malformed_payload_as_whole_call) {
    Store st;
    LOGOS_ASSERT_THROWS(st.applyBatch(S, json("nope"), "routing"));
    LOGOS_ASSERT_THROWS(st.applyBatch(S, json{{"v", 2}, {"ops", json::array()}}, "routing"));
    LOGOS_ASSERT_THROWS(st.applyBatch(S, json{{"v", 1}}, "routing"));
    LOGOS_ASSERT_THROWS(st.applyBatch("nosession", basicBatch(), "routing"));
    bool coded = false;
    try { st.applyBatch(S, json(42), "routing"); } catch (const MapsError& e) { coded = e.code == "maps.badArgument"; }
    LOGOS_ASSERT_TRUE(coded);
    LOGOS_ASSERT_EQ(st.revision(), std::uint64_t(0));
}

LOGOS_TEST(invalid_ops_are_rejected_individually_and_valid_ones_apply) {
    Store st;
    json b = basicBatch();
    b.push_back(op("putLayer", {{"id", "bad"}, {"type", "line"}, {"source", "missing"}}));
    b.push_back(op("putLayer", {{"id", "bad2"}, {"type", "hologram"}, {"source", "route"}}));
    b.push_back(op("frobnicate", {{"id", "x"}}));
    b.push_back(op("putLayer", {{"id", "ok"}, {"type", "circle"}, {"source", "route"}}));
    auto r = st.applyBatch(S, b, "routing");
    LOGOS_ASSERT_EQ(r.accepted.size(), std::size_t(3));
    LOGOS_ASSERT_EQ(r.rejected.size(), std::size_t(3));
    LOGOS_ASSERT_EQ(r.rejected[0].index, std::size_t(2));
    LOGOS_ASSERT_EQ(r.rejected[0].code, std::string("maps.unknownId"));
    LOGOS_ASSERT_EQ(r.rejected[1].code, std::string("maps.badArgument"));
    LOGOS_ASSERT_EQ(r.rejected[2].code, std::string("maps.badArgument"));
    LOGOS_ASSERT_TRUE(st.scene().findLayer("routing/main:ok") != nullptr);
    LOGOS_ASSERT_TRUE(st.scene().findLayer("routing/main:bad") == nullptr);
    const json rj = r.toJson();
    LOGOS_ASSERT_EQ(rj["rejected"].size(), std::size_t(3));
    LOGOS_ASSERT_EQ(rj["v"], json(1));
}

LOGOS_TEST(all_rejected_batch_leaves_no_ghost_session) {
    Store st;
    auto r = st.applyBatch(S, json::array({op("removeLayer", {{"id", "nope"}})}), "routing");
    LOGOS_ASSERT_FALSE(r.published);
    LOGOS_ASSERT_EQ(r.rejected.size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(st.scene().findSession(S) == nullptr);
}

LOGOS_TEST(unknown_keys_are_ignored_within_known_version) {
    Store st;
    json b = basicBatch();
    b[0]["futureKey"] = {{"x", 1}};
    auto r = st.applyBatch(S, json{{"v", 1}, {"ops", b}, {"extra", true}}, "routing");
    LOGOS_ASSERT_TRUE(r.rejected.empty());
}

LOGOS_TEST(published_ops_are_ordered_sources_images_layers) {
    Store st;
    json b = json::array({
        op("putLayer", {{"id", "l"}, {"type", "symbol"}, {"source", "s"}, {"layout", {{"icon-image", "pin"}}}}),
        op("putImage", {{"id", "pin"}, {"bytes", "aGVsbG8="}}),
        op("putSource", {{"id", "s"}, {"type", "geojson"}, {"data", points(1)}}),
    });
    auto r = st.applyBatch(S, b, "routing");
    // putLayer before putSource within the batch is rejected (unknown source);
    // the batch is not reordered before validation. Re-issue in order.
    LOGOS_ASSERT_EQ(r.rejected.size(), std::size_t(1));
    auto r2 = st.applyBatch(S, json::array({b[0]}), "routing");
    LOGOS_ASSERT_TRUE(r2.rejected.empty());
    const auto t1 = mt::opTokens(r.ops);
    LOGOS_ASSERT_EQ(mt::join(t1), std::string("putSource:routing/main:s,replaceGeoJson:routing/main:s,putImage:routing/main:pin"));
    LOGOS_ASSERT_EQ(mt::join(mt::opTokens(r2.ops)), std::string("putLayer:routing/main:l"));
}

LOGOS_TEST(coalescing_last_state_wins_within_batch) {
    Store st;
    json b = basicBatch();
    b.push_back(op("setPaint", {{"layerId", "line"}, {"props", {{"line-color", "#0f0"}}}}));
    b.push_back(op("setPaint", {{"layerId", "line"}, {"props", {{"line-width", 3}}}}));
    b.push_back(op("replaceGeoJson", {{"sourceId", "route"}, {"data", points(3)}}));
    b.push_back(op("replaceGeoJson", {{"sourceId", "route"}, {"data", points(4)}}));
    auto r = st.applyBatch(S, b, "routing");
    LOGOS_ASSERT_TRUE(r.rejected.empty());
    // One putLayer carrying the merged final paint, one replaceGeoJson with the last data.
    int layers = 0, geo = 0;
    for (const auto& o : r.ops) {
        if (o["op"] == "putLayer") { ++layers; LOGOS_ASSERT_EQ(o["paint"], json({{"line-color", "#0f0"}, {"line-width", 3}})); }
        if (o["op"] == "replaceGeoJson") { ++geo; LOGOS_ASSERT_EQ(o["data"]["features"].size(), std::size_t(4)); }
    }
    LOGOS_ASSERT_EQ(layers, 1);
    LOGOS_ASSERT_EQ(geo, 1);
}

LOGOS_TEST(coalescing_remove_cancels_earlier_put) {
    Store st;
    json b = basicBatch();
    b.push_back(op("putLayer", {{"id", "tmp"}, {"type", "circle"}, {"source", "route"}}));
    b.push_back(op("removeLayer", {{"id", "tmp"}}));
    auto r = st.applyBatch(S, b, "routing");
    LOGOS_ASSERT_TRUE(r.rejected.empty());
    for (const auto& o : r.ops) LOGOS_ASSERT_NE(o.value("id", ""), std::string("routing/main:tmp"));
    LOGOS_ASSERT_TRUE(st.scene().findLayer("routing/main:tmp") == nullptr);
}

LOGOS_TEST(coalescing_put_after_remove_is_a_replace) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    json b = json::array({
        op("removeLayer", {{"id", "line"}}),
        op("putLayer", {{"id", "line"}, {"type", "line"}, {"source", "route"}, {"paint", {{"line-color", "#00f"}}}}),
    });
    auto r = st.applyBatch(S, b, "routing");
    LOGOS_ASSERT_EQ(mt::join(mt::opTokens(r.ops)), std::string("putLayer:routing/main:line"));
}

LOGOS_TEST(identity_gate_drops_unchanged_geojson) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    const auto rev = st.revision();
    // Same data, different key order and formatting.
    json same = json::parse(points(2).dump());
    auto r = st.applyBatch(S, json::array({op("replaceGeoJson", {{"sourceId", "route"}, {"data", same}})}), "routing");
    LOGOS_ASSERT_FALSE(r.published);
    LOGOS_ASSERT_EQ(r.revision, rev);
    LOGOS_ASSERT_EQ(r.noop.size(), std::size_t(1));
    LOGOS_ASSERT_TRUE(r.rejected.empty());
    LOGOS_ASSERT_EQ(st.revision(), rev);
    // Changed data publishes.
    auto r2 = st.applyBatch(S, json::array({op("replaceGeoJson", {{"sourceId", "route"}, {"data", points(2, 1.0)}})}), "routing");
    LOGOS_ASSERT_TRUE(r2.published);
    LOGOS_ASSERT_EQ(r2.revision, rev + 1);
}

LOGOS_TEST(idempotent_rebatch_is_noop) {
    Store st;
    auto r1 = st.applyBatch(S, basicBatch(), "routing");
    auto r2 = st.applyBatch(S, basicBatch(), "routing");
    LOGOS_ASSERT_TRUE(r1.published);
    LOGOS_ASSERT_FALSE(r2.published);
    LOGOS_ASSERT_EQ(r2.noop.size(), std::size_t(2));
}

LOGOS_TEST(revisions_are_monotonic_and_logged) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    st.applyBatch(S, json::array({op("setPaint", {{"layerId", "line"}, {"props", {{"line-width", 2}}}})}), "routing");
    st.applyBatch("other/sess", json::array({op("putSource", {{"id", "p"}, {"type", "geojson"}})}), "other");
    LOGOS_ASSERT_EQ(st.revision(), std::uint64_t(3));
    json c = st.changesSince(1);
    LOGOS_ASSERT_EQ(c["from"], json(1));
    LOGOS_ASSERT_EQ(c["to"], json(3));
    LOGOS_ASSERT_FALSE(c.contains("full"));
    LOGOS_ASSERT_EQ(c["ops"].size(), std::size_t(3));  // putLayer (rev2) + putSource + replaceGeoJson (rev3)
    LOGOS_ASSERT_EQ(c["ops"][0]["revision"], json(2));
    LOGOS_ASSERT_EQ(c["ops"][1]["session"], json("other/sess"));
    LOGOS_ASSERT_EQ(st.changesSince(3)["ops"].size(), std::size_t(0));
    LOGOS_ASSERT_THROWS(st.changesSince(9));
}

LOGOS_TEST(changes_outside_window_fall_back_to_full_scene) {
    Limits lim;
    lim.revisionLogLength = 2;
    Store st(lim);
    st.applyBatch(S, basicBatch(), "routing");
    for (int i = 0; i < 4; ++i)
        st.applyBatch(S, json::array({op("setPaint", {{"layerId", "line"}, {"props", {{"line-width", i + 1}}}})}), "routing");
    json c = st.changesSince(1);
    LOGOS_ASSERT_TRUE(c.value("full", false));
    LOGOS_ASSERT_EQ(c["scene"]["revision"], json(5));
    LOGOS_ASSERT_FALSE(st.changesSince(3).value("full", false));
}

LOGOS_TEST(session_ownership_is_enforced_when_caller_known) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    LOGOS_ASSERT_THROWS(st.applyBatch(S, basicBatch(), "intruder"));
    // Unknown caller (logosctl, tests) is allowed through.
    st.applyBatch(S, basicBatch(), "");
    LOGOS_ASSERT_THROWS(st.dropSession(S, "intruder"));
    LOGOS_ASSERT_TRUE(st.dropSession(S, "routing"));
    LOGOS_ASSERT_FALSE(st.dropSession(S, "routing"));
}

LOGOS_TEST(drop_session_publishes_removals) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    const auto rev = st.revision();
    LOGOS_ASSERT_TRUE(st.dropSession(S, "routing"));
    LOGOS_ASSERT_EQ(st.revision(), rev + 1);
    json c = st.changesSince(rev);
    LOGOS_ASSERT_EQ(c["ops"][0]["op"], json("dropSession"));
    LOGOS_ASSERT_TRUE(st.scene().findSession(S) == nullptr);
    // dropSession as an op inside a batch empties but keeps the session.
    st.applyBatch(S, basicBatch(), "routing");
    auto r = st.applyBatch(S, json::array({op("dropSession")}), "routing");
    LOGOS_ASSERT_TRUE(r.published);
    LOGOS_ASSERT_EQ(mt::join(mt::opTokens(r.ops)), std::string("removeLayer:routing/main:line,removeSource:routing/main:route"));
}

LOGOS_TEST(remove_source_removes_dependent_layers) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    auto r = st.applyBatch(S, json::array({op("removeSource", {{"id", "route"}})}), "routing");
    LOGOS_ASSERT_EQ(mt::join(mt::opTokens(r.ops)), std::string("removeLayer:routing/main:line,removeSource:routing/main:route"));
    LOGOS_ASSERT_TRUE(st.scene().findLayer("routing/main:line") == nullptr);
}

LOGOS_TEST(patch_features_requires_ids_and_upserts_by_id) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    json upsert = json::array({{{"type", "Feature"}, {"id", 1}, {"geometry", {{"type", "Point"}, {"coordinates", {9, 9}}}}, {"properties", json::object()}},
                               {{"type", "Feature"}, {"id", 7}, {"geometry", {{"type", "Point"}, {"coordinates", {7, 7}}}}, {"properties", json::object()}}});
    auto r = st.applyBatch(S, json::array({op("patchFeatures", {{"sourceId", "route"}, {"upsert", upsert}, {"remove", json::array({0})}})}), "routing");
    LOGOS_ASSERT_TRUE(r.rejected.empty());
    const Source* src = st.scene().findSource("routing/main:route");
    LOGOS_ASSERT_EQ(src->data["features"].size(), std::size_t(2));
    LOGOS_ASSERT_EQ(src->data["features"][0]["id"], json(1));
    LOGOS_ASSERT_EQ(src->data["features"][0]["geometry"]["coordinates"], json({9, 9}));
    LOGOS_ASSERT_EQ(src->data["features"][1]["id"], json(7));
    // Views receive one replaceGeoJson (patches are a core convenience).
    LOGOS_ASSERT_EQ(mt::join(mt::opTokens(r.ops)), std::string("replaceGeoJson:routing/main:route"));

    // A source without ids refuses patches.
    json noIds = json{{"type", "FeatureCollection"}, {"features", json::array({{{"type", "Feature"}, {"geometry", nullptr}, {"properties", json::object()}}})}};
    st.applyBatch(S, json::array({op("putSource", {{"id", "anon"}, {"type", "geojson"}, {"data", noIds}})}), "routing");
    auto r2 = st.applyBatch(S, json::array({op("patchFeatures", {{"sourceId", "anon"}, {"upsert", upsert}}) }), "routing");
    LOGOS_ASSERT_EQ(r2.rejected.size(), std::size_t(1));
    LOGOS_ASSERT_EQ(r2.rejected[0].code, std::string("maps.badArgument"));
}

LOGOS_TEST(source_definition_and_data_are_distinct) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    // Changing def republishes the source (and its data) but keeps data when none given.
    auto r = st.applyBatch(S, json::array({op("putSource", {{"id", "route"}, {"type", "geojson"}, {"def", {{"cluster", true}}}})}), "routing");
    LOGOS_ASSERT_EQ(mt::join(mt::opTokens(r.ops)), std::string("putSource:routing/main:route,replaceGeoJson:routing/main:route"));
    LOGOS_ASSERT_EQ(st.scene().findSource("routing/main:route")->data["features"].size(), std::size_t(2));
    LOGOS_ASSERT_EQ(st.scene().findSource("routing/main:route")->def["cluster"], json(true));
    // `data` nested inside def is hoisted out of the definition.
    auto r2 = st.applyBatch(S, json::array({op("putSource", {{"id", "d"}, {"type", "geojson"}, {"def", {{"data", points(1)}, {"tolerance", 0.5}}}})}), "routing");
    LOGOS_ASSERT_TRUE(r2.rejected.empty());
    LOGOS_ASSERT_FALSE(st.scene().findSource("routing/main:d")->def.contains("data"));
    LOGOS_ASSERT_EQ(st.scene().findSource("routing/main:d")->def["tolerance"], json(0.5));
}

LOGOS_TEST(layer_validation_covers_types_zoom_filter_and_vector_source_layer) {
    Store st;
    json b = json::array({
        op("putSource", {{"id", "v"}, {"type", "vector"}, {"def", {{"url", "https://x/tiles.json"}}}}),
        op("putLayer", {{"id", "needsSL"}, {"type", "fill"}, {"source", "v"}}),                                   // 1: rejected
        op("putLayer", {{"id", "ok"}, {"type", "fill"}, {"source", "v"}, {"sourceLayer", "water"}}),              // 2
        op("putLayer", {{"id", "zoom"}, {"type", "fill"}, {"source", "v"}, {"sourceLayer", "w"}, {"minzoom", 30}}), // 3: rejected
        op("putLayer", {{"id", "filt"}, {"type", "fill"}, {"source", "v"}, {"sourceLayer", "w"}, {"filter", "bad"}}), // 4: rejected
        op("putLayer", {{"id", "bg"}, {"type", "background"}}),                                                     // 5: ok, no source
        op("putSource", {{"id", "r"}, {"type", "raster"}, {"def", json::object()}}),                                 // 6: rejected, no url/tiles
        op("putSource", {{"id", "withdata"}, {"type", "raster"}, {"def", {{"tiles", json::array({"a"})}}}, {"data", points(1)}}), // 7: rejected
    });
    auto r = st.applyBatch(S, b, "routing");
    json rej = json::array();
    for (const auto& x : r.rejected) rej.push_back(x.index);
    LOGOS_ASSERT_EQ(rej, json::array({1, 3, 4, 6, 7}));
}

LOGOS_TEST(set_props_merge_and_null_deletes) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    st.applyBatch(S, json::array({op("setPaint", {{"layerId", "line"}, {"props", {{"line-width", 4}, {"line-color", nullptr}}}}),
                                  op("setLayout", {{"layerId", "line"}, {"props", {{"line-cap", "round"}}}}),
                                  op("setFilter", {{"layerId", "line"}, {"filter", json::array({"==", "n", 1})}}),
                                  op("placeLayer", {{"layerId", "line"}, {"placement", {{"anchor", "labels"}}}})}), "routing");
    const Layer* l = st.scene().findLayer("routing/main:line");
    LOGOS_ASSERT_EQ(l->paint, json({{"line-width", 4}}));
    LOGOS_ASSERT_EQ(l->layout, json({{"line-cap", "round"}}));
    LOGOS_ASSERT_EQ(l->filter, json::array({"==", "n", 1}));
    LOGOS_ASSERT_TRUE(l->placement.kind == Placement::Kind::Anchor);
    LOGOS_ASSERT_EQ(l->placement.ref, std::string("labels"));
}

LOGOS_TEST(limits_are_enforced) {
    Limits lim;
    lim.maxBatchBytes = 200;
    lim.maxImageBytes = 4;
    Store st(lim);
    LOGOS_ASSERT_THROWS(st.applyBatch(S, json::array({op("putSource", {{"id", "big"}, {"type", "geojson"}, {"data", points(20)}})}), "routing"));
    auto r = st.applyBatch(S, json::array({op("putImage", {{"id", "i"}, {"bytes", "aGVsbG8gd29ybGQ="}})}), "routing");
    LOGOS_ASSERT_EQ(r.rejected.size(), std::size_t(1));
}

LOGOS_TEST(scene_json_reports_revision_and_sessions) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    json all = st.sceneJson();
    LOGOS_ASSERT_EQ(all["revision"], json(1));
    LOGOS_ASSERT_TRUE(all["sessions"].contains(S));
    json one = st.sceneJson(S);
    LOGOS_ASSERT_EQ(one["name"], json(S));
    LOGOS_ASSERT_EQ(one["owner"], json("routing"));
    LOGOS_ASSERT_THROWS(st.sceneJson(std::string("no/such")));
}

LOGOS_TEST(session_priority_change_publishes) {
    Store st;
    st.applyBatch(S, basicBatch(), "routing");
    LOGOS_ASSERT_TRUE(st.setSessionPriority(S, 5));
    LOGOS_ASSERT_EQ(st.revision(), std::uint64_t(2));
    LOGOS_ASSERT_TRUE(st.setSessionPriority(S, 5));
    LOGOS_ASSERT_EQ(st.revision(), std::uint64_t(2));
    LOGOS_ASSERT_FALSE(st.setSessionPriority("no/such", 1));
    LOGOS_ASSERT_EQ(st.scene().findSession(S)->priority, 5);
}
