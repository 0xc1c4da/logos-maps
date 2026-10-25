#include "engine/store.h"

#include "engine/canon.h"

#include <algorithm>
#include <set>

namespace maps::engine {

namespace {

const std::set<std::string> kSourceTypes = {"geojson", "vector", "raster", "raster-dem", "image"};
const std::set<std::string> kLayerTypes = {"fill", "line", "symbol", "circle", "fill-extrusion",
                                           "raster", "hillshade", "heatmap", "background"};
const std::set<std::string> kOps = {"putSource", "replaceGeoJson", "patchFeatures", "putImage", "putLayer",
                                    "setPaint", "setLayout", "setFilter", "placeLayer", "removeLayer",
                                    "removeSource", "removeImage", "dropSession"};

struct OpError {
    std::string code;
    std::string message;
};

[[noreturn]] void fail(const std::string& code, const std::string& message) {
    throw OpError{code, message};
}

std::string requireString(const json& op, const char* key) {
    if (!op.contains(key) || !op[key].is_string() || op[key].get<std::string>().empty())
        fail("maps.badArgument", std::string("missing or empty string field '") + key + "'");
    return op[key].get<std::string>();
}

std::string requireLocalId(const json& op, const char* key) {
    const std::string v = requireString(op, key);
    if (!isValidLocalId(v)) fail("maps.badArgument", std::string("invalid id in '") + key + "': " + v);
    return v;
}

void requireObjectOrAbsent(const json& op, const char* key) {
    if (op.contains(key) && !op[key].is_null() && !op[key].is_object())
        fail("maps.badArgument", std::string("'") + key + "' must be an object");
}

bool isFeatureCollection(const json& d) {
    return d.is_object() && d.value("type", "") == "FeatureCollection" && d.contains("features") &&
           d["features"].is_array();
}

bool featureHasId(const json& f) {
    return f.is_object() && f.contains("id") && (f["id"].is_string() || f["id"].is_number());
}

std::string idKey(const json& id) {
    return id.is_string() ? "s:" + id.get<std::string>() : "n:" + canonical(id);
}

std::size_t base64DecodedSize(const std::string& b64) {
    std::size_t pad = 0;
    for (auto it = b64.rbegin(); it != b64.rend() && *it == '='; ++it) ++pad;
    return b64.size() * 3 / 4 - pad;
}

}  // namespace

// ── result ───────────────────────────────────────────────────────────────

json BatchResult::toJson() const {
    json rej = json::array();
    for (const auto& r : rejected) rej.push_back(r.toJson());
    return json{{"v", 1}, {"revision", revision}, {"published", published},
                {"accepted", accepted}, {"noop", noop}, {"rejected", rej}};
}

int opRank(const std::string& op) {
    if (op == "dropSession") return 0;
    if (op == "removeLayer") return 1;
    if (op == "removeSource") return 2;
    if (op == "removeImage") return 3;
    if (op == "putSource") return 4;
    if (op == "replaceGeoJson") return 5;
    if (op == "putImage") return 6;
    if (op == "putLayer") return 7;
    return 8;
}

// ── working copy ─────────────────────────────────────────────────────────

struct Store::Working {
    std::string sessionName;
    Session session;      // copy being mutated
};

Store::Store(Limits limits) : m_limits(limits) {}

void Store::validateAndApply(Working& w, std::size_t, const json& op) {
    if (!op.is_object()) fail("maps.badArgument", "op must be an object");
    const std::string kind = requireString(op, "op");
    if (!kOps.count(kind)) fail("maps.badArgument", "unknown op '" + kind + "'");
    Session& s = w.session;

    if (kind == "dropSession") {
        const auto name = s.name; const auto owner = s.owner; const auto prio = s.priority;
        s = Session{};
        s.name = name; s.owner = owner; s.priority = prio;
        return;
    }

    if (kind == "putSource") {
        const std::string local = requireLocalId(op, "id");
        const std::string type = requireString(op, "type");
        if (!kSourceTypes.count(type)) fail("maps.badArgument", "unknown source type '" + type + "'");
        requireObjectOrAbsent(op, "def");
        json def = op.value("def", json::object());
        if (def.is_null()) def = json::object();
        json data;
        if (def.contains("data")) {  // data travels in `data`, not `def` (M6.3)
            data = def["data"];
            def.erase("data");
        }
        if (op.contains("data")) data = op["data"];
        if (type == "geojson") {
            if (!data.is_null() && !isFeatureCollection(data))
                fail("maps.badArgument", "geojson source data must be a FeatureCollection");
            if (data.is_null()) data = json{{"type", "FeatureCollection"}, {"features", json::array()}};
        } else {
            if (!data.is_null()) fail("maps.badArgument", "only geojson sources carry data");
            if (type == "image") {
                if (!def.contains("url") || !def.contains("coordinates"))
                    fail("maps.badArgument", "image source needs def.url and def.coordinates");
            } else if (!def.contains("url") && !def.contains("tiles")) {
                fail("maps.badArgument", type + " source needs def.url or def.tiles");
            }
        }
        const std::string gid = globalId(w.sessionName, local);
        Source& src = s.sources[gid];
        const bool existed = !src.id.empty();
        // Replacing the definition keeps existing data unless new data is given.
        if (existed && type == "geojson" && !op.contains("data") && !op.value("def", json::object()).contains("data"))
            data = src.data;
        src.id = gid; src.type = type; src.def = def; src.data = data;
        src.dataDigest = data.is_null() ? std::string() : digest(data);
        return;
    }

    if (kind == "replaceGeoJson" || kind == "patchFeatures") {
        const std::string local = requireLocalId(op, "sourceId");
        const std::string gid = globalId(w.sessionName, local);
        auto it = s.sources.find(gid);
        if (it == s.sources.end()) fail("maps.unknownId", "unknown source '" + local + "'");
        Source& src = it->second;
        if (src.type != "geojson") fail("maps.badArgument", "source '" + local + "' is not geojson");
        if (kind == "replaceGeoJson") {
            if (!op.contains("data") || !isFeatureCollection(op["data"]))
                fail("maps.badArgument", "data must be a FeatureCollection");
            src.data = op["data"];
        } else {
            // M6.2: every feature in the collection must carry an id.
            for (const auto& f : src.data["features"])
                if (!featureHasId(f)) fail("maps.badArgument", "patchFeatures needs feature ids on '" + local + "'");
            const json upsert = op.value("upsert", json::array());
            const json remove = op.value("remove", json::array());
            if (!upsert.is_array() || !remove.is_array()) fail("maps.badArgument", "upsert/remove must be arrays");
            for (const auto& f : upsert)
                if (!featureHasId(f) || f.value("type", "") != "Feature")
                    fail("maps.badArgument", "upsert entries must be Features with ids");
            std::set<std::string> removeKeys;
            for (const auto& id : remove) {
                if (!id.is_string() && !id.is_number()) fail("maps.badArgument", "remove entries must be ids");
                removeKeys.insert(idKey(id));
            }
            std::map<std::string, std::size_t> indexById;
            json& features = src.data["features"];
            for (std::size_t i = 0; i < features.size(); ++i) indexById[idKey(features[i]["id"])] = i;
            for (const auto& f : upsert) {
                const auto k = idKey(f["id"]);
                auto found = indexById.find(k);
                if (found != indexById.end()) features[found->second] = f;
                else { indexById[k] = features.size(); features.push_back(f); }
            }
            if (!removeKeys.empty()) {
                json kept = json::array();
                for (const auto& f : features)
                    if (!removeKeys.count(idKey(f["id"]))) kept.push_back(f);
                features = std::move(kept);
            }
        }
        src.dataDigest = digest(src.data);
        return;
    }

    if (kind == "putImage") {
        const std::string local = requireLocalId(op, "id");
        const std::string bytes = requireString(op, "bytes");
        if (base64DecodedSize(bytes) > m_limits.maxImageBytes) fail("maps.badArgument", "image exceeds limits.maxImageSize");
        Image img;
        img.id = globalId(w.sessionName, local);
        img.bytes = bytes;
        img.pixelRatio = op.value("pixelRatio", 1.0);
        if (img.pixelRatio <= 0) fail("maps.badArgument", "pixelRatio must be positive");
        img.sdf = op.value("sdf", false);
        img.stretchX = op.value("stretchX", json());
        img.stretchY = op.value("stretchY", json());
        img.content = op.value("content", json());
        s.images[img.id] = img;
        return;
    }

    if (kind == "putLayer") {
        const std::string local = requireLocalId(op, "id");
        const std::string type = requireString(op, "type");
        if (!kLayerTypes.count(type)) fail("maps.badArgument", "unknown layer type '" + type + "'");
        Layer l;
        l.id = globalId(w.sessionName, local);
        l.type = type;
        if (type != "background") {
            // A bare local id names this session's source; a global id may
            // reference another session's source (read-only sharing).
            const std::string srcLocal = requireString(op, "source");
            if (srcLocal.find(':') == std::string::npos && !isValidLocalId(srcLocal))
                fail("maps.badArgument", "invalid source id '" + srcLocal + "'");
            l.source = srcLocal.find(':') == std::string::npos ? globalId(w.sessionName, srcLocal) : srcLocal;
            const Source* src = (sessionOf(l.source) == w.sessionName)
                ? (s.sources.count(l.source) ? &s.sources.at(l.source) : nullptr)
                : m_scene.findSource(l.source);
            if (!src) fail("maps.unknownId", "layer '" + local + "' references unknown source '" + srcLocal + "'");
            if (src->type == "vector" && !(op.contains("sourceLayer") && op["sourceLayer"].is_string()))
                fail("maps.badArgument", "layer on a vector source needs sourceLayer");
        }
        if (op.contains("sourceLayer")) {
            if (!op["sourceLayer"].is_string()) fail("maps.badArgument", "sourceLayer must be a string");
            l.sourceLayer = op["sourceLayer"].get<std::string>();
        }
        for (const char* z : {"minzoom", "maxzoom"}) {
            if (!op.contains(z) || op[z].is_null()) continue;
            if (!op[z].is_number()) fail("maps.badArgument", std::string(z) + " must be a number");
            const double v = op[z].get<double>();
            if (v < 0 || v > 25.5) fail("maps.badArgument", std::string(z) + " out of range 0..25.5");
            (std::string(z) == "minzoom" ? l.minzoom : l.maxzoom) = v;
        }
        requireObjectOrAbsent(op, "layout");
        requireObjectOrAbsent(op, "paint");
        l.filter = op.value("filter", json());
        if (!l.filter.is_null() && !l.filter.is_array()) fail("maps.badArgument", "filter must be an expression array");
        l.layout = op.value("layout", json::object());
        l.paint = op.value("paint", json::object());
        if (l.layout.is_null()) l.layout = json::object();
        if (l.paint.is_null()) l.paint = json::object();
        try {
            l.placement = Placement::fromJson(op.value("placement", json()), w.sessionName);
        } catch (const std::invalid_argument& e) {
            fail("maps.badArgument", e.what());
        }
        if (Layer* existing = s.findLayer(l.id)) *existing = l;
        else s.layers.push_back(l);
        return;
    }

    if (kind == "setPaint" || kind == "setLayout" || kind == "setFilter" || kind == "placeLayer") {
        const std::string local = requireLocalId(op, "layerId");
        const std::string gid = globalId(w.sessionName, local);
        Layer* l = s.findLayer(gid);
        if (!l) fail("maps.unknownId", "unknown layer '" + local + "'");
        if (kind == "setFilter") {
            const json f = op.value("filter", json());
            if (!f.is_null() && !f.is_array()) fail("maps.badArgument", "filter must be an expression array");
            l->filter = f;
        } else if (kind == "placeLayer") {
            try {
                l->placement = Placement::fromJson(op.value("placement", json()), w.sessionName);
            } catch (const std::invalid_argument& e) {
                fail("maps.badArgument", e.what());
            }
        } else {
            if (!op.contains("props") || !op["props"].is_object()) fail("maps.badArgument", "props must be an object");
            json& target = (kind == "setPaint") ? l->paint : l->layout;
            for (auto it = op["props"].begin(); it != op["props"].end(); ++it) {
                if (it->is_null()) target.erase(it.key());
                else target[it.key()] = *it;
            }
        }
        return;
    }

    if (kind == "removeLayer") {
        const std::string gid = globalId(w.sessionName, requireLocalId(op, "id"));
        auto it = std::find_if(s.layers.begin(), s.layers.end(), [&](const Layer& l) { return l.id == gid; });
        if (it == s.layers.end()) fail("maps.unknownId", "unknown layer");
        s.layers.erase(it);
        return;
    }
    if (kind == "removeSource") {
        const std::string gid = globalId(w.sessionName, requireLocalId(op, "id"));
        if (!s.sources.erase(gid)) fail("maps.unknownId", "unknown source");
        // Removing a source removes its layers (M6.4).
        for (auto it = s.layers.begin(); it != s.layers.end();) {
            if (it->source == gid) it = s.layers.erase(it);
            else ++it;
        }
        return;
    }
    if (kind == "removeImage") {
        const std::string gid = globalId(w.sessionName, requireLocalId(op, "id"));
        if (!s.images.erase(gid)) fail("maps.unknownId", "unknown image");
        return;
    }
}

// ── diff ─────────────────────────────────────────────────────────────────

json Store::diffSession(const Session* before, const Session& after) const {
    json ops = json::array();
    auto add = [&](json op) { ops.push_back(std::move(op)); };

    // Layers: removed, then changed/added in final order.
    if (before) {
        for (const auto& l : before->layers)
            if (!after.findLayer(l.id)) add({{"op", "removeLayer"}, {"id", l.id}});
        for (const auto& [id, src] : before->sources)
            if (!after.sources.count(id)) add({{"op", "removeSource"}, {"id", id}});
        for (const auto& [id, img] : before->images)
            if (!after.images.count(id)) add({{"op", "removeImage"}, {"id", id}});
    }
    for (const auto& [id, src] : after.sources) {
        const Source* old = before && before->sources.count(id) ? &before->sources.at(id) : nullptr;
        if (!old || old->type != src.type || old->def != src.def) {
            add({{"op", "putSource"}, {"id", id}, {"type", src.type}, {"def", src.def}});
            if (src.type == "geojson") add({{"op", "replaceGeoJson"}, {"sourceId", id}, {"data", src.data}, {"digest", src.dataDigest}});
        } else if (src.type == "geojson" && old->dataDigest != src.dataDigest) {
            // Identity gate (M6.6): same digest -> nothing published.
            add({{"op", "replaceGeoJson"}, {"sourceId", id}, {"data", src.data}, {"digest", src.dataDigest}});
        }
    }
    for (const auto& [id, img] : after.images) {
        const Image* old = before && before->images.count(id) ? &before->images.at(id) : nullptr;
        if (!old || old->toJson() != img.toJson()) {
            json op = img.toJson();
            op["op"] = "putImage";
            add(op);
        }
    }
    // Relative order of the layers both sides share: if it is unchanged, only
    // changed/new layers are sent (new ones append); otherwise every layer is
    // re-sent so views rebuild the session order from the burst.
    bool orderChanged = false;
    if (before) {
        std::vector<std::string> a, b;
        for (const auto& l : before->layers) if (after.findLayer(l.id)) a.push_back(l.id);
        for (const auto& l : after.layers) if (before->findLayer(l.id)) b.push_back(l.id);
        orderChanged = (a != b);
    }
    for (const auto& l : after.layers) {
        const Layer* old = before ? before->findLayer(l.id) : nullptr;
        if (!old || old->toJson() != l.toJson() || orderChanged) {
            json op = l.toJson();
            op["op"] = "putLayer";
            add(op);
        }
    }
    std::stable_sort(ops.begin(), ops.end(), [](const json& a, const json& b) {
        return opRank(a["op"].get<std::string>()) < opRank(b["op"].get<std::string>());
    });
    return ops;
}

std::uint64_t Store::publish(const std::string& session, json ops) {
    ++m_revision;
    m_log.push_back(Published{m_revision, session, std::move(ops)});
    while (m_log.size() > m_limits.revisionLogLength) m_log.pop_front();
    return m_revision;
}

// ── public ───────────────────────────────────────────────────────────────

BatchResult Store::applyBatch(const std::string& sessionName, const json& batch, const std::string& caller) {
    if (!isValidSessionName(sessionName))
        throw MapsError("maps.badArgument", "session must be <module>/<name>: '" + sessionName + "'");

    json ops;
    if (batch.is_array()) {
        ops = batch;
    } else if (batch.is_object()) {
        const json v = batch.value("v", json(1));
        if (!v.is_number_integer() || v.get<int>() != 1)
            throw MapsError("maps.badArgument", "unsupported batch schema version");
        if (!batch.contains("ops") || !batch["ops"].is_array())
            throw MapsError("maps.badArgument", "batch.ops must be an array");
        ops = batch["ops"];
    } else {
        throw MapsError("maps.badArgument", "batch must be an array of ops or {v, ops}");
    }
    if (canonical(ops).size() > m_limits.maxBatchBytes)
        throw MapsError("maps.badArgument", "batch exceeds limits.maxBatchBytes");

    const Session* existing = m_scene.findSession(sessionName);
    if (existing && !caller.empty() && !existing->owner.empty() && existing->owner != caller)
        throw MapsError("maps.badArgument", "session '" + sessionName + "' is owned by " + existing->owner);

    Working w;
    w.sessionName = sessionName;
    if (existing) w.session = *existing;
    else { w.session.name = sessionName; w.session.owner = caller; }

    BatchResult result;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        try {
            validateAndApply(w, i, ops[i]);
            result.accepted.push_back(i);
        } catch (const OpError& e) {
            result.rejected.push_back(Rejection{i, e.code, e.message});
        }
    }

