#include "engine/projection.h"

#include <algorithm>
#include <cmath>

namespace maps::engine {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadius = 6378137.0;
double deg2rad(double d) { return d * kPi / 180.0; }
double rad2deg(double r) { return r * 180.0 / kPi; }
double clampLat(double lat) { return std::max(-kMaxLatitude, std::min(kMaxLatitude, lat)); }
double wrapLon(double lon) {
    while (lon > 180) lon -= 360;
    while (lon < -180) lon += 360;
    return lon;
}
}  // namespace

std::optional<Camera> Camera::fromJson(const json& s) {
    if (!s.is_object()) return std::nullopt;
    Camera c;
    const json center = s.value("center", json());
    if (!center.is_array() || center.size() != 2 || !center[0].is_number() || !center[1].is_number()) return std::nullopt;
    c.lon = center[0].get<double>();
    c.lat = center[1].get<double>();
    c.zoom = s.value("zoom", 0.0);
    c.bearing = s.value("bearing", 0.0);
    c.pitch = s.value("pitch", 0.0);
    const json size = s.value("size", json());
    if (size.is_array() && size.size() == 2) { c.width = size[0].get<double>(); c.height = size[1].get<double>(); }
    return c;
}

json Camera::toJson() const {
    return json{{"center", {lon, lat}}, {"zoom", zoom}, {"bearing", bearing}, {"pitch", pitch}, {"size", {width, height}}};
}

double worldSize(double zoom) { return kTileSize * std::pow(2.0, zoom); }

Point projectToWorld(LonLat ll, double zoom) {
    const double ws = worldSize(zoom);
    const double lat = deg2rad(clampLat(ll.lat));
    const double x = (ll.lon + 180.0) / 360.0 * ws;
    const double y = (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / kPi) / 2.0 * ws;
    return {x, y};
}

LonLat unprojectFromWorld(Point p, double zoom) {
    const double ws = worldSize(zoom);
    const double lon = p.x / ws * 360.0 - 180.0;
    const double n = kPi - 2.0 * kPi * p.y / ws;
    const double lat = rad2deg(std::atan(0.5 * (std::exp(n) - std::exp(-n))));
    return {wrapLon(lon), lat};
}

Point toScreen(const Camera& cam, LonLat ll) {
    const Point c = projectToWorld({cam.lon, cam.lat}, cam.zoom);
    const Point p = projectToWorld(ll, cam.zoom);
    const double dx = p.x - c.x, dy = p.y - c.y;
    const double b = deg2rad(-cam.bearing);  // rotating the world by -bearing turns the map clockwise by bearing
    const double rx = dx * std::cos(b) - dy * std::sin(b);
    const double ry = dx * std::sin(b) + dy * std::cos(b);
    return {cam.width / 2.0 + rx, cam.height / 2.0 + ry};
}

LonLat toGeo(const Camera& cam, Point screen) {
    const Point c = projectToWorld({cam.lon, cam.lat}, cam.zoom);
    const double rx = screen.x - cam.width / 2.0, ry = screen.y - cam.height / 2.0;
    const double b = deg2rad(cam.bearing);
    const double dx = rx * std::cos(b) - ry * std::sin(b);
    const double dy = rx * std::sin(b) + ry * std::cos(b);
    return unprojectFromWorld({c.x + dx, c.y + dy}, cam.zoom);
}

double metersPerPixel(const Camera& cam) {
    return 2.0 * kPi * kEarthRadius * std::cos(deg2rad(clampLat(cam.lat))) / worldSize(cam.zoom);
}

Camera fitBounds(const Camera& base, LonLat sw, LonLat ne, double padding) {
    Camera out = base;
    out.bearing = 0;
    out.pitch = 0;
    const double w = std::max(1.0, base.width - 2 * padding);
    const double h = std::max(1.0, base.height - 2 * padding);
    // Work at zoom 0 and scale.
    const Point a = projectToWorld(sw, 0), b = projectToWorld(ne, 0);
    const double dx = std::max(1e-9, std::fabs(b.x - a.x));
    const double dy = std::max(1e-9, std::fabs(b.y - a.y));
    const double zx = std::log2(w / dx), zy = std::log2(h / dy);
    out.zoom = std::max(0.0, std::min(zx, zy));
    const LonLat centre = unprojectFromWorld({(a.x + b.x) / 2.0, (a.y + b.y) / 2.0}, 0);
    out.lon = centre.lon;
    out.lat = centre.lat;
    return out;
}

}  // namespace maps::engine
