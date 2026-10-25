#include "engine/anchors.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace maps::engine {

namespace {

struct Entry {
    const Layer* layer;
    const Session* session;
    std::size_t index;  // position within session
};

struct Resolution {
    std::string anchor;  // "top" or an anchor name
    bool resolved = false;
    bool visiting = false;
};

}  // namespace

json ResolvedOrder::toJson() const {
    json ls = json::array();
    for (const auto& l : layers) ls.push_back({{"id", l.layerId}, {"before", l.before}, {"anchor", l.anchor}});
    return json{{"v", 1}, {"layers", ls}, {"missingAnchors", missingAnchors}, {"danglingRefs", danglingRefs}};
}

ResolvedOrder resolveOrder(const Scene& scene, const json& anchors) {
    ResolvedOrder out;
    std::set<std::string> missing, dangling;

    // Collect all entries in a deterministic base order: priority, name, index.
    std::vector<const Session*> sessions;
    for (const auto& [name, s] : scene.sessions) sessions.push_back(&s);
    std::stable_sort(sessions.begin(), sessions.end(), [](const Session* a, const Session* b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        return a->name < b->name;
    });
    std::vector<Entry> entries;
    std::map<std::string, std::size_t> byId;
    for (const Session* s : sessions) {
        for (std::size_t i = 0; i < s->layers.size(); ++i) {
            byId[s->layers[i].id] = entries.size();
            entries.push_back(Entry{&s->layers[i], s, i});
        }
    }

    // Resolve each layer's anchor group (following before/above chains).
    std::vector<Resolution> res(entries.size());
    auto anchorKnown = [&](const std::string& name) {
        // An empty string is a known anchor that maps to "top" (raster basemaps, M9.2).
        return anchors.is_object() && anchors.contains(name) && anchors[name].is_string();
    };
    std::function<const std::string&(std::size_t)> groupOf = [&](std::size_t i) -> const std::string& {
        Resolution& r = res[i];
        if (r.resolved) return r.anchor;
        if (r.visiting) {  // cycle
            dangling.insert(entries[i].layer->id);
            r.anchor = "top"; r.resolved = true; return r.anchor;
        }
        r.visiting = true;
        const Placement& p = entries[i].layer->placement;
        switch (p.kind) {
        case Placement::Kind::Top:
            r.anchor = "top";
            break;
        case Placement::Kind::Anchor:
            if (anchorKnown(p.ref)) r.anchor = p.ref;
            else { missing.insert(p.ref); r.anchor = "top"; }
            break;
        case Placement::Kind::Before:
        case Placement::Kind::Above: {
            auto it = byId.find(p.ref);
            if (it == byId.end()) { dangling.insert(p.ref); r.anchor = "top"; }
            else r.anchor = groupOf(it->second);
            break;
        }
        }
        r.visiting = false;
        r.resolved = true;
        return r.anchor;
    };
    for (std::size_t i = 0; i < entries.size(); ++i) groupOf(i);

    // Within each group, order: base order, then splice before/above relative
    // to their target. We build the list by inserting non-relative layers in
    // base order, then relative ones (in base order) next to their targets;
    // relatives whose target is itself relative wait until the target is placed.
    std::map<std::string, std::vector<std::size_t>> groups;  // anchor -> ordered entry indices
    for (std::size_t i = 0; i < entries.size(); ++i) groups[res[i].anchor];
    std::vector<bool> placed(entries.size(), false);
    auto isRelative = [&](std::size_t i) {
        const auto k = entries[i].layer->placement.kind;
        if (k != Placement::Kind::Before && k != Placement::Kind::Above) return false;
        return byId.count(entries[i].layer->placement.ref) > 0;
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (isRelative(i)) continue;
        groups[res[i].anchor].push_back(i);
        placed[i] = true;
    }
    bool progress = true;
    while (progress) {
        progress = false;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (placed[i] || !isRelative(i)) continue;
            const std::size_t target = byId.at(entries[i].layer->placement.ref);
            if (!placed[target]) continue;
            auto& g = groups[res[i].anchor];
            auto pos = std::find(g.begin(), g.end(), target);
            if (pos == g.end()) { g.push_back(i); }
            else if (entries[i].layer->placement.kind == Placement::Kind::Before) g.insert(pos, i);
            else g.insert(pos + 1, i);
            placed[i] = true;
            progress = true;
        }
    }
    for (std::size_t i = 0; i < entries.size(); ++i)  // cycles: append to their group
        if (!placed[i]) groups[res[i].anchor].push_back(i);

    // Emit: anchored groups in descriptor order of anchors (so "routes" below
    // "labels" follows the basemap's own ordering), then "top" last.
    std::vector<std::string> anchorNames;
    if (anchors.is_object())
        for (auto it = anchors.begin(); it != anchors.end(); ++it) anchorNames.push_back(it.key());
    std::set<std::string> emitted;
    auto emit = [&](const std::string& a) {
        if (!emitted.insert(a).second) return;
        auto it = groups.find(a);
        if (it == groups.end()) return;
        const std::string before = (a == "top") ? std::string() : anchors[a].get<std::string>();
        for (std::size_t i : it->second) out.layers.push_back(ResolvedLayer{entries[i].layer->id, before, a});
    };
    for (const auto& a : anchorNames) emit(a);
    for (const auto& [a, _] : groups) if (a != "top") emit(a);
    emit("top");

    out.missingAnchors.assign(missing.begin(), missing.end());
    out.danglingRefs.assign(dangling.begin(), dangling.end());
    return out;
}

}  // namespace maps::engine