    if (canonical(w.session.toJson()).size() > m_limits.maxSessionBytes)
        throw MapsError("maps.badArgument", "session exceeds limits.maxSessionBytes");

    json diff = diffSession(existing, w.session);

    // A newly created, still-empty session is not committed: a batch of only
    // rejected ops must not leave a ghost session behind.
    const bool sessionHasContent = !w.session.sources.empty() || !w.session.layers.empty() || !w.session.images.empty();
    if (!existing && !sessionHasContent) {
        result.revision = m_revision;
        return result;
    }

    m_scene.sessions[sessionName] = std::move(w.session);
    if (diff.empty()) {
        // Every accepted op was a no-op after coalescing (identity gate).
        result.noop = result.accepted;
        result.revision = m_revision;
        return result;
    }
    result.ops = diff;
    result.published = true;
    result.revision = publish(sessionName, diff);
    return result;
}

bool Store::dropSession(const std::string& sessionName, const std::string& caller) {
    Session* s = m_scene.findSession(sessionName);
    if (!s) return false;
    if (!caller.empty() && !s->owner.empty() && s->owner != caller)
        throw MapsError("maps.badArgument", "session '" + sessionName + "' is owned by " + s->owner);
    Session empty;
    empty.name = s->name; empty.owner = s->owner; empty.priority = s->priority;
    json diff = diffSession(s, empty);
    m_scene.sessions.erase(sessionName);
    if (!diff.empty()) {
        diff.insert(diff.begin(), json{{"op", "dropSession"}, {"session", sessionName}});
        publish(sessionName, diff);
    }
    return true;
}

