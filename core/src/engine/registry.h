// View and provider registries (docs/architecture.md §12.1 views, §12.5
// tileProvider). Pure bookkeeping; the IPC glue lives in maps_core_impl.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace maps::engine {

using json = nlohmann::json;

struct ViewRecord {
    std::string viewId;
    std::string surfaceToken;
    json capabilities = json::object();
    json state = json::object();          // camera, basemap, size, dpr, gestureState (last published)
    std::string basemap;                  // descriptor id, "" until setBasemap
    std::uint64_t appliedRevision = 0;    // last acknowledged revision (M6.7)
    std::uint64_t renderedRevision = 0;   // last frameRendered revision
    json toJson() const;
};

class Views {
public:
    // Returns false if viewId already registered.
    bool open(const std::string& viewId, const std::string& surfaceToken, const json& capabilities);
    bool close(const std::string& viewId);
    ViewRecord* find(const std::string& viewId);
    const ViewRecord* find(const std::string& viewId) const;
    std::vector<std::string> ids() const;
    std::vector<json> capabilities() const;
    json list() const;  // {"v":1,"views":[...]}

    // Revision tracking: returns true when the ack moves the view forward.
    bool ack(const std::string& viewId, std::uint64_t revision);
    bool rendered(const std::string& viewId, std::uint64_t revision);
    // Lowest appliedRevision over all views (0 if none) -- what every view has applied.
    std::uint64_t minApplied() const;

    std::size_t size() const { return m_views.size(); }

private:
    std::map<std::string, ViewRecord> m_views;
};

struct ProviderRecord {
    std::string session;  // registering session (dropped with it)
    std::string scheme;   // "logos" (others reserved)
    std::string module;   // module implementing the tileProvider contract
    std::string prefix;   // path prefix under logos://<module>/<prefix>
    json spec;            // raw spec as registered
    json toJson() const;
};

class Providers {
public:
    // spec: {"module": "<name>", "prefix": "<path prefix>"}; validates and
    // rejects duplicate (scheme, module, prefix). Returns error message or "".
    std::string registerProvider(const std::string& session, const std::string& scheme, const json& spec);
    bool unregisterProvider(const std::string& session, const std::string& scheme);
    std::size_t dropSession(const std::string& session);  // count removed

    // Longest-prefix match for a logos:// URL path "<module>/<path>".
    const ProviderRecord* resolve(const std::string& scheme, const std::string& module, const std::string& path) const;
    json list() const;  // {"v":1,"providers":[...]}
    std::vector<std::string> schemes() const;  // unique, sorted
    std::size_t size() const { return m_providers.size(); }

private:
    std::vector<ProviderRecord> m_providers;
};

}  // namespace maps::engine
