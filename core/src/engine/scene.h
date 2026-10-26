// The retained scene (docs/architecture.md §6.2–§6.3).
//
// A Scene is the union over sessions of sources, layers and images. Every id
// is global: "<session>:<local id>" (M6.1), where a session is
// "<module>/<name>". The engine stores JSON for all style-level payloads
// (definitions, paint/layout, filters, GeoJSON data) because the engine does
// not interpret them -- the view hands them to MapLibre -- but it validates
// their shape at the batch boundary (batch.h).
#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace maps::engine {

using json = nlohmann::json;

// Where a layer sits relative to the basemap and to other overlay layers
// (M6.10). Exactly one of the four kinds.
struct Placement {
    enum class Kind { Top, Anchor, Before, Above };
    Kind kind = Kind::Top;
    std::string ref;  // anchor name | layer id (global) for Before/Above

    static Placement top() { return {}; }
    json toJson() const;
    // Parses {"top":true} | {"anchor":"labels"} | {"before":"id"} | {"above":"id"}.
    // Absent/null -> top. Throws std::invalid_argument on anything else.
    static Placement fromJson(const json& j, const std::string& session);
    bool operator==(const Placement& o) const { return kind == o.kind && ref == o.ref; }
};

struct Source {
    std::string id;    // global
    std::string type;  // geojson | vector | raster | raster-dem | image
    json def;          // type-specific definition without `data` (M6.3)
    json data;         // geojson only: the FeatureCollection; null otherwise
    std::string dataDigest;  // digest(data) when data is set (M6.6)
    json toJson() const;
};

struct Layer {
    std::string id;      // global
    std::string type;    // fill | line | symbol | circle | fill-extrusion | raster | hillshade | heatmap | background
    std::string source;  // global source id ("" for background)
    std::optional<std::string> sourceLayer;
    std::optional<double> minzoom, maxzoom;
    json filter;   // null = none
    json layout;   // object
    json paint;    // object
    Placement placement;
    std::optional<std::string> group;  // global group id in the same session (M6.3a)
    json callout;                      // null or {title, subtitle?, icon?, actions?[]} (M6.3b)
    std::optional<bool> selectable;    // M6.3b; absent = (callout present)
    json toJson() const;
};

// A user-toggleable set of layers within one session (M6.3a). Visibility is
// per view and lives in the view's options, not here.
struct Group {
    std::string id;  // global
    std::string title;
    std::optional<std::string> icon, description;
    bool defaultVisible = true;
    bool toggleable = true;
    int order = 0;
    json legend;  // null or [{label, color? | image?}]
    json toJson() const;
};

struct Image {
    std::string id;  // global
    double pixelRatio = 1.0;
    bool sdf = false;
    json stretchX, stretchY, content;  // null or arrays as in the style spec
    std::string bytes;                 // base64 PNG
    json toJson() const;
};

struct Session {
    std::string name;      // "<module>/<name>"
    std::string owner;     // module that created it ("" = unowned/headless)
    int priority = 0;      // host-assigned; lower draws first (M6.10)
    std::map<std::string, Source> sources;
    std::map<std::string, Image> images;
    std::map<std::string, Group> groups;
    std::vector<Layer> layers;  // insertion order = batch order (M6.10)

    Layer* findLayer(const std::string& id);
    const Layer* findLayer(const std::string& id) const;
    const Group* findGroup(const std::string& id) const;
    json toJson() const;
};

// Geographic extent [[w,s],[e,n]] of every coordinate in the session's
// GeoJSON sources (or one source), for fitSession (M7.4a). nullopt when
// there is no geometry.
struct Extent { double w, s, e, n; };
std::optional<Extent> sessionExtent(const Session& s, const std::optional<std::string>& sourceGlobalId = std::nullopt);

struct Scene {
    std::map<std::string, Session> sessions;

    Session* findSession(const std::string& name);
    const Session* findSession(const std::string& name) const;
    // Looks a global id up in the session its prefix names.
    const Source* findSource(const std::string& globalId) const;
    const Layer* findLayer(const std::string& globalId) const;
    const Image* findImage(const std::string& globalId) const;

    json toJson() const;  // {"v":1,"sessions":{...}}
};

// "<session>:<local>" helpers.
std::string globalId(const std::string& session, const std::string& local);
// Session prefix of a global id, or "" if it has none.
std::string sessionOf(const std::string& gid);
// A session name is "<module>/<name>" with [A-Za-z0-9_.-] on each side.
bool isValidSessionName(const std::string& s);
// A local id is non-empty, has no ':' and no whitespace.
bool isValidLocalId(const std::string& s);

}  // namespace maps::engine
