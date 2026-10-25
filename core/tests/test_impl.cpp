// MapsCoreImpl: the §12.1 surface over the engine -- caller identity,
// result/error shapes, events, the view protocol, basemaps and providers.
#include <logos_test.h>

#include "maps_core_impl.h"
#include "test_util.h"

using nlohmann::json;
using mt::op;

namespace {

std::string batch(json ops) { return ops.dump(); }

json basic() {
    return json::array({
        op("putSource", {{"id", "route"}, {"type", "geojson"}, {"data", mt::points(2)}}),
        op("putLayer", {{"id", "line"}, {"type", "line"}, {"source", "route"}, {"placement", {{"anchor", "routes"}}}}),
    });
}

json last(logos_test::EventCapture& ev, const char* name) {
    auto all = ev.all(name);
    return all.empty() ? json() : json::parse(all.back().data);
}

std::string code(const StdLogosResult& r) { return r.error.substr(0, r.error.find(':')); }

}  // namespace

LOGOS_TEST(impl_apply_batch_returns_result_and_publishes_event) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    const auto caller = logos::CallCaller::module("routing");
    StdLogosResult r = impl.applyBatch("routing/main", batch(basic()));
    LOGOS_ASSERT_TRUE(r.success);
    LOGOS_ASSERT_EQ(r.value["revision"], json(1));
    LOGOS_ASSERT_EQ(r.value["accepted"].size(), std::size_t(2));
    LOGOS_ASSERT_TRUE(ev.has("revisionPublished"));
    LOGOS_ASSERT_EQ(last(ev, "revisionPublished")["sessions"], json::array({"routing/main"}));

    // Scene and changes are readable.
    StdLogosResult s = impl.getScene(std::nullopt);
    LOGOS_ASSERT_TRUE(s.success);
    LOGOS_ASSERT_TRUE(s.value["sessions"].contains("routing/main"));
    StdLogosResult one = impl.getScene(std::string("routing/main"));
    LOGOS_ASSERT_EQ(one.value["owner"], json("routing"));
    StdLogosResult c = impl.getChanges(0);
    LOGOS_ASSERT_EQ(c.value["ops"].size(), std::size_t(3));  // putSource, replaceGeoJson, putLayer
    LOGOS_ASSERT_EQ(code(impl.getScene(std::string("no/such"))), std::string("maps.unknownSession"));
}

LOGOS_TEST(impl_enforces_caller_session_prefix_and_ownership) {
    MapsCoreImpl impl;
    {
        const auto caller = logos::CallCaller::module("routing");
        StdLogosResult r = impl.applyBatch("other/main", batch(basic()));
        LOGOS_ASSERT_FALSE(r.success);
        LOGOS_ASSERT_EQ(code(r), std::string("maps.badArgument"));
        LOGOS_ASSERT_TRUE(impl.applyBatch("routing/main", batch(basic())).success);
    }
    {
        const auto caller = logos::CallCaller::module("intruder");
        LOGOS_ASSERT_FALSE(impl.applyBatch("routing/main", batch(basic())).success);
        LOGOS_ASSERT_FALSE(impl.dropSession("routing/main"));
    }
    // Host / unknown callers may touch any session (logosctl, tests).
    {
        const auto caller = logos::CallCaller::host();
        LOGOS_ASSERT_TRUE(impl.applyBatch("routing/main", batch(basic())).success);
    }
    LOGOS_ASSERT_TRUE(impl.dropSession("routing/main"));
    LOGOS_ASSERT_FALSE(impl.dropSession("routing/main"));
}

LOGOS_TEST(impl_malformed_input_is_coded_error_not_exception) {
    MapsCoreImpl impl;
    LOGOS_ASSERT_EQ(code(impl.applyBatch("a/b", "{not json")), std::string("maps.badArgument"));
    LOGOS_ASSERT_EQ(code(impl.applyBatch("a/b", R"({"v":2,"ops":[]})")), std::string("maps.badArgument"));
    LOGOS_ASSERT_EQ(code(impl.applyBatch("bad", "[]")), std::string("maps.badArgument"));
    LOGOS_ASSERT_EQ(code(impl.camera("nope", "{}")), std::string("maps.noSuchView"));
    LOGOS_ASSERT_EQ(code(impl.getChanges(99)), std::string("maps.badArgument"));
    LOGOS_ASSERT_EQ(code(impl.addBasemap("[]")), std::string("maps.badArgument"));
    LOGOS_ASSERT_EQ(code(impl.getBasemap("no/such")), std::string("maps.unknownId"));
    LOGOS_ASSERT_EQ(impl.schemaVersion(), std::string("maps/1"));
}

