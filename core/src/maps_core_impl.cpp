#include "maps_core_impl.h"

#include "engine/anchors.h"
#include "engine/builtin_basemaps.h"
#include "engine/canon.h"
#include "engine/capability.h"
#include "engine/projection.h"
#include "engine/stylepatch.h"

#include <logos_caller.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

using nlohmann::json;
using namespace maps::engine;

namespace {

StdLogosResult ok(json value) { return {true, std::move(value), ""}; }
StdLogosResult fail(const std::string& code, const std::string& message) {
    return {false, json(), code + ": " + message};
}
StdLogosResult fail(const MapsError& e) { return fail(e.code, e.what()); }

// Parse a JSON text argument. Returns false and fills `err` on syntax errors.
bool parseJson(const std::string& text, json& out, std::string& err) {
    if (text.empty()) { out = json(); return true; }
    try {
        out = json::parse(text);
        return true;
    } catch (const json::exception& e) {
        err = e.what();
        return false;
    }
}

// M12.1: payload objects may carry "v"; anything but 1 is rejected.
bool versionOk(const json& j) {
    if (!j.is_object() || !j.contains("v")) return true;
    return j["v"].is_number_integer() && j["v"].get<int>() == 1;
}

json withV(json j) {
    if (j.is_object()) j["v"] = 1;
    return j;
}

const std::set<std::string> kIntents = {"jumpTo", "easeTo", "flyTo", "fitBounds", "fitCoordinates", "cancel"};
const std::set<std::string> kViewTopics = {
    "frameRendered", "cameraChanged", "cameraIdle", "gestureBegan", "gestureEnded", "featurePicked",
    "trackedMoved", "basemapLoading", "basemapLoaded", "basemapFailed", "snapshotReady", "providerRequest",
    "attributionChanged", "cacheStats", "styleReady"};

}  // namespace

// ── lifecycle ────────────────────────────────────────────────────────────

MapsCoreImpl::MapsCoreImpl() : m_store(m_limits) {
    std::string err;
    for (const auto& d : builtinBasemaps()) m_catalogue.add(d, &err);
}

MapsCoreImpl::~MapsCoreImpl() = default;

void MapsCoreImpl::onContextReady() {
    loadUserBasemaps();
}

std::string MapsCoreImpl::callerModule() const {
    const logos::LogosCaller& c = logos::currentCaller();
    return c.isModule() ? c.name : std::string();
}

bool MapsCoreImpl::sessionAllowed(const std::string& session, std::string* error) const {
    if (!isValidSessionName(session)) {
        if (error) *error = "maps.badArgument: session must be <module>/<name>: '" + session + "'";
        return false;
    }
    // A known caller may only create sessions under its own name; existing
    // sessions are further guarded by the store's owner check.
    const std::string caller = callerModule();
    if (!caller.empty() && !m_store.scene().findSession(session) && session.rfind(caller + "/", 0) != 0) {
        if (error) *error = "maps.badArgument: module '" + caller + "' may only create sessions named '" + caller + "/<name>'";
        return false;
    }
    return true;
}

std::string MapsCoreImpl::nextRequestId() {
    return "r" + std::to_string(++m_requestCounter);
}

// ── scene ────────────────────────────────────────────────────────────────

StdLogosResult MapsCoreImpl::applyBatch(const std::string& session, const std::string& batchJson) {
    std::string err;
    if (!sessionAllowed(session, &err)) return {false, json(), err};
    json batch;
    if (!parseJson(batchJson, batch, err)) return fail("maps.badArgument", "batch is not JSON: " + err);
    if (!versionOk(batch)) return fail("maps.badArgument", "unsupported batch version");
    try {
        BatchResult r = m_store.applyBatch(session, batch, callerModule());
        if (r.published) {
            revisionPublished(json{{"v", 1}, {"revision", r.revision}, {"sessions", json::array({session})}}.dump());
        }
        return ok(r.toJson());
    } catch (const MapsError& e) {
        return fail(e);
    }
}

