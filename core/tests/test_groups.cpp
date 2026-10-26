// Groups, callouts, fitSession and the descriptor's thumbnail/details
// (rev 0.4: M6.3a, M6.3b, M7.4a, M9.1a, §12.1 listGroups/setGroupVisible/
// setCalloutContent, §16.8 core half).
#include <logos_test.h>

#include "engine/basemap.h"
#include "engine/capability.h"
#include "engine/store.h"
#include "maps_core_impl.h"
#include "test_util.h"

using nlohmann::json;
using namespace maps::engine;
using mt::op;

namespace {
const std::string S = "quests/main";

json groupedBatch() {
    return json::array({
        op("putSource", {{"id", "pts"}, {"type", "geojson"}, {"data", mt::points(3)}}),
        op("putGroup", {{"id", "waypoints"}, {"title", "Waypoints"}, {"order", 2},
                        {"legend", json::array({json{{"label", "Waypoint"}, {"color", "#0a0"}}})}}),
        op("putGroup", {{"id", "trail"}, {"title", "Trail"}, {"order", 1}, {"defaultVisible", false}, {"toggleable", false}}),
        op("putLayer", {{"id", "dots"}, {"type", "circle"}, {"source", "pts"}, {"group", "waypoints"},
                        {"callout", {{"title", "{name}"}, {"subtitle", "#{n}"},
                                     {"actions", json::array({json{{"id", "open"}, {"label", "Open"}}})}}}}),
        op("putLayer", {{"id", "plain"}, {"type", "circle"}, {"source", "pts"}}),
    });
}

json last(logos_test::EventCapture& ev, const char* name) {
    auto all = ev.all(name);
    return all.empty() ? json() : json::parse(all.back().data);
}

std::string code(const StdLogosResult& r) { return r.error.substr(0, r.error.find(':')); }
}  // namespace

// ── store ────────────────────────────────────────────────────────────────

LOGOS_TEST(groups_put_and_layer_membership_publish_in_order) {
    Store st;
    auto r = st.applyBatch(S, groupedBatch(), "quests");
    LOGOS_ASSERT_TRUE(r.rejected.empty());
    LOGOS_ASSERT_TRUE(r.published);
    // putGroup travels after sources/images and before the layers naming it.
    const auto toks = mt::opTokens(r.ops);
    std::size_t iGroup = 0, iLayer = 0;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] == "putGroup:quests/main:waypoints") iGroup = i;
        if (toks[i] == "putLayer:quests/main:dots") iLayer = i;
    }
    LOGOS_ASSERT_TRUE(iGroup < iLayer);
    LOGOS_ASSERT_TRUE(opRank("putGroup") < opRank("putLayer"));
    LOGOS_ASSERT_TRUE(opRank("putGroup") > opRank("putImage"));

    const Session* s = st.scene().findSession(S);
    LOGOS_ASSERT_EQ(s->groups.size(), std::size_t(2));
    const Layer* dots = s->findLayer("quests/main:dots");
    LOGOS_ASSERT_TRUE(dots->group.has_value());
    LOGOS_ASSERT_EQ(*dots->group, std::string("quests/main:waypoints"));
    LOGOS_ASSERT_EQ(dots->callout["title"], json("{name}"));
    LOGOS_ASSERT_EQ(dots->callout["actions"][0]["id"], json("open"));
    LOGOS_ASSERT_FALSE(s->findLayer("quests/main:plain")->group.has_value());

    // Scene JSON carries groups per session and group/callout per layer.
    const json scene = st.sceneJson(S);
    LOGOS_ASSERT_EQ(scene["groups"].size(), std::size_t(2));
    LOGOS_ASSERT_EQ(scene["groups"]["quests/main:trail"]["defaultVisible"], json(false));
    LOGOS_ASSERT_EQ(scene["groups"]["quests/main:trail"]["toggleable"], json(false));
    LOGOS_ASSERT_EQ(scene["groups"]["quests/main:waypoints"]["legend"][0]["color"], json("#0a0"));
    bool sawGroup = false;
    for (const auto& l : scene["layers"])
        if (l["id"] == "quests/main:dots") sawGroup = l.value("group", "") == "quests/main:waypoints" && l.contains("callout");
    LOGOS_ASSERT_TRUE(sawGroup);

    // Re-putting the same group is a no-op; changing a field republishes it.
    auto same = st.applyBatch(S, json::array({groupedBatch()[1]}), "quests");
    LOGOS_ASSERT_FALSE(same.published);
    auto changed = st.applyBatch(S, json::array({op("putGroup", {{"id", "waypoints"}, {"title", "Stops"}})}), "quests");
    LOGOS_ASSERT_TRUE(changed.published);
    LOGOS_ASSERT_EQ(mt::join(mt::opTokens(changed.ops)), std::string("putGroup:quests/main:waypoints"));
}

