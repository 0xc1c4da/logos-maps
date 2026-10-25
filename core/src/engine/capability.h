// Capability aggregation (docs/architecture.md §11, M11.1).
//
// Each view reports a flat capabilities object when it opens. `aggregate`
// folds a set of them into the "what every current view can do" answer that
// getCapabilities() returns without a viewId: booleans AND, numeric limits
// MIN, string arrays INTERSECT, `surface.transport` becomes the weakest
// transport present, identity keys are kept only when all views agree.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace maps::engine {

using json = nlohmann::json;

// Keys are dotted ("render.terrain"); values are nested in the JSON
// ({"render":{"terrain":true}}). These helpers read/write by dotted path.
json getPath(const json& obj, const std::string& dotted);
void setPath(json& obj, const std::string& dotted, json value);

// The core-side defaults every view starts from; a view's report overrides
// per key. Keeps unknown keys (M12.1).
json defaultCapabilities(const json& limits);

json aggregateCapabilities(const std::vector<json>& views, const json& coreDefaults);

// Ordered weakest->strongest: none < inproc < wayland-shm < wayland-dmabuf.
int transportRank(const std::string& transport);

}  // namespace maps::engine
