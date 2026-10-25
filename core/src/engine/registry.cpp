#include "engine/registry.h"

#include <algorithm>
#include <set>

namespace maps::engine {

json ViewRecord::toJson() const {
    return json{{"viewId", viewId}, {"surfaceToken", surfaceToken}, {"capabilities", capabilities},
                {"state", state}, {"basemap", basemap}, {"appliedRevision", appliedRevision},
                {"renderedRevision", renderedRevision}};
}

bool Views::open(const std::string& viewId, const std::string& surfaceToken, const json& capabilities) {
    if (viewId.empty() || m_views.count(viewId)) return false;
    ViewRecord r;
    r.viewId = viewId;
    r.surfaceToken = surfaceToken;
    r.capabilities = capabilities.is_object() ? capabilities : json::object();
    m_views[viewId] = std::move(r);
    return true;
}

bool Views::close(const std::string& viewId) { return m_views.erase(viewId) > 0; }

ViewRecord* Views::find(const std::string& viewId) {
    auto it = m_views.find(viewId);
    return it == m_views.end() ? nullptr : &it->second;
}
const ViewRecord* Views::find(const std::string& viewId) const {
    auto it = m_views.find(viewId);
    return it == m_views.end() ? nullptr : &it->second;
}

std::vector<std::string> Views::ids() const {
    std::vector<std::string> out;
    for (const auto& [id, _] : m_views) out.push_back(id);
    return out;
}

std::vector<json> Views::capabilities() const {
    std::vector<json> out;
    for (const auto& [_, v] : m_views) out.push_back(v.capabilities);
    return out;
}

json Views::list() const {
    json arr = json::array();
    for (const auto& [_, v] : m_views) arr.push_back(v.toJson());
    return json{{"v", 1}, {"views", arr}};
}

bool Views::ack(const std::string& viewId, std::uint64_t revision) {
    ViewRecord* v = find(viewId);
    if (!v || revision <= v->appliedRevision) return false;
    v->appliedRevision = revision;
    return true;
}

bool Views::rendered(const std::string& viewId, std::uint64_t revision) {
    ViewRecord* v = find(viewId);
    if (!v || revision <= v->renderedRevision) return false;
    v->renderedRevision = revision;
    return true;
}

std::uint64_t Views::minApplied() const {
    if (m_views.empty()) return 0;
    std::uint64_t m = UINT64_MAX;
    for (const auto& [_, v] : m_views) m = std::min(m, v.appliedRevision);
    return m;
}

// ── providers ────────────────────────────────────────────────────────────

json ProviderRecord::toJson() const {
    return json{{"session", session}, {"scheme", scheme}, {"module", module}, {"prefix", prefix}, {"spec", spec}};
}

std::string Providers::registerProvider(const std::string& session, const std::string& scheme, const json& spec) {
    if (session.empty()) return "session is required";
    if (scheme != "logos") return "unsupported scheme '" + scheme + "' (only logos)";
    if (!spec.is_object()) return "spec must be an object";
    if (!spec.contains("module") || !spec["module"].is_string() || spec["module"].get<std::string>().empty())
        return "spec.module is required";
    ProviderRecord r;
    r.session = session;
    r.scheme = scheme;
    r.module = spec["module"].get<std::string>();
    r.prefix = spec.value("prefix", "");
    r.spec = spec;
    if (r.module.find('/') != std::string::npos) return "spec.module must not contain '/'";
    while (!r.prefix.empty() && r.prefix.front() == '/') r.prefix.erase(0, 1);
    for (const auto& p : m_providers)
        if (p.scheme == r.scheme && p.module == r.module && p.prefix == r.prefix)
            return "provider already registered for logos://" + r.module + "/" + r.prefix;
    m_providers.push_back(std::move(r));
    return "";
}

bool Providers::unregisterProvider(const std::string& session, const std::string& scheme) {
    const auto before = m_providers.size();
    m_providers.erase(std::remove_if(m_providers.begin(), m_providers.end(),
                                     [&](const ProviderRecord& p) { return p.session == session && p.scheme == scheme; }),
                      m_providers.end());
    return m_providers.size() != before;
}

std::size_t Providers::dropSession(const std::string& session) {
    const auto before = m_providers.size();
    m_providers.erase(std::remove_if(m_providers.begin(), m_providers.end(),
                                     [&](const ProviderRecord& p) { return p.session == session; }),
                      m_providers.end());
    return before - m_providers.size();
}

const ProviderRecord* Providers::resolve(const std::string& scheme, const std::string& module, const std::string& path) const {
    const ProviderRecord* best = nullptr;
    for (const auto& p : m_providers) {
        if (p.scheme != scheme || p.module != module) continue;
        if (path.rfind(p.prefix, 0) != 0) continue;
        if (!best || p.prefix.size() > best->prefix.size()) best = &p;
    }
    return best;
}

json Providers::list() const {
    json arr = json::array();
    for (const auto& p : m_providers) arr.push_back(p.toJson());
    return json{{"v", 1}, {"providers", arr}};
}

std::vector<std::string> Providers::schemes() const {
    std::set<std::string> s;
    for (const auto& p : m_providers) s.insert(p.scheme);
    return {s.begin(), s.end()};
}

}  // namespace maps::engine