LOGOS_TEST(groups_validation_rejects_bad_shapes_and_unknown_group) {
    Store st;
    json b = json::array({
        op("putSource", {{"id", "pts"}, {"type", "geojson"}, {"data", mt::points(1)}}),
        op("putLayer", {{"id", "orphan"}, {"type", "circle"}, {"source", "pts"}, {"group", "nope"}}),     // 1 unknownId
        op("putGroup", {{"id", "g"}}),                                                                   // 2 no title
        op("putGroup", {{"id", "g"}, {"title", "G"}, {"order", 1.5}}),                                   // 3 order
        op("putGroup", {{"id", "g"}, {"title", "G"}, {"legend", json::array({json{{"color", "#fff"}}})}}),  // 4 legend label
        op("putGroup", {{"id", "g"}, {"title", "G"}, {"toggleable", "yes"}}),                             // 5 bool
        op("putGroup", {{"id", "g"}, {"title", "G"}}),                                                   // 6 ok
        op("putLayer", {{"id", "x"}, {"type", "circle"}, {"source", "pts"}, {"group", "other/s:g"}}),     // 7 cross-session group
        op("putLayer", {{"id", "c1"}, {"type", "circle"}, {"source", "pts"}, {"callout", "hello"}}),     // 8 callout shape
        op("putLayer", {{"id", "c2"}, {"type", "circle"}, {"source", "pts"}, {"callout", {{"subtitle", "x"}}}}),  // 9 no title
        op("putLayer", {{"id", "c4"}, {"type", "circle"}, {"source", "pts"}, {"selectable", "no"}}),     // 10 selectable
        op("putLayer", {{"id", "ok"}, {"type", "circle"}, {"source", "pts"}, {"group", "g"}, {"selectable", false}}),  // 11 ok
        op("removeGroup", {{"id", "missing"}}),                                                          // 12 unknownId
    });
    json nine = json::array();
    for (int i = 0; i < 9; ++i) nine.push_back(json{{"id", "a" + std::to_string(i)}, {"label", "A"}});
    b.push_back(op("putLayer", {{"id", "c3"}, {"type", "circle"}, {"source", "pts"}, {"callout", {{"title", "t"}, {"actions", nine}}}}));  // 13 > 8 actions
    b.push_back(op("putLayer", {{"id", "c5"}, {"type", "circle"}, {"source", "pts"},
                                {"callout", {{"title", "t"}, {"actions", json::array({json{{"id", "a"}, {"label", "A"}}, json{{"id", "a"}, {"label", "B"}}})}}}}));  // 14 dup action
    auto r = st.applyBatch(S, b, "quests");
    json rejected = json::array();
    for (const auto& x : r.rejected) rejected.push_back(x.index);
    LOGOS_ASSERT_EQ(rejected, json::parse("[1,2,3,4,5,7,8,9,10,12,13,14]"));
    LOGOS_ASSERT_EQ(r.rejected[0].code, std::string("maps.unknownId"));
    LOGOS_ASSERT_EQ(r.rejected[1].code, std::string("maps.badArgument"));
    const Layer* ok = st.scene().findLayer("quests/main:ok");
    LOGOS_ASSERT_TRUE(ok && ok->group && *ok->group == "quests/main:g");
    LOGOS_ASSERT_TRUE(ok->selectable && *ok->selectable == false);
}