LOGOS_TEST(impl_view_protocol_open_ack_state_events_close) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    impl.applyBatch("routing/main", batch(basic()));

    StdLogosResult o = impl.openView("basecamp/main", "tok", R"({"render":{"terrain":true},"surface":{"transport":"wayland-shm"}})");
    LOGOS_ASSERT_TRUE(o.success);
    LOGOS_ASSERT_EQ(o.value["revision"], json(1));
    LOGOS_ASSERT_EQ(o.value["basemap"], json("openfreemap/bright"));
    LOGOS_ASSERT_TRUE(ev.has("viewOpened"));
    LOGOS_ASSERT_EQ(json::parse(impl.listViews())["views"].size(), std::size_t(1));

    LOGOS_ASSERT_TRUE(impl.ackRevision("basecamp/main", 1));
    LOGOS_ASSERT_FALSE(impl.ackRevision("basecamp/main", 1));
    LOGOS_ASSERT_EQ(last(ev, "revisionApplied")["revision"], json(1));

    LOGOS_ASSERT_TRUE(impl.reportViewState("basecamp/main", R"({"camera":{"center":[13.405,52.52],"zoom":12,"bearing":0,"pitch":0,"size":[800,600]},"dpr":2})"));
    StdLogosResult st = impl.getViewState("basecamp/main");
    LOGOS_ASSERT_EQ(st.value["dpr"], json(2));
    LOGOS_ASSERT_EQ(st.value["appliedRevision"], json(1));

    // toScreen/toGeo work from the reported camera.
    StdLogosResult ts = impl.toScreen("basecamp/main", "[[13.405,52.52]]");
    LOGOS_ASSERT_TRUE(ts.success);
    LOGOS_ASSERT_EQ(ts.value["points"][0], json::array({400.0, 300.0}));
    StdLogosResult tg = impl.toGeo("basecamp/main", "[[400,300]]");
    LOGOS_ASSERT_TRUE(tg.success);
    LOGOS_ASSERT_TRUE(std::fabs(tg.value["coordinates"][0][0].get<double>() - 13.405) < 1e-9);

    LOGOS_ASSERT_TRUE(impl.reportViewEvent("basecamp/main", "frameRendered", R"({"revision":1})"));
    LOGOS_ASSERT_FALSE(impl.reportViewEvent("basecamp/main", "bogusTopic", "{}"));
    LOGOS_ASSERT_EQ(last(ev, "viewEvent")["topic"], json("frameRendered"));
    LOGOS_ASSERT_EQ(json::parse(impl.listViews())["views"][0]["renderedRevision"], json(1));

    StdLogosResult caps = impl.getCapabilities(std::nullopt);
    LOGOS_ASSERT_EQ(caps.value["render"]["terrain"], json(true));
    LOGOS_ASSERT_EQ(caps.value["surface"]["transport"], json("wayland-shm"));
    LOGOS_ASSERT_EQ(caps.value["scene"]["patchFeatures"], json(true));
    LOGOS_ASSERT_EQ(impl.getCapabilities(std::string("basecamp/main")).value["viewId"], json("basecamp/main"));

    LOGOS_ASSERT_TRUE(impl.closeView("basecamp/main"));
    LOGOS_ASSERT_FALSE(impl.closeView("basecamp/main"));
    LOGOS_ASSERT_TRUE(ev.has("viewClosed"));
    LOGOS_ASSERT_EQ(code(impl.getViewState("basecamp/main")), std::string("maps.noSuchView"));
}

