// Web Mercator camera maths for maps_core.toScreen / toGeo (§12.1).
//
// The core answers these from the view's last published camera and size so a
// client can position chrome without a round trip to the view process. It
// is the standard 512-px tile Mercator with centre/zoom/bearing; pitch is
// ignored (the view's own picking path is authoritative when it matters).
#pragma once

#include <nlohmann/json.hpp>

#include <optional>

namespace maps::engine {

using json = nlohmann::json;

struct Camera {
    double lon = 0, lat = 0;  // centre
    double zoom = 0;
    double bearing = 0;       // degrees, clockwise from north
    double pitch = 0;         // degrees (ignored by the maths below)
    double width = 0, height = 0;  // logical pixels

    // From {"center":[lon,lat],"zoom":z,"bearing":b,"pitch":p,"size":[w,h]}.
    static std::optional<Camera> fromJson(const json& state);
    json toJson() const;
};

struct Point { double x, y; };
struct LonLat { double lon, lat; };

constexpr double kTileSize = 512.0;
constexpr double kMaxLatitude = 85.05112878;

double worldSize(double zoom);
Point projectToWorld(LonLat ll, double zoom);   // absolute world pixels
LonLat unprojectFromWorld(Point p, double zoom);

Point toScreen(const Camera& cam, LonLat ll);
LonLat toGeo(const Camera& cam, Point screen);

// Metres per logical pixel at the camera centre latitude (scale bar, M6.11).
double metersPerPixel(const Camera& cam);

// Camera that fits [[west,south],[east,north]] with `padding` px on every side.
Camera fitBounds(const Camera& base, LonLat sw, LonLat ne, double padding);

}  // namespace maps::engine