LOGOS_TEST(groups_remove_ungroups_layers_and_drop_session_clears) {
    Store st;
    st.applyBatch(S, groupedBatch(), "quests");
    auto r = st.applyBatch(S, json::array({op("removeGroup", {{"id", "waypoints"}})}), "quests");
    LOGOS_ASSERT_TRUE(r.published);
    const auto toks = mt::opTokens(r.ops);
    // removeGroup is published, and the member layer is re-sent without its group.
    LOGOS_ASSERT_EQ(toks[0], std::string("removeGroup:quests/main:waypoints"));
    LOGOS_ASSERT_EQ(toks[1], std::string("putLayer:quests/main:dots"));
    LOGOS_ASSERT_FALSE(r.ops[1].contains("group"));
    const Session* s = st.scene().findSession(S);
    LOGOS_ASSERT_EQ(s->groups.size(), std::size_t(1));
    LOGOS_ASSERT_FALSE(s->findLayer("quests/main:dots")->group.has_value());

    // A session of only groups is content (it commits); dropSession removes them.
    Store st2;
    auto g = st2.applyBatch("a/b", json::array({op("putGroup", {{"id", "g"}, {"title", "G"}})}), "a");
    LOGOS_ASSERT_TRUE(g.published);
    LOGOS_ASSERT_TRUE(st2.scene().findSession("a/b") != nullptr);
    LOGOS_ASSERT_TRUE(st2.dropSession("a/b", "a"));
    LOGOS_ASSERT_TRUE(st2.scene().findSession("a/b") == nullptr);
    const json ch = st2.changesSince(1);
    LOGOS_ASSERT_EQ(ch["ops"][0]["op"], json("dropSession"));
    LOGOS_ASSERT_EQ(ch["ops"][1]["op"], json("removeGroup"));
}

LOGOS_TEST(session_extent_walks_every_geometry_kind) {
    Session s;
    Source src;
    src.id = "a/b:x"; src.type = "geojson";
    src.data = json{{"type", "FeatureCollection"}, {"features", json::array({
        json{{"type", "Feature"}, {"geometry", {{"type", "Point"}, {"coordinates", {10.0, 20.0}}}}},
        json{{"type", "Feature"}, {"geometry", {{"type", "LineString"}, {"coordinates", {{-5.0, 1.0}, {3.0, 30.0}}}}}},
        json{{"type", "Feature"}, {"geometry", {{"type", "GeometryCollection"},
              {"geometries", json::array({json{{"type", "Polygon"}, {"coordinates", {{{0.0, -8.0}, {1.0, -8.0}, {1.0, -7.0}, {0.0, -8.0}}}}}})}}}},
        json{{"type", "Feature"}, {"geometry", nullptr}},
    })}};
    s.sources[src.id] = src;
    Source raster; raster.id = "a/b:r"; raster.type = "raster";
    s.sources[raster.id] = raster;
    auto e = sessionExtent(s);
    LOGOS_ASSERT_TRUE(e.has_value());
    LOGOS_ASSERT_EQ(e->w, -5.0); LOGOS_ASSERT_EQ(e->e, 10.0);
    LOGOS_ASSERT_EQ(e->s, -8.0); LOGOS_ASSERT_EQ(e->n, 30.0);
    LOGOS_ASSERT_FALSE(sessionExtent(s, std::string("a/b:r")).has_value());
    Session empty;
    LOGOS_ASSERT_FALSE(sessionExtent(empty).has_value());
}

LOGOS_TEST(normalize_callout_is_shared_validation) {
    json c = normalizeCallout(json{{"title", "{name}"}, {"icon", "pin"}, {"actions", json::array({json{{"id", "go"}, {"label", "Go"}}})}});
    LOGOS_ASSERT_EQ(c["icon"], json("pin"));
    LOGOS_ASSERT_FALSE(c.contains("subtitle"));
    LOGOS_ASSERT_THROWS(normalizeCallout(json{{"subtitle", "no title"}}));
    LOGOS_ASSERT_THROWS(normalizeCallout(json("string")));
}

// ── impl ─────────────────────────────────────────────────────────────────