LOGOS_TEST(impl_camera_options_track_snapshot_relay_as_events) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    impl.openView("v/1", "t", "{}");
    LOGOS_ASSERT_TRUE(impl.camera("v/1", R"({"type":"flyTo","center":[1,2],"zoom":5})").success);
    LOGOS_ASSERT_EQ(last(ev, "cameraIntent")["intent"]["type"], json("flyTo"));
    LOGOS_ASSERT_FALSE(impl.camera("v/1", R"({"type":"teleport"})").success);
    LOGOS_ASSERT_FALSE(impl.camera("v/1", R"({"type":"fitBounds"})").success);
    LOGOS_ASSERT_FALSE(impl.camera("v/1", R"({"type":"jumpTo","center":"here"})").success);

    LOGOS_ASSERT_TRUE(impl.setViewOptions("v/1", R"({"padding":{"top":10},"gestures":{"rotate":false}})").success);
    LOGOS_ASSERT_TRUE(impl.setViewOptions("v/1", R"({"padding":null})").success);
    LOGOS_ASSERT_EQ(last(ev, "viewOptionsChanged")["options"], json::parse(R"({"gestures":{"rotate":false}})"));
    // Options survive a state report from the view.
    impl.reportViewState("v/1", R"({"camera":{"center":[0,0],"zoom":1}})");
    LOGOS_ASSERT_EQ(impl.getViewState("v/1").value["options"]["gestures"]["rotate"], json(false));

    LOGOS_ASSERT_TRUE(impl.trackCoordinates("v/1", "routing/main", R"(["a","b"])").success);
    LOGOS_ASSERT_EQ(last(ev, "trackRequested")["tracked"].size(), std::size_t(2));
    StdLogosResult snap = impl.snapshot("v/1", "");
    LOGOS_ASSERT_TRUE(snap.success);
    LOGOS_ASSERT_EQ(last(ev, "snapshotRequested")["requestId"], snap.value["requestId"]);
    LOGOS_ASSERT_TRUE(impl.clearCache());
    LOGOS_ASSERT_TRUE(ev.has("cacheClearRequested"));
}

LOGOS_TEST(impl_basemaps_catalogue_and_layer_order_per_view) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    json list = json::parse(impl.listBasemaps("{}"));
    LOGOS_ASSERT_GE(list["basemaps"].size(), std::size_t(3));
    LOGOS_ASSERT_EQ(json::parse(impl.listBasemaps(R"({"format":"raster"})"))["basemaps"].size(), std::size_t(1));

    impl.openView("v/1", "t", "{}");
    impl.applyBatch("routing/main", batch(basic()));
    StdLogosResult order = impl.getLayerOrder("v/1");
    LOGOS_ASSERT_TRUE(order.success);
    LOGOS_ASSERT_EQ(order.value["layers"][0]["id"], json("routing/main:line"));
    LOGOS_ASSERT_EQ(order.value["layers"][0]["before"], json("tunnel-service-track-casing"));  // bright's routes anchor
    LOGOS_ASSERT_TRUE(order.value["missingAnchors"].empty());

    // Switching to the raster basemap: anchors resolve to top, no anchorMissing.
    LOGOS_ASSERT_TRUE(impl.setBasemap("v/1", "osm/standard").success);
    LOGOS_ASSERT_EQ(last(ev, "basemapChanged")["id"], json("osm/standard"));
    LOGOS_ASSERT_EQ(impl.getLayerOrder("v/1").value["layers"][0]["before"], json(""));
    LOGOS_ASSERT_FALSE(ev.has("anchorMissing"));
    LOGOS_ASSERT_EQ(code(impl.setBasemap("v/1", "no/such")), std::string("maps.unknownId"));
    StdLogosResult raster = impl.getBasemap("osm/standard");
    LOGOS_ASSERT_TRUE(raster.value.contains("style"));
    LOGOS_ASSERT_FALSE(impl.getBasemap("openfreemap/bright").value.contains("style"));

    // A user descriptor lacking the `routes` anchor reports it when selected.
    json d = mt::readJsonFixture("basemaps/openfreemap-bright.json");
    d["id"] = "user/odd";
    d["anchors"] = json{{"labels", "place_label"}, {"routes", "nope"}};
    d["patch"] = json::array({json{{"op", "hideLayers"}, {"prefixes", json::array({"building"})}}});
    d["lang"] = "de";
    LOGOS_ASSERT_TRUE(impl.addBasemap(d.dump()).success);
    LOGOS_ASSERT_EQ(last(ev, "catalogueChanged")["action"], json("added"));
    LOGOS_ASSERT_TRUE(impl.setBasemap("v/1", "user/odd").success);
    LOGOS_ASSERT_FALSE(ev.has("anchorMissing"));  // labels/routes always exist by schema
    // A layer on an anchor this descriptor does not declare is reported.
    impl.applyBatch("routing/main", batch(json::array({
        op("putLayer", {{"id", "bld"}, {"type", "fill"}, {"source", "route"}, {"placement", {{"anchor", "buildings"}}}})})));
    LOGOS_ASSERT_TRUE(impl.setBasemap("v/1", "user/odd").success);
    LOGOS_ASSERT_EQ(last(ev, "anchorMissing")["anchor"], json("buildings"));
    LOGOS_ASSERT_EQ(impl.getLayerOrder("v/1").value["missingAnchors"], json::array({"buildings"}));

    // patchStyle applies descriptor rules plus the language and caches.
    const std::string style = mt::readFile(mt::fixturePath("styles/mini-bright.json"));
    StdLogosResult p = impl.patchStyle("user/odd", style);
    LOGOS_ASSERT_TRUE(p.success);
    bool hidden = false, de = false;
    for (const auto& l : p.value["layers"]) {
        if (l["id"] == "building" && l["layout"]["visibility"] == "none") hidden = true;
        if (l["id"] == "place_label" && l["layout"]["text-field"][1][1] == "name:de") de = true;
    }
    LOGOS_ASSERT_TRUE(hidden && de);
    LOGOS_ASSERT_EQ(impl.patchStyle("user/odd", style).value, p.value);
    LOGOS_ASSERT_FALSE(impl.patchStyle("user/odd", "nope").success);

    LOGOS_ASSERT_FALSE(impl.removeBasemap("openfreemap/bright"));  // bundled
    LOGOS_ASSERT_TRUE(impl.removeBasemap("user/odd"));
    LOGOS_ASSERT_FALSE(impl.removeBasemap("user/odd"));
}