StdLogosResult MapsCoreImpl::getScene(const std::optional<std::string>& session) {
    try {
        return ok(m_store.sceneJson(session && !session->empty() ? session : std::nullopt));
    } catch (const MapsError& e) {
        return fail(e);
    }
}

StdLogosResult MapsCoreImpl::getChanges(uint64_t sinceRevision) {
    try {
        return ok(m_store.changesSince(sinceRevision));
    } catch (const MapsError& e) {
        return fail(e);
    }
}

bool MapsCoreImpl::dropSession(const std::string& session) {
    try {
        const bool dropped = m_store.dropSession(session, callerModule());
        m_providers.dropSession(session);
        for (auto it = m_featureState.begin(); it != m_featureState.end();)
            it = (sessionOf(it->first) == session) ? m_featureState.erase(it) : std::next(it);
        if (dropped) {
            revisionPublished(json{{"v", 1}, {"revision", m_store.revision()}, {"sessions", json::array({session})}}.dump());
        }
        return dropped;
    } catch (const MapsError&) {
        return false;
    }
}

bool MapsCoreImpl::setSessionPriority(const std::string& session, int64_t priority) {
    const auto before = m_store.revision();
    if (!m_store.setSessionPriority(session, static_cast<int>(priority))) return false;
    if (m_store.revision() != before)
        revisionPublished(json{{"v", 1}, {"revision", m_store.revision()}, {"sessions", json::array({session})}}.dump());
    return true;
}

bool MapsCoreImpl::setFeatureState(const std::string& session, const std::string& sourceId,
                                   const std::string& featureId, const std::string& stateJson) {
    std::string err;
    json state;
    if (!isValidSessionName(session) || featureId.empty() || !parseJson(stateJson, state, err) || !state.is_object()) return false;
    const std::string gid = globalId(session, sourceId);
    const Source* src = m_store.scene().findSource(gid);
    if (!src) return false;
    json& cur = m_featureState[gid][featureId];
    if (!cur.is_object()) cur = json::object();
    for (auto it = state.begin(); it != state.end(); ++it) {
        if (it.key() == "v") continue;
        if (it->is_null()) cur.erase(it.key());
        else cur[it.key()] = *it;
    }
    featureStateChanged(json{{"v", 1}, {"session", session}, {"sourceId", gid}, {"featureId", featureId}, {"state", cur}}.dump());
    return true;
}

bool MapsCoreImpl::clearFeatureState(const std::string& session, const std::string& sourceId,
                                     const std::optional<std::string>& featureId) {
    const std::string gid = globalId(session, sourceId);
    auto it = m_featureState.find(gid);
    if (it == m_featureState.end()) return false;
    if (featureId && !featureId->empty()) {
        if (!it->second.erase(*featureId)) return false;
        featureStateChanged(json{{"v", 1}, {"session", session}, {"sourceId", gid}, {"featureId", *featureId}, {"state", nullptr}}.dump());
    } else {
        m_featureState.erase(it);
        featureStateChanged(json{{"v", 1}, {"session", session}, {"sourceId", gid}, {"featureId", nullptr}, {"state", nullptr}}.dump());
    }
    return true;
}

// ── basemaps ─────────────────────────────────────────────────────────────

std::string MapsCoreImpl::basemapsFile() const {
    if (instancePersistencePath().empty()) return {};
    return instancePersistencePath() + "/basemaps.json";
}

void MapsCoreImpl::loadUserBasemaps() {
    const std::string path = basemapsFile();
    if (path.empty()) return;
    std::ifstream in(path);
    if (!in) return;
    json all;
    try { all = json::parse(in); } catch (const json::exception&) { return; }
    if (!all.is_object() || !all.contains("basemaps") || !all["basemaps"].is_array()) return;
    std::string err;
    for (const auto& d : all["basemaps"]) {
        if (m_catalogue.add(d, &err) && d.contains("id")) m_userBasemaps[d["id"].get<std::string>()] = d;
    }
}

void MapsCoreImpl::saveUserBasemaps() const {
    const std::string path = basemapsFile();
    if (path.empty()) return;
    json arr = json::array();
    for (const auto& [id, d] : m_userBasemaps) arr.push_back(d);
    std::ofstream out(path, std::ios::trunc);
    if (out) out << json{{"v", 1}, {"basemaps", arr}}.dump(2) << "\n";
}