LOGOS_TEST(impl_list_groups_and_set_group_visible_per_view) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    LOGOS_ASSERT_TRUE(impl.applyBatch(S, groupedBatch().dump()).success);
    impl.applyBatch("other/x", json::array({op("putGroup", {{"id", "z"}, {"title", "Z"}, {"order", 0}})}).dump());

    json all = json::parse(impl.listGroups(std::nullopt));
    LOGOS_ASSERT_EQ(all["groups"].size(), std::size_t(3));
    // Sorted by order, then id: other/x:z (0), trail (1), waypoints (2).
    LOGOS_ASSERT_EQ(all["groups"][0]["id"], json("other/x:z"));
    LOGOS_ASSERT_EQ(all["groups"][1]["id"], json("quests/main:trail"));
    LOGOS_ASSERT_EQ(all["groups"][2]["id"], json("quests/main:waypoints"));
    LOGOS_ASSERT_EQ(all["groups"][2]["session"], json(S));
    LOGOS_ASSERT_EQ(all["groups"][2]["layers"], json::array({"quests/main:dots"}));
    json one = json::parse(impl.listGroups(std::string(S)));
    LOGOS_ASSERT_EQ(one["groups"].size(), std::size_t(2));

    impl.openView("v/1", "t", "{}");
    impl.openView("v/2", "t", "{}");
    LOGOS_ASSERT_FALSE(impl.setGroupVisible("v/9", S, "waypoints", false));
    LOGOS_ASSERT_FALSE(impl.setGroupVisible("v/1", S, "nope", false));
    LOGOS_ASSERT_TRUE(impl.setGroupVisible("v/1", S, "waypoints", false));
    LOGOS_ASSERT_EQ(last(ev, "groupVisibilityChanged")["groupId"], json("quests/main:waypoints"));
    LOGOS_ASSERT_EQ(last(ev, "groupVisibilityChanged")["visible"], json(false));
    LOGOS_ASSERT_EQ(last(ev, "viewOptionsChanged")["options"]["hiddenGroups"], json::array({"quests/main:waypoints"}));
    LOGOS_ASSERT_EQ(impl.getViewState("v/1").value["options"]["hiddenGroups"], json::array({"quests/main:waypoints"}));
    // The other view is untouched (visibility is per view).
    LOGOS_ASSERT_FALSE(impl.getViewState("v/2").value["options"].contains("hiddenGroups") &&
                       !impl.getViewState("v/2").value["options"]["hiddenGroups"].empty());
    // Idempotent: hiding again emits nothing new.
    const auto before = ev.all("groupVisibilityChanged").size();
    LOGOS_ASSERT_TRUE(impl.setGroupVisible("v/1", S, "waypoints", false));
    LOGOS_ASSERT_EQ(ev.all("groupVisibilityChanged").size(), before);
    // "" = every view; a global id is accepted too.
    LOGOS_ASSERT_TRUE(impl.setGroupVisible("", S, "quests/main:trail", false));
    LOGOS_ASSERT_EQ(impl.getViewState("v/2").value["options"]["hiddenGroups"], json::array({"quests/main:trail"}));
    LOGOS_ASSERT_TRUE(impl.setGroupVisible("v/1", S, "waypoints", true));
    LOGOS_ASSERT_EQ(impl.getViewState("v/1").value["options"]["hiddenGroups"], json::array({"quests/main:trail"}));
    // Options set by setViewOptions coexist with hiddenGroups.
    impl.setViewOptions("v/1", R"({"chrome":{"zoomButtons":true}})");
    LOGOS_ASSERT_EQ(impl.getViewState("v/1").value["options"]["hiddenGroups"], json::array({"quests/main:trail"}));
}

LOGOS_TEST(impl_set_callout_content_relays_normalised_content) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    LOGOS_ASSERT_FALSE(impl.setCalloutContent("v/1", R"({"title":"x"})"));  // no such view
    impl.openView("v/1", "t", "{}");
    LOGOS_ASSERT_FALSE(impl.setCalloutContent("v/1", R"({"subtitle":"no title"})"));
    LOGOS_ASSERT_FALSE(impl.setCalloutContent("v/1", "[]"));
    LOGOS_ASSERT_TRUE(impl.setCalloutContent("v/1", R"({"title":"Café","subtitle":"Open","actions":[{"id":"route","label":"Route"}],"junk":1})"));
    json e = last(ev, "calloutContentChanged");
    LOGOS_ASSERT_EQ(e["viewId"], json("v/1"));
    LOGOS_ASSERT_EQ(e["content"]["title"], json("Café"));
    LOGOS_ASSERT_FALSE(e["content"].contains("junk"));
    LOGOS_ASSERT_TRUE(impl.setCalloutContent("", R"({"title":"all"})"));
    LOGOS_ASSERT_EQ(last(ev, "calloutContentChanged")["viewId"], json(""));
}