LOGOS_TEST(impl_feature_state_and_providers) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    impl.applyBatch("routing/main", batch(basic()));
    LOGOS_ASSERT_TRUE(impl.setFeatureState("routing/main", "route", "1", R"({"hover":true})"));
    LOGOS_ASSERT_TRUE(impl.setFeatureState("routing/main", "route", "1", R"({"selected":true,"hover":null})"));
    LOGOS_ASSERT_EQ(last(ev, "featureStateChanged")["state"], json::parse(R"({"selected":true})"));
    LOGOS_ASSERT_FALSE(impl.setFeatureState("routing/main", "nope", "1", "{}"));
    LOGOS_ASSERT_TRUE(impl.clearFeatureState("routing/main", "route", std::string("1")));
    LOGOS_ASSERT_FALSE(impl.clearFeatureState("routing/main", "route", std::string("1")));
    impl.setFeatureState("routing/main", "route", "0", R"({"a":1})");
    LOGOS_ASSERT_TRUE(impl.clearFeatureState("routing/main", "route", std::nullopt));
    LOGOS_ASSERT_TRUE(last(ev, "featureStateChanged")["featureId"].is_null());

    {
        const auto caller = logos::CallCaller::module("tiles");
        LOGOS_ASSERT_TRUE(impl.registerProvider("tiles/main", "logos", R"({"module":"tile_store","prefix":"planet/"})").success);
        LOGOS_ASSERT_FALSE(impl.registerProvider("tiles/main", "http", R"({"module":"x"})").success);
    }
    LOGOS_ASSERT_EQ(json::parse(impl.listProviders())["providers"].size(), std::size_t(1));
    LOGOS_ASSERT_EQ(impl.getCapabilities(std::nullopt).value["provider"]["schemes"], json::array({"logos"}));
    LOGOS_ASSERT_TRUE(impl.unregisterProvider("tiles/main", "logos"));
    LOGOS_ASSERT_EQ(json::parse(impl.listProviders())["providers"].size(), std::size_t(0));
    LOGOS_ASSERT_EQ(json::parse(impl.cacheStats()), json::parse(R"({"v":1})"));
}