std::string MapsCoreImpl::listBasemaps(const std::string& filterJson) {
    json filter;
    std::string err;
    if (!parseJson(filterJson, filter, err) || !filter.is_object()) filter = json::object();
    return withV(m_catalogue.list(filter)).dump();
}

StdLogosResult MapsCoreImpl::addBasemap(const std::string& descriptorJson) {
    json d;
    std::string err;
    if (!parseJson(descriptorJson, d, err)) return fail("maps.badArgument", "descriptor is not JSON: " + err);
    if (!m_catalogue.add(d, &err)) return fail("maps.badArgument", err);
    const std::string id = d["id"].get<std::string>();
    m_userBasemaps[id] = d;
    m_patchedStyles.clear();
    saveUserBasemaps();
    catalogueChanged(json{{"v", 1}, {"id", id}, {"action", "added"}}.dump());
    return ok(*m_catalogue.find(id));
}

bool MapsCoreImpl::removeBasemap(const std::string& id) {
    if (!m_userBasemaps.erase(id)) return false;  // bundled descriptors stay
    m_catalogue.remove(id);
    // A bundled descriptor shadowed by a user one comes back.
    std::string err;
    for (const auto& d : builtinBasemaps())
        if (d["id"] == id) m_catalogue.add(d, &err);
    saveUserBasemaps();
    catalogueChanged(json{{"v", 1}, {"id", id}, {"action", "removed"}}.dump());
    return true;
}

StdLogosResult MapsCoreImpl::setBasemap(const std::string& viewId, const std::string& id) {
    ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    const json* d = m_catalogue.find(id);
    if (!d) return fail("maps.unknownId", "no basemap '" + id + "'");
    v->basemap = id;
    basemapChanged(json{{"v", 1}, {"viewId", viewId}, {"id", id}}.dump());
    ResolvedOrder order = resolveOrder(m_store.scene(), (*d)["anchors"]);
    emitAnchorMissing(*v, order.missingAnchors);
    return ok(json{{"v", 1}, {"viewId", viewId}, {"id", id}});
}

StdLogosResult MapsCoreImpl::getBasemap(const std::string& id) {
    const json* d = m_catalogue.find(id);
    if (!d) return fail("maps.unknownId", "no basemap '" + id + "'");
    json out = {{"v", 1}, {"descriptor", *d}};
    if ((*d)["format"] == "raster") out["style"] = rasterStyle(*d);
    return ok(out);
}

StdLogosResult MapsCoreImpl::patchStyle(const std::string& id, const std::string& styleJson) {
    const json* d = m_catalogue.find(id);
    if (!d) return fail("maps.unknownId", "no basemap '" + id + "'");
    const std::string fp = (*d)["fingerprint"].get<std::string>() + ":" + sha256Hex(styleJson);
    auto cached = m_patchedStyles.find(fp);
    if (cached != m_patchedStyles.end()) return ok(cached->second);
    json style;
    std::string err;
    if (!parseJson(styleJson, style, err) || !style.is_object()) return fail("maps.badArgument", "style is not a JSON object");
    try {
        json ops = d->value("patch", json::array());
        // The descriptor's language choice is a patch rule like any other.
        const std::string lang = d->value("lang", "local");
        if (lang != "local") ops.push_back(json{{"op", "labelLanguage"}, {"lang", lang}, {"key", d->value("lang_key", "name:{lang}")}});
        json patched = applyStylePatch(style, ops);
        if (m_patchedStyles.size() > 16) m_patchedStyles.clear();
        m_patchedStyles[fp] = patched;
        return ok(patched);
    } catch (const std::invalid_argument& e) {
        return fail("maps.badArgument", e.what());
    }
}

// ── views & camera intents ───────────────────────────────────────────────

std::string MapsCoreImpl::listViews() { return m_views.list().dump(); }

