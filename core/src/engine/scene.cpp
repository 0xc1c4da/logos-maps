#include "engine/scene.h"

#include <cctype>
#include <stdexcept>

namespace maps::engine {

// ── ids ──────────────────────────────────────────────────────────────────

std::string globalId(const std::string& session, const std::string& local) {
    return session + ":" + local;
}

std::string sessionOf(const std::string& gid) {
    const auto p = gid.find(':');
    return p == std::string::npos ? std::string() : gid.substr(0, p);
}

static bool isIdChar(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '.' || c == '-';
}

bool isValidSessionName(const std::string& s) {
    const auto slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) return false;
    if (s.find('/', slash + 1) != std::string::npos) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == slash) continue;
        if (!isIdChar(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool isValidLocalId(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c == ':' || std::isspace(c)) return false;
    }
    return true;
}

// ── Placement ────────────────────────────────────────────────────────────

json Placement::toJson() const {
    switch (kind) {
    case Kind::Top:    return json{{"top", true}};
    case Kind::Anchor: return json{{"anchor", ref}};
    case Kind::Before: return json{{"before", ref}};
    case Kind::Above:  return json{{"above", ref}};
    }
    return json{{"top", true}};
}

Placement Placement::fromJson(const json& j, const std::string& session) {
    Placement p;
    if (j.is_null()) return p;
    if (!j.is_object()) throw std::invalid_argument("placement must be an object");
    int set = 0;
    if (j.contains("top")) { ++set; if (j["top"] != true) throw std::invalid_argument("placement.top must be true"); }
    if (j.contains("anchor")) {
        ++set;
        if (!j["anchor"].is_string() || j["anchor"].get<std::string>().empty())
            throw std::invalid_argument("placement.anchor must be a non-empty string");
        p.kind = Kind::Anchor; p.ref = j["anchor"].get<std::string>();
    }
    for (const char* key : {"before", "above"}) {
        if (!j.contains(key)) continue;
        ++set;
        if (!j[key].is_string() || j[key].get<std::string>().empty())
            throw std::invalid_argument(std::string("placement.") + key + " must be a non-empty string");
        std::string ref = j[key].get<std::string>();
        // A bare local id refers to a layer of the same session; a global id
        // (contains ':') may name any session's layer. Basemap layer ids never
        // contain ':' and are only reachable through anchors (M6.10).
        if (ref.find(':') == std::string::npos) ref = globalId(session, ref);
        p.kind = (std::string(key) == "before") ? Kind::Before : Kind::Above;
        p.ref = ref;
    }
    if (set != 1) throw std::invalid_argument("placement must have exactly one of top|anchor|before|above");
    return p;
}

// ── toJson ───────────────────────────────────────────────────────────────

json Source::toJson() const {
    json j = {{"id", id}, {"type", type}, {"def", def}};
    if (!data.is_null()) { j["data"] = data; j["dataDigest"] = dataDigest; }
    return j;
}

json Layer::toJson() const {
    json j = {{"id", id}, {"type", type}, {"placement", placement.toJson()}};
    if (!source.empty()) j["source"] = source;
    if (sourceLayer) j["sourceLayer"] = *sourceLayer;
    if (minzoom) j["minzoom"] = *minzoom;
    if (maxzoom) j["maxzoom"] = *maxzoom;
    if (!filter.is_null()) j["filter"] = filter;
    j["layout"] = layout.is_null() ? json::object() : layout;
    j["paint"] = paint.is_null() ? json::object() : paint;
    return j;
}

json Image::toJson() const {
    json j = {{"id", id}, {"pixelRatio", pixelRatio}, {"sdf", sdf}, {"bytes", bytes}};
    if (!stretchX.is_null()) j["stretchX"] = stretchX;
    if (!stretchY.is_null()) j["stretchY"] = stretchY;
    if (!content.is_null()) j["content"] = content;
    return j;
}

Layer* Session::findLayer(const std::string& id) {
    for (auto& l : layers) if (l.id == id) return &l;
    return nullptr;
}
const Layer* Session::findLayer(const std::string& id) const {
    for (const auto& l : layers) if (l.id == id) return &l;
    return nullptr;
}

json Session::toJson() const {
    json srcs = json::object(), imgs = json::object(), lyrs = json::array();
    for (const auto& [id, s] : sources) srcs[id] = s.toJson();
    for (const auto& [id, i] : images) imgs[id] = i.toJson();
    for (const auto& l : layers) lyrs.push_back(l.toJson());
    return json{{"name", name}, {"owner", owner}, {"priority", priority},
                {"sources", srcs}, {"images", imgs}, {"layers", lyrs}};
}

Session* Scene::findSession(const std::string& name) {
    auto it = sessions.find(name);
    return it == sessions.end() ? nullptr : &it->second;
}
const Session* Scene::findSession(const std::string& name) const {
    auto it = sessions.find(name);
    return it == sessions.end() ? nullptr : &it->second;
}

const Source* Scene::findSource(const std::string& gid) const {
    const Session* s = findSession(sessionOf(gid));
    if (!s) return nullptr;
    auto it = s->sources.find(gid);
    return it == s->sources.end() ? nullptr : &it->second;
}
const Layer* Scene::findLayer(const std::string& gid) const {
    const Session* s = findSession(sessionOf(gid));
    return s ? s->findLayer(gid) : nullptr;
}
const Image* Scene::findImage(const std::string& gid) const {
    const Session* s = findSession(sessionOf(gid));
    if (!s) return nullptr;
    auto it = s->images.find(gid);
    return it == s->images.end() ? nullptr : &it->second;
}

json Scene::toJson() const {
    json ss = json::object();
    for (const auto& [name, s] : sessions) ss[name] = s.toJson();
    return json{{"v", 1}, {"sessions", ss}};
}

}  // namespace maps::engine