bool Store::setSessionPriority(const std::string& sessionName, int priority) {
    Session* s = m_scene.findSession(sessionName);
    if (!s) return false;
    if (s->priority == priority) return true;
    s->priority = priority;
    publish(sessionName, json::array({json{{"op", "setSessionPriority"}, {"session", sessionName}, {"priority", priority}}}));
    return true;
}

json Store::sceneJson(const std::optional<std::string>& session) const {
    if (session) {
        const Session* s = m_scene.findSession(*session);
        if (!s) throw MapsError("maps.unknownSession", "unknown session '" + *session + "'");
        json j = s->toJson();
        j["v"] = 1;
        j["revision"] = m_revision;
        return j;
    }
    json j = m_scene.toJson();
    j["revision"] = m_revision;
    return j;
}

json Store::changesSince(std::uint64_t since) const {
    json out = {{"v", 1}, {"from", since}, {"to", m_revision}};
    if (since > m_revision) throw MapsError("maps.badArgument", "revision is in the future");
    if (since == m_revision) { out["ops"] = json::array(); return out; }
    const bool inWindow = !m_log.empty() && m_log.front().revision <= since + 1;
    if (!inWindow) {
        out["full"] = true;
        out["scene"] = sceneJson();
        return out;
    }
    json ops = json::array();
    for (const auto& p : m_log) {
        if (p.revision <= since) continue;
        for (const auto& op : p.ops) {
            json o = op;
            o["session"] = p.session;
            o["revision"] = p.revision;
            ops.push_back(o);
        }
    }
    out["ops"] = ops;
    return out;
}

}  // namespace maps::engine
