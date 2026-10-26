#include "engine/capability.h"

#include <algorithm>
#include <set>

namespace maps::engine {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
    }
    return out;
}

void flatten(const json& obj, const std::string& prefix, std::vector<std::pair<std::string, json>>& out) {
    if (!obj.is_object()) return;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) flatten(*it, key, out);
        else out.emplace_back(key, *it);
    }
}

}  // namespace

json getPath(const json& obj, const std::string& dotted) {
    const json* cur = &obj;
    for (const auto& part : split(dotted, '.')) {
        if (!cur->is_object() || !cur->contains(part)) return json();
        cur = &(*cur)[part];
    }
    return *cur;
}

void setPath(json& obj, const std::string& dotted, json value) {
    json* cur = &obj;
    const auto parts = split(dotted, '.');
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!cur->is_object()) *cur = json::object();
        cur = &(*cur)[parts[i]];
    }
    if (!cur->is_object()) *cur = json::object();
    (*cur)[parts.back()] = std::move(value);
}

int transportRank(const std::string& t) {
    if (t == "wayland-dmabuf") return 3;
    if (t == "wayland-shm") return 2;
    if (t == "inproc") return 1;
    return 0;  // "none" or unknown
}

json defaultCapabilities(const json& limits) {
    json c = json::object();
    setPath(c, "engine.version", "");
    setPath(c, "binding.version", "");
    setPath(c, "patchSet", json::array());
    for (const char* k : {"render.customLayers", "render.terrain", "render.fillExtrusion", "render.hillshade",
                          "scene.featureState", "scene.patchFeatures", "scene.imageMetadata",
                          "pick.renderedFeatures", "pick.sourceFeatures", "provider.rangeRequests", "offline.regions"})
        setPath(c, k, false);
    // patchFeatures is implemented core-side (M6.4): always available.
    setPath(c, "scene.patchFeatures", true);
    // Groups and callout declarations are accepted by core (M6.3a/b); views
    // report whether they render them through chrome.* (§8.4).
    setPath(c, "scene.groups", true);
    setPath(c, "scene.callouts", true);
    setPath(c, "chrome.items", json::array());
    setPath(c, "chrome.follow", false);
    setPath(c, "chrome.pin", false);
    setPath(c, "provider.schemes", json::array());
    setPath(c, "surface.transport", "none");
    setPath(c, "limits.maxImageSize", limits.value("maxImageBytes", 0));
    setPath(c, "limits.maxBatchBytes", limits.value("maxBatchBytes", 0));
    setPath(c, "limits.maxPickHits", limits.value("maxPickHits", 0));
    return c;
}

json aggregateCapabilities(const std::vector<json>& views, const json& coreDefaults) {
    if (views.empty()) return coreDefaults;
    // Start from the first view merged over defaults, then fold the rest.
    auto merged = [&](const json& v) {
        json out = coreDefaults;
        std::vector<std::pair<std::string, json>> flat;
        flatten(v, "", flat);
        for (const auto& [k, val] : flat) setPath(out, k, val);
        return out;
    };
    json acc = merged(views.front());
    for (std::size_t i = 1; i < views.size(); ++i) {
        const json v = merged(views[i]);
        std::vector<std::pair<std::string, json>> flat;
        flatten(acc, "", flat);
        for (const auto& [k, a] : flat) {
            const json b = getPath(v, k);
            json r;
            if (a.is_boolean()) r = a.get<bool>() && b.is_boolean() && b.get<bool>();
            else if (a.is_number() && b.is_number()) r = std::min(a.get<double>(), b.get<double>());
            else if (a.is_array() && b.is_array()) {
                r = json::array();
                for (const auto& x : a) if (std::find(b.begin(), b.end(), x) != b.end()) r.push_back(x);
            } else if (k == "surface.transport" && a.is_string()) {
                const std::string bt = b.is_string() ? b.get<std::string>() : "none";
                r = (transportRank(a.get<std::string>()) <= transportRank(bt)) ? a : json(bt);
            } else {
                r = (a == b) ? a : json();  // identity disagreement -> null
            }
            setPath(acc, k, r);
        }
    }
    // Number limits that came back as doubles should stay integral when they were.
    for (const char* k : {"limits.maxImageSize", "limits.maxBatchBytes", "limits.maxPickHits"}) {
        json v = getPath(acc, k);
        if (v.is_number()) setPath(acc, k, static_cast<std::int64_t>(v.get<double>()));
    }
    return acc;
}

}  // namespace maps::engine
