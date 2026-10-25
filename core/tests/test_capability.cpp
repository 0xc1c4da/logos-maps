// Capability aggregation (M11.1).
#include <logos_test.h>

#include "engine/capability.h"

using nlohmann::json;
using namespace maps::engine;

LOGOS_TEST(capability_defaults_cover_every_m11_1_key) {
    const json d = defaultCapabilities(json{{"maxImageBytes", 1048576}, {"maxBatchBytes", 4194304}, {"maxPickHits", 64}});
    for (const char* k : {"engine.version", "binding.version", "patchSet", "render.customLayers", "render.terrain",
                          "render.fillExtrusion", "render.hillshade", "scene.featureState", "scene.patchFeatures",
                          "scene.imageMetadata", "pick.renderedFeatures", "pick.sourceFeatures", "provider.schemes",
                          "provider.rangeRequests", "offline.regions", "surface.transport", "limits.maxImageSize",
                          "limits.maxBatchBytes", "limits.maxPickHits"})
        LOGOS_ASSERT_FALSE(getPath(d, k).is_null());
    LOGOS_ASSERT_EQ(getPath(d, "scene.patchFeatures"), json(true));  // core-side, always on
    LOGOS_ASSERT_EQ(getPath(d, "surface.transport"), json("none"));
    LOGOS_ASSERT_EQ(getPath(d, "limits.maxBatchBytes"), json(4194304));
}

LOGOS_TEST(capability_aggregate_ands_booleans_mins_limits_intersects_arrays) {
    const json defaults = defaultCapabilities(json{{"maxImageBytes", 100}, {"maxBatchBytes", 100}, {"maxPickHits", 10}});
    json a = json::parse(R"({"engine":{"version":"1.2"},"render":{"terrain":true,"customLayers":true},
        "pick":{"renderedFeatures":true},"provider":{"schemes":["https","pmtiles","logos"],"rangeRequests":true},
        "surface":{"transport":"wayland-dmabuf"},"limits":{"maxPickHits":50},"patchSet":["p1","p4"]})");
    json b = json::parse(R"({"engine":{"version":"1.2"},"render":{"terrain":false,"customLayers":true},
        "pick":{"renderedFeatures":true},"provider":{"schemes":["https","logos"],"rangeRequests":true},
        "surface":{"transport":"wayland-shm"},"limits":{"maxPickHits":20},"patchSet":["p1"]})");
    const json agg = aggregateCapabilities({a, b}, defaults);
    LOGOS_ASSERT_EQ(getPath(agg, "engine.version"), json("1.2"));
    LOGOS_ASSERT_EQ(getPath(agg, "render.terrain"), json(false));
    LOGOS_ASSERT_EQ(getPath(agg, "render.customLayers"), json(true));
    LOGOS_ASSERT_EQ(getPath(agg, "render.hillshade"), json(false));  // default false AND default false
    LOGOS_ASSERT_EQ(getPath(agg, "pick.renderedFeatures"), json(true));
    LOGOS_ASSERT_EQ(getPath(agg, "provider.schemes"), json::array({"https", "logos"}));
    LOGOS_ASSERT_EQ(getPath(agg, "surface.transport"), json("wayland-shm"));
    LOGOS_ASSERT_EQ(getPath(agg, "limits.maxPickHits"), json(20));
    LOGOS_ASSERT_EQ(getPath(agg, "limits.maxBatchBytes"), json(100));
    LOGOS_ASSERT_EQ(getPath(agg, "patchSet"), json::array({"p1"}));

    // Identity disagreement -> null; single view -> its own values over defaults.
    json c = b; c["engine"]["version"] = "1.3";
    LOGOS_ASSERT_TRUE(getPath(aggregateCapabilities({a, c}, defaults), "engine.version").is_null());
    LOGOS_ASSERT_EQ(getPath(aggregateCapabilities({a}, defaults), "surface.transport"), json("wayland-dmabuf"));
    LOGOS_ASSERT_EQ(aggregateCapabilities({}, defaults), defaults);
}

LOGOS_TEST(capability_transport_rank_is_total) {
    LOGOS_ASSERT_LT(transportRank("none"), transportRank("inproc"));
    LOGOS_ASSERT_LT(transportRank("inproc"), transportRank("wayland-shm"));
    LOGOS_ASSERT_LT(transportRank("wayland-shm"), transportRank("wayland-dmabuf"));
    LOGOS_ASSERT_EQ(transportRank("bogus"), transportRank("none"));
}

LOGOS_TEST(capability_path_helpers) {
    json o = json::object();
    setPath(o, "a.b.c", 1);
    setPath(o, "a.d", "x");
    LOGOS_ASSERT_EQ(o, json::parse(R"({"a":{"b":{"c":1},"d":"x"}})"));
    LOGOS_ASSERT_EQ(getPath(o, "a.b.c"), json(1));
    LOGOS_ASSERT_TRUE(getPath(o, "a.b.z").is_null());
    LOGOS_ASSERT_TRUE(getPath(o, "a.d.q").is_null());
}