StdLogosResult MapsCoreImpl::getViewState(const std::string& viewId) {
    const ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    json out = v->state.is_object() ? v->state : json::object();
    out["v"] = 1;
    out["viewId"] = viewId;
    out["basemap"] = v->basemap;
    out["appliedRevision"] = v->appliedRevision;
    out["renderedRevision"] = v->renderedRevision;
    return ok(out);
}

StdLogosResult MapsCoreImpl::camera(const std::string& viewId, const std::string& intentJson) {
    const ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    json intent;
    std::string err;
    if (!parseJson(intentJson, intent, err) || !intent.is_object()) return fail("maps.badArgument", "intent must be a JSON object");
    if (!versionOk(intent)) return fail("maps.badArgument", "unsupported intent version");
    const std::string type = intent.value("type", "");
    if (!kIntents.count(type)) return fail("maps.badArgument", "intent.type must be one of jumpTo|easeTo|flyTo|fitBounds|fitCoordinates|cancel");
    if (type == "fitBounds" && !(intent.contains("bounds") && intent["bounds"].is_array() && intent["bounds"].size() == 2))
        return fail("maps.badArgument", "fitBounds needs bounds [[w,s],[e,n]]");
    if (type == "fitCoordinates" && !(intent.contains("coordinates") && intent["coordinates"].is_array() && !intent["coordinates"].empty()))
        return fail("maps.badArgument", "fitCoordinates needs a non-empty coordinates array");
    if ((type == "jumpTo" || type == "easeTo" || type == "flyTo") && intent.contains("center")) {
        const json& c = intent["center"];
        if (!c.is_array() || c.size() != 2 || !c[0].is_number() || !c[1].is_number()) return fail("maps.badArgument", "center must be [lon, lat]");
    }
    cameraIntent(json{{"v", 1}, {"viewId", viewId}, {"intent", intent}}.dump());
    return ok(json{{"v", 1}, {"viewId", viewId}});
}

StdLogosResult MapsCoreImpl::setViewOptions(const std::string& viewId, const std::string& optionsJson) {
    ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    json options;
    std::string err;
    if (!parseJson(optionsJson, options, err) || !options.is_object()) return fail("maps.badArgument", "options must be a JSON object");
    if (!versionOk(options)) return fail("maps.badArgument", "unsupported options version");
    json& stored = v->state["options"];
    if (!stored.is_object()) stored = json::object();
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (it.key() == "v") continue;
        if (it->is_null()) stored.erase(it.key());
        else stored[it.key()] = *it;
    }
    viewOptionsChanged(json{{"v", 1}, {"viewId", viewId}, {"options", stored}}.dump());
    return ok(json{{"v", 1}, {"viewId", viewId}, {"options", stored}});
}

StdLogosResult MapsCoreImpl::toScreen(const std::string& viewId, const std::string& lonLatsJson) {
    const ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    auto cam = Camera::fromJson(v->state.value("camera", json()));
    if (!cam) return fail("maps.styleNotLoaded", "view has not reported a camera yet");
    json pts;
    std::string err;
    if (!parseJson(lonLatsJson, pts, err) || !pts.is_array()) return fail("maps.badArgument", "expected [[lon,lat],...]");
    json out = json::array();
    for (const auto& p : pts) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) return fail("maps.badArgument", "expected [[lon,lat],...]");
        Point s = maps::engine::toScreen(*cam, {p[0].get<double>(), p[1].get<double>()});
        out.push_back({s.x, s.y});
    }
    return ok(json{{"v", 1}, {"points", out}});
}

StdLogosResult MapsCoreImpl::toGeo(const std::string& viewId, const std::string& pixelsJson) {
    const ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    auto cam = Camera::fromJson(v->state.value("camera", json()));
    if (!cam) return fail("maps.styleNotLoaded", "view has not reported a camera yet");
    json pts;
    std::string err;
    if (!parseJson(pixelsJson, pts, err) || !pts.is_array()) return fail("maps.badArgument", "expected [[x,y],...]");
    json out = json::array();
    for (const auto& p : pts) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) return fail("maps.badArgument", "expected [[x,y],...]");
        LonLat g = maps::engine::toGeo(*cam, {p[0].get<double>(), p[1].get<double>()});
        out.push_back({g.lon, g.lat});
    }
    return ok(json{{"v", 1}, {"coordinates", out}});
}

