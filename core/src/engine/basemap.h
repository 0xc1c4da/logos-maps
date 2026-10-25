// Basemap descriptors and catalogue (docs/architecture.md §9.1, Appendix C).
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace maps::engine {

using json = nlohmann::json;

struct DescriptorError {
    std::string path;     // JSON pointer-ish, e.g. "/anchors/labels"
    std::string message;
};

// Validates against Appendix C. Returns the list of problems (empty = valid).
// Unknown top-level keys are errors (schema: additionalProperties false).
std::vector<DescriptorError> validateDescriptor(const json& descriptor);

// Fills defaults (v=1, tile_size=512 for raster, profiles=["online"],
// lang="local", lang_key="name:{lang}", zoom=[0,20], patch=[]) and derives
// `fingerprint` when absent (sha256 over the canonical descriptor minus
// fingerprint). Call after validateDescriptor.
json normalizeDescriptor(json descriptor);

// M9.2: expand a raster descriptor into a full MapLibre style with one raster
// source ("basemap") and one raster layer ("basemap"). Catalogue::add sets the
// descriptor's standard anchors to "" (= top, see anchors.h) so session layers
// always land above the raster.
json rasterStyle(const json& descriptor);

// Simple filter: every key in `filter` must match the descriptor (arrays:
// membership of every requested value; scalars: equality). {} matches all.
bool descriptorMatches(const json& descriptor, const json& filter);

class Catalogue {
public:
    // Returns false (with error) when invalid; replaces an existing id.
    bool add(const json& descriptor, std::string* error = nullptr);
    bool remove(const std::string& id);
    const json* find(const std::string& id) const;
    json list(const json& filter = json::object()) const;  // {"v":1,"basemaps":[...]}
    std::size_t size() const { return m_byId.size(); }

private:
    std::map<std::string, json> m_byId;
};

}  // namespace maps::engine