LOGOS_TEST(impl_fit_session_resolves_bounds_or_point) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    impl.openView("v/1", "t", "{}");
    LOGOS_ASSERT_EQ(code(impl.camera("v/1", R"({"type":"fitSession"})")), std::string("maps.badArgument"));
    LOGOS_ASSERT_EQ(code(impl.camera("v/1", R"({"type":"fitSession","session":"no/such"})")), std::string("maps.unknownSession"));

    impl.applyBatch(S, groupedBatch().dump());  // points at (0,0),(1,0),(2,0)
    StdLogosResult r = impl.camera("v/1", R"({"type":"fitSession","session":"quests/main","padding":{"top":20},"animation":{"duration":500},"keepFollow":true})");
    LOGOS_ASSERT_TRUE(r.success);
    json intent = last(ev, "cameraIntent")["intent"];
    LOGOS_ASSERT_EQ(intent["type"], json("fitBounds"));
    LOGOS_ASSERT_EQ(intent["bounds"], json::parse("[[0,0],[2,0]]"));
    LOGOS_ASSERT_EQ(intent["maxZoom"], json(16.0));
    LOGOS_ASSERT_EQ(intent["padding"]["top"], json(20));
    LOGOS_ASSERT_EQ(intent["animation"]["duration"], json(500));
    LOGOS_ASSERT_EQ(intent["keepFollow"], json(true));
    LOGOS_ASSERT_EQ(intent["resolvedFrom"], json("fitSession"));

    // One point -> easeTo at maxZoom.
    impl.applyBatch("solo/p", json::array({op("putSource", {{"id", "s"}, {"type", "geojson"}, {"data", mt::points(1, 5.0)}})}).dump());
    LOGOS_ASSERT_TRUE(impl.camera("v/1", R"({"type":"fitSession","session":"solo/p","maxZoom":14})").success);
    intent = last(ev, "cameraIntent")["intent"];
    LOGOS_ASSERT_EQ(intent["type"], json("easeTo"));
    LOGOS_ASSERT_EQ(intent["center"], json::parse("[5.0,5.0]"));
    LOGOS_ASSERT_EQ(intent["zoom"], json(14.0));

    // sourceId narrows; unknown source and empty geometry are unknownId.
    LOGOS_ASSERT_TRUE(impl.camera("v/1", R"({"type":"fitSession","session":"quests/main","sourceId":"pts"})").success);
    LOGOS_ASSERT_EQ(code(impl.camera("v/1", R"({"type":"fitSession","session":"quests/main","sourceId":"nope"})")), std::string("maps.unknownId"));
    impl.applyBatch("empty/e", json::array({op("putSource", {{"id", "s"}, {"type", "geojson"}})}).dump());
    LOGOS_ASSERT_EQ(code(impl.camera("v/1", R"({"type":"fitSession","session":"empty/e"})")), std::string("maps.unknownId"));
    LOGOS_ASSERT_EQ(code(impl.camera("v/1", R"({"type":"jumpTo","keepFollow":"yes"})")), std::string("maps.badArgument"));
}

LOGOS_TEST(impl_reports_chrome_view_topics) {
    logos_test::EventCapture ev;
    MapsCoreImpl impl;
    impl.openView("v/1", "t", "{}");
    for (const char* t : {"calloutOpened", "calloutAction", "calloutClosed", "pinDropped", "pinCleared", "followChanged"}) {
        LOGOS_ASSERT_TRUE(impl.reportViewEvent("v/1", t, R"({"x":1})"));
        LOGOS_ASSERT_EQ(last(ev, "viewEvent")["topic"], json(t));
    }
    StdLogosResult caps = impl.getCapabilities(std::nullopt);
    LOGOS_ASSERT_EQ(caps.value["scene"]["groups"], json(true));
    LOGOS_ASSERT_EQ(caps.value["scene"]["callouts"], json(true));
    LOGOS_ASSERT_EQ(caps.value["chrome"]["follow"], json(false));
    impl.openView("v/2", "t", R"({"chrome":{"items":["scaleBar","compass"],"follow":true,"pin":true}})");
    caps = impl.getCapabilities(std::string("v/2"));
    LOGOS_ASSERT_EQ(caps.value["chrome"]["items"], json::array({"scaleBar", "compass"}));
    LOGOS_ASSERT_EQ(caps.value["chrome"]["follow"], json(true));
}

