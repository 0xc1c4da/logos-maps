// Web Mercator camera maths behind toScreen/toGeo, and the bundled catalogue.
#include <logos_test.h>

#include "engine/basemap.h"
#include "engine/builtin_basemaps.h"
#include "engine/projection.h"

#include <cmath>

using nlohmann::json;
using namespace maps::engine;

namespace {
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
Camera berlin() {
    Camera c;
    c.lon = 13.405; c.lat = 52.52; c.zoom = 12; c.width = 800; c.height = 600;
    return c;
}
}  // namespace

LOGOS_TEST(projection_round_trips_through_world_pixels) {
    for (double z : {0.0, 3.5, 12.0, 18.0}) {
        for (LonLat ll : {LonLat{0, 0}, LonLat{13.405, 52.52}, LonLat{-122.4, 37.8}, LonLat{179.9, -45}}) {
            LonLat back = unprojectFromWorld(projectToWorld(ll, z), z);
            LOGOS_ASSERT_TRUE(near(back.lon, ll.lon, 1e-7));
            LOGOS_ASSERT_TRUE(near(back.lat, ll.lat, 1e-7));
        }
    }
    const Point origin = projectToWorld({0, 0}, 0);
    LOGOS_ASSERT_TRUE(near(origin.x, 256) && near(origin.y, 256));
    LOGOS_ASSERT_TRUE(near(worldSize(1), 1024));
}

LOGOS_TEST(projection_screen_maps_centre_and_respects_bearing) {
    Camera cam = berlin();
    Point c = toScreen(cam, {cam.lon, cam.lat});
    LOGOS_ASSERT_TRUE(near(c.x, 400) && near(c.y, 300));
    // A point due east lands to the right with bearing 0...
    Point east = toScreen(cam, {cam.lon + 0.01, cam.lat});
    LOGOS_ASSERT_GT(east.x, 400.0);
    LOGOS_ASSERT_TRUE(near(east.y, 300, 1e-6));
    // ...and towards the top when the map is rotated 90° clockwise (bearing 90).
    cam.bearing = 90;
    Point eastRot = toScreen(cam, {cam.lon + 0.01, cam.lat});
    LOGOS_ASSERT_TRUE(near(eastRot.x, 400, 1e-6));
    LOGOS_ASSERT_LT(eastRot.y, 300.0);
    // toGeo inverts toScreen under rotation.
    LonLat back = toGeo(cam, eastRot);
    LOGOS_ASSERT_TRUE(near(back.lon, cam.lon + 0.01, 1e-7));
    LOGOS_ASSERT_TRUE(near(back.lat, cam.lat, 1e-7));
}

LOGOS_TEST(projection_meters_per_pixel_and_fit_bounds) {
    Camera cam = berlin();
    cam.zoom = 0; cam.lat = 0;
    LOGOS_ASSERT_TRUE(near(metersPerPixel(cam), 2 * 3.14159265358979323846 * 6378137.0 / 512.0, 1e-3));
    Camera fit = fitBounds(berlin(), {13.30, 52.45}, {13.50, 52.58}, 20);
    LOGOS_ASSERT_TRUE(near(fit.lon, 13.40, 1e-9));
    LOGOS_ASSERT_GT(fit.zoom, 9.0);
    LOGOS_ASSERT_LT(fit.zoom, 13.0);
    // Both corners end up inside the padded viewport.
    for (LonLat corner : {LonLat{13.30, 52.45}, LonLat{13.50, 52.58}}) {
        Point p = toScreen(fit, corner);
        LOGOS_ASSERT_GE(p.x, 20.0 - 1e-6);
        LOGOS_ASSERT_LE(p.x, 780.0 + 1e-6);
        LOGOS_ASSERT_GE(p.y, 20.0 - 1e-6);
        LOGOS_ASSERT_LE(p.y, 580.0 + 1e-6);
    }
}

LOGOS_TEST(projection_camera_json_round_trip) {
    const json j = json::parse(R"({"center":[13.405,52.52],"zoom":12,"bearing":30,"pitch":10,"size":[800,600]})");
    auto c = Camera::fromJson(j);
    LOGOS_ASSERT_TRUE(c.has_value());
    LOGOS_ASSERT_EQ(c->toJson(), j);
    LOGOS_ASSERT_FALSE(Camera::fromJson(json{{"zoom", 1}}).has_value());
    LOGOS_ASSERT_FALSE(Camera::fromJson(json("x")).has_value());
}

LOGOS_TEST(builtin_basemaps_are_valid_and_include_the_default) {
    Catalogue cat;
    std::string err;
    bool sawDefault = false;
    for (const auto& d : builtinBasemaps()) {
        LOGOS_ASSERT_TRUE(validateDescriptor(d).empty());
        LOGOS_ASSERT_TRUE(cat.add(d, &err));
        if (d["id"] == defaultBasemapId()) sawDefault = true;
    }
    LOGOS_ASSERT_TRUE(sawDefault);
    LOGOS_ASSERT_GE(cat.size(), std::size_t(3));
    LOGOS_ASSERT_EQ(cat.list(json{{"format", "raster"}})["basemaps"].size(), std::size_t(1));
}