StdLogosResult MapsCoreImpl::trackCoordinates(const std::string& viewId, const std::string& session, const std::string& trackedJson) {
    const ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    if (!isValidSessionName(session)) return fail("maps.badArgument", "session must be <module>/<name>");
    json tracked;
    std::string err;
    if (!parseJson(trackedJson, tracked, err) || !tracked.is_array()) return fail("maps.badArgument", "tracked must be an array");
    trackRequested(json{{"v", 1}, {"viewId", viewId}, {"session", session}, {"tracked", tracked}}.dump());
    return ok(json{{"v", 1}, {"viewId", viewId}, {"count", tracked.size()}});
}

StdLogosResult MapsCoreImpl::snapshot(const std::string& viewId, const std::string& optionsJson) {
    const ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    json options;
    std::string err;
    if (!parseJson(optionsJson, options, err)) return fail("maps.badArgument", "options is not JSON");
    if (options.is_null()) options = json::object();
    if (!options.is_object()) return fail("maps.badArgument", "options must be a JSON object");
    const std::string requestId = nextRequestId();
    snapshotRequested(json{{"v", 1}, {"viewId", viewId}, {"requestId", requestId}, {"options", options}}.dump());
    return ok(json{{"v", 1}, {"requestId", requestId}});
}

json MapsCoreImpl::anchorsFor(const ViewRecord& view) const {
    const json* d = m_catalogue.find(view.basemap.empty() ? defaultBasemapId() : view.basemap);
    return d ? (*d)["anchors"] : json::object();
}

void MapsCoreImpl::emitAnchorMissing(const ViewRecord& view, const std::vector<std::string>& anchors) {
    for (const auto& a : anchors)
        anchorMissing(json{{"v", 1}, {"viewId", view.viewId}, {"basemap", view.basemap}, {"anchor", a}}.dump());
}

StdLogosResult MapsCoreImpl::getLayerOrder(const std::string& viewId) {
    const ViewRecord* v = m_views.find(viewId);
    if (!v) return fail("maps.noSuchView", viewId);
    ResolvedOrder order = resolveOrder(m_store.scene(), anchorsFor(*v));
    json out = order.toJson();
    out["revision"] = m_store.revision();
    out["basemap"] = v->basemap;
    return ok(out);
}

// ── view-side protocol ───────────────────────────────────────────────────

StdLogosResult MapsCoreImpl::openView(const std::string& viewId, const std::string& surfaceToken, const std::string& capabilitiesJson) {
    if (viewId.empty()) return fail("maps.badArgument", "viewId is required");
    json caps;
    std::string err;
    if (!parseJson(capabilitiesJson, caps, err)) return fail("maps.badArgument", "capabilities is not JSON: " + err);
    if (caps.is_null()) caps = json::object();
    if (!caps.is_object()) return fail("maps.badArgument", "capabilities must be a JSON object");
    if (m_views.find(viewId)) m_views.close(viewId);  // a restarted view re-registers under its id
    m_views.open(viewId, surfaceToken, caps);
    ViewRecord* v = m_views.find(viewId);
    v->basemap = defaultBasemapId();
    viewOpened(json{{"v", 1}, {"viewId", viewId}, {"capabilities", caps}}.dump());
    return ok(json{{"v", 1}, {"viewId", viewId}, {"revision", m_store.revision()}, {"basemap", v->basemap},
                   {"limits", {{"maxBatchBytes", m_limits.maxBatchBytes}, {"maxImageBytes", m_limits.maxImageBytes}}}});
}

bool MapsCoreImpl::closeView(const std::string& viewId) {
    if (!m_views.close(viewId)) return false;
    viewClosed(json{{"v", 1}, {"viewId", viewId}}.dump());
    return true;
}

bool MapsCoreImpl::ackRevision(const std::string& viewId, uint64_t revision) {
    if (!m_views.ack(viewId, revision)) return false;
    revisionApplied(json{{"v", 1}, {"viewId", viewId}, {"revision", revision}}.dump());
    return true;
}