// ── basemap descriptors ──────────────────────────────────────────────────

LOGOS_TEST(descriptor_thumbnail_and_details_validate_and_normalise) {
    json d = mt::readJsonFixture("basemaps/openfreemap-bright.json");
    d["thumbnail"] = "bright.png";
    d["label"] = "Bright";
    d["details"] = json::array({
        json{{"id", "buildings3d"}, {"title", "3D buildings"}, {"icon", "data:image/svg+xml;utf8,<svg/>"}, {"layerPrefixes", json::array({"building-3d"})}, {"defaultVisible", false}},
        json{{"id", "poi"}, {"title", "POI"}, {"layerPrefixes", json::array({"poi_", "poi-"})}},
    });
    LOGOS_ASSERT_TRUE(validateDescriptor(d).empty());
    const json n = normalizeDescriptor(d);
    LOGOS_ASSERT_EQ(n["thumbnail"], json("bright.png"));
    LOGOS_ASSERT_EQ(n["label"], json("Bright"));
    LOGOS_ASSERT_TRUE(n["details"][0]["icon"].is_string());
    LOGOS_ASSERT_EQ(n["details"][0]["defaultVisible"], json(false));
    LOGOS_ASSERT_EQ(n["details"][1]["defaultVisible"], json(true));
    // Absent -> empty array; null thumbnail dropped.
    json plain = mt::readJsonFixture("basemaps/openfreemap-bright.json");
    plain["thumbnail"] = nullptr;
    plain["label"] = nullptr;
    const json np = normalizeDescriptor(plain);
    LOGOS_ASSERT_EQ(np["details"], json::array());
    LOGOS_ASSERT_FALSE(np.contains("thumbnail"));
    LOGOS_ASSERT_FALSE(np.contains("label"));

    json bad = d;
    bad["thumbnail"] = 3;
    bad["label"] = 7;
    bad["details"] = json::array({
        json{{"id", "a"}, {"title", "A"}, {"layerPrefixes", json::array()}},
        json{{"id", "a"}, {"layerPrefixes", json::array({"x"})}, {"defaultVisible", "no"}, {"icon", 1}},
        "nope",
    });
    const auto errs = validateDescriptor(bad);
    std::vector<std::string> paths;
    for (const auto& e : errs) paths.push_back(e.path);
    const std::string joined = mt::join(paths);
    for (const char* p : {"/thumbnail", "/label", "/details/0/layerPrefixes", "/details/1/id", "/details/1/title", "/details/1/defaultVisible", "/details/1/icon", "/details/2"})
        LOGOS_ASSERT_TRUE(joined.find(p) != std::string::npos);

    // Round-trips through the catalogue and the impl.
    MapsCoreImpl impl;
    d["id"] = "test/detailed";
    StdLogosResult added = impl.addBasemap(d.dump());
    LOGOS_ASSERT_TRUE(added.success);
    StdLogosResult got = impl.getBasemap("test/detailed");
    LOGOS_ASSERT_EQ(got.value["descriptor"]["details"].size(), std::size_t(2));
    LOGOS_ASSERT_EQ(got.value["descriptor"]["thumbnail"], json("bright.png"));
}

LOGOS_TEST(capability_defaults_include_rev04_keys) {
    const json d = defaultCapabilities(json{{"maxImageBytes", 1}, {"maxBatchBytes", 2}, {"maxPickHits", 3}});
    LOGOS_ASSERT_EQ(getPath(d, "scene.groups"), json(true));
    LOGOS_ASSERT_EQ(getPath(d, "scene.callouts"), json(true));
    LOGOS_ASSERT_EQ(getPath(d, "chrome.items"), json::array());
    LOGOS_ASSERT_EQ(getPath(d, "chrome.follow"), json(false));
    LOGOS_ASSERT_EQ(getPath(d, "chrome.pin"), json(false));
}
