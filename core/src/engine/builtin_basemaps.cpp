#include "engine/builtin_basemaps.h"

namespace maps::engine {

using json = nlohmann::json;

namespace {

// Anchor layer ids per OpenFreeMap style (references/openfreemap-styles/
// styles/<name>/style.json): `labels` is the first symbol layer with a
// text-field (waterway_line_label in all three), `routes` the first road
// layer (the first tunnel casing). Keep in sync with the pinned styles.
const char* kOpenFreeMapAttribution = R"({
  "OpenFreeMap": "https://openfreemap.org",
  "© OpenMapTiles": "https://openmaptiles.org",
  "© OpenStreetMap contributors": "https://www.openstreetmap.org/copyright"
})";

json ofm(const char* id, const char* name, const char* style, const char* light, const char* type,
         const char* labels, const char* routes) {
    return json{
        {"v", 1},
        {"id", id},
        {"name", name},
        {"format", "mapbox-gl"},
        {"style_url", style},
        {"provider", "OpenFreeMap"},
        {"attribution", json::parse(kOpenFreeMapAttribution)},
        {"profiles", json::array({"online"})},
        {"type", type},
        {"light", light},
        {"vehicle", json::array({"car", "bicycle", "foot", "transit"})},
        {"lang", "local"},
        {"lang_key", "name:{lang}"},
        {"keys", json::array()},
        {"logo", "openfreemap.svg"},
        {"anchors", {{"labels", labels}, {"routes", routes}}},
        {"zoom", json::array({0, 20})},
        {"patch", json::array()},
    };
}

}  // namespace

json builtinBasemaps() {
    json out = json::array();
    out.push_back(ofm("openfreemap/bright", "OpenFreeMap Bright", "https://tiles.openfreemap.org/styles/bright",
                      "day", "default", "waterway_line_label", "tunnel-service-track-casing"));
    out.push_back(ofm("openfreemap/liberty", "OpenFreeMap Liberty", "https://tiles.openfreemap.org/styles/liberty",
                      "day", "default", "waterway_line_label", "tunnel_motorway_link_casing"));
    out.push_back(ofm("openfreemap/positron", "OpenFreeMap Positron", "https://tiles.openfreemap.org/styles/positron",
                      "day", "clean", "waterway_line_label", "tunnel_motorway_casing"));
    out.push_back(json{
        {"v", 1},
        {"id", "osm/standard"},
        {"name", "OpenStreetMap Standard"},
        {"format", "raster"},
        {"tile_url", "https://tile.openstreetmap.org/{z}/{x}/{y}.png"},
        {"tile_size", 256},
        {"provider", "OpenStreetMap"},
        {"attribution", {{"© OpenStreetMap contributors", "https://www.openstreetmap.org/copyright"}}},
        {"profiles", json::array({"online"})},
        {"type", "default"},
        {"light", "day"},
        {"zoom", json::array({0, 19})},
    });
    return out;
}

const char* defaultBasemapId() { return "openfreemap/bright"; }

}  // namespace maps::engine