bool MapsCoreImpl::reportViewState(const std::string& viewId, const std::string& stateJson) {
    ViewRecord* v = m_views.find(viewId);
    if (!v) return false;
    json state;
    std::string err;
    if (!parseJson(stateJson, state, err) || !state.is_object()) return false;
    json options = v->state.value("options", json());
    v->state = state;
    if (!options.is_null() && !v->state.contains("options")) v->state["options"] = options;
    if (state.contains("basemap") && state["basemap"].is_string()) v->basemap = state["basemap"].get<std::string>();
    viewStateChanged(json{{"v", 1}, {"viewId", viewId}, {"state", v->state}}.dump());
    return true;
}

bool MapsCoreImpl::reportViewEvent(const std::string& viewId, const std::string& topic, const std::string& payloadJson) {
    ViewRecord* v = m_views.find(viewId);
    if (!v || !kViewTopics.count(topic)) return false;
    json payload;
    std::string err;
    if (!parseJson(payloadJson, payload, err)) return false;
    if (payload.is_null()) payload = json::object();
    if (topic == "frameRendered" && payload.is_object() && payload.contains("revision") && payload["revision"].is_number_unsigned())
        m_views.rendered(viewId, payload["revision"].get<std::uint64_t>());
    if (topic == "cacheStats" && payload.is_object()) m_cacheStats = payload;
    viewEvent(json{{"v", 1}, {"viewId", viewId}, {"topic", topic}, {"payload", payload}}.dump());
    return true;
}

// ── providers ────────────────────────────────────────────────────────────

StdLogosResult MapsCoreImpl::registerProvider(const std::string& session, const std::string& scheme, const std::string& specJson) {
    std::string err;
    if (!sessionAllowed(session, &err)) return {false, json(), err};
    json spec;
    if (!parseJson(specJson, spec, err)) return fail("maps.badArgument", "spec is not JSON: " + err);
    const std::string problem = m_providers.registerProvider(session, scheme, spec);
    if (!problem.empty()) return fail("maps.badArgument", problem);
    return ok(withV(m_providers.list()));
}

bool MapsCoreImpl::unregisterProvider(const std::string& session, const std::string& scheme) {
    return m_providers.unregisterProvider(session, scheme);
}

std::string MapsCoreImpl::listProviders() { return withV(m_providers.list()).dump(); }

bool MapsCoreImpl::clearCache() {
    if (m_views.size() == 0) return false;
    const std::string requestId = nextRequestId();
    for (const auto& id : m_views.ids())
        cacheClearRequested(json{{"v", 1}, {"viewId", id}, {"requestId", requestId}}.dump());
    return true;
}

std::string MapsCoreImpl::cacheStats() { return withV(m_cacheStats).dump(); }

// ── meta ─────────────────────────────────────────────────────────────────

StdLogosResult MapsCoreImpl::getCapabilities(const std::optional<std::string>& viewId) {
    json limits = {{"maxImageBytes", m_limits.maxImageBytes}, {"maxBatchBytes", m_limits.maxBatchBytes}, {"maxPickHits", 64}};
    const json defaults = defaultCapabilities(limits);
    if (viewId && !viewId->empty()) {
        const ViewRecord* v = m_views.find(*viewId);
        if (!v) return fail("maps.noSuchView", *viewId);
        json out = aggregateCapabilities({v->capabilities}, defaults);
        out["v"] = 1;
        out["viewId"] = *viewId;
        return ok(out);
    }
    json out = aggregateCapabilities(m_views.capabilities(), defaults);
    out["v"] = 1;
    out["views"] = m_views.ids();
    if (m_providers.size() > 0) {
        json schemes = getPath(out, "provider.schemes");
        if (!schemes.is_array()) schemes = json::array();
        for (const auto& s : m_providers.schemes())
            if (std::find(schemes.begin(), schemes.end(), json(s)) == schemes.end()) schemes.push_back(s);
        setPath(out, "provider.schemes", schemes);
    }
    return ok(out);
}

std::string MapsCoreImpl::schemaVersion() { return "maps/1"; }
