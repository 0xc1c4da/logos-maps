#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "logos_module_context.h"
#include "logos_result.h"

#include "engine/basemap.h"
#include "engine/registry.h"
#include "engine/store.h"

/**
 * maps_core -- the retained map scene (docs/architecture.md §12.1).
 *
 * Runs in its own logos_host process, under Basecamp or logosctl alike. Any
 * module publishes sources, layers and images into its own session through
 * applyBatch; this class validates, coalesces and versions the scene
 * (engine/store.h), keeps the basemap catalogue (engine/basemap.h), knows
 * every open Maps view (engine/registry.h) and relays camera intents and view
 * events over the event bus. It never waits on a view (M12.9).
 *
 * Conventions:
 *   - Structured input travels as JSON text in `std::string` (M12.1: every
 *     payload may carry "v": 1; unknown keys are ignored).
 *   - Fallible methods return StdLogosResult: `success`, a JSON `value`, and
 *     on failure an `error` of the form "<code>: <message>" where <code> is
 *     one of the M11.2 codes (maps.badArgument, maps.unknownSession,
 *     maps.unknownId, maps.noSuchView, maps.unsupported, ...).
 *   - Session ownership is the caller identity the host delivers
 *     (logos::currentCaller()); unknown callers (logosctl, tests) may touch
 *     any session.
 *   - Events carry one JSON string. Topics are "<event name>" under this
 *     module; §12.4 lists them as maps.<name>.
 */
class MapsCoreImpl : public LogosModuleContext
{
public:
    MapsCoreImpl();
    ~MapsCoreImpl() override;

    // ── scene ────────────────────────────────────────────────────────────

    /// Apply an ordered op array (or {"v":1,"ops":[...]}) to `session`
    /// ("<module>/<name>"). value = {revision, published, accepted[], noop[],
    /// rejected[{index,code,message}]}. Fails only when the payload is
    /// malformed or the session belongs to another module (M6.7).
    StdLogosResult applyBatch(const std::string& session, const std::string& batchJson);

    /// The whole scene, or one session's subtree, as JSON with `revision`.
    StdLogosResult getScene(const std::optional<std::string>& session);

    /// Published ops after `sinceRevision` ({"from","to","ops":[...]}) or the
    /// full scene ({"full":true,"scene":…}) when the log window is exceeded.
    StdLogosResult getChanges(uint64_t sinceRevision);

    /// Remove everything in `session` (and its providers). false if unknown.
    bool dropSession(const std::string& session);

    /// Host-assigned ordering between sessions (M6.10). false if unknown.
    bool setSessionPriority(const std::string& session, int64_t priority);

    /// Per-feature state (hover/selected…) for `featureId` in `sourceId`;
    /// `stateJson` is an object merged into the current state. Relayed to
    /// views as featureStateChanged.
    bool setFeatureState(const std::string& session, const std::string& sourceId, const std::string& featureId, const std::string& stateJson);
    /// Clear one feature's state, or every feature's when featureId is absent.
    bool clearFeatureState(const std::string& session, const std::string& sourceId, const std::optional<std::string>& featureId);

    // ── basemaps ─────────────────────────────────────────────────────────

    /// Catalogue entries matching `filterJson` ({} for all) -> {"basemaps":[…]}.
    std::string listBasemaps(const std::string& filterJson);
    /// Add or replace a descriptor (Appendix C). Persisted per instance.
    StdLogosResult addBasemap(const std::string& descriptorJson);
    /// Remove a user-added descriptor. Bundled ones cannot be removed.
    bool removeBasemap(const std::string& id);
    /// Select `id` for `viewId`; emits basemapChanged for the view to load.
    StdLogosResult setBasemap(const std::string& viewId, const std::string& id);
    /// Descriptor plus, for raster basemaps, the generated style (M9.2):
    /// {"descriptor":…, "style":…?}. Views fetch mapbox-gl styles themselves.
    StdLogosResult getBasemap(const std::string& id);
    /// Apply `id`'s patch rules (M9.6) to a fetched style JSON. Cached per
    /// fingerprint. value = patched style.
    StdLogosResult patchStyle(const std::string& id, const std::string& styleJson);

    // ── views & camera intents ───────────────────────────────────────────

    /// {"views":[{viewId, surfaceToken, capabilities, state, basemap, …}]}
    std::string listViews();
    /// camera, basemap, size, dpr, gestureState as last reported by the view.
    StdLogosResult getViewState(const std::string& viewId);
    /// Relay a camera intent (jumpTo|easeTo|flyTo|fitBounds|fitCoordinates|
    /// cancel) to the view as cameraIntent. Validated here; applied there.
    StdLogosResult camera(const std::string& viewId, const std::string& intentJson);
    /// Relay view options (padding, constraints, gestures §8.2, pick priority).
    StdLogosResult setViewOptions(const std::string& viewId, const std::string& optionsJson);
    /// [[lon,lat],…] -> [[x,y],…] from the view's last camera (Web Mercator,
    /// pitch ignored).
    StdLogosResult toScreen(const std::string& viewId, const std::string& lonLatsJson);
    /// [[x,y],…] -> [[lon,lat],…].
    StdLogosResult toGeo(const std::string& viewId, const std::string& pixelsJson);
    /// Ask the view to report screen positions of `trackedJson` ([ids] or
    /// [[lon,lat],…]) on every frame as trackedMoved.
    StdLogosResult trackCoordinates(const std::string& viewId, const std::string& session, const std::string& trackedJson);
    /// Request a snapshot; value = {"requestId"}; result via snapshotReady.
    StdLogosResult snapshot(const std::string& viewId, const std::string& optionsJson);
    /// Layer apply order for `viewId`'s basemap (§6.6): {"layers":[{id,
    /// before, anchor}], "missingAnchors":[…]}.
    StdLogosResult getLayerOrder(const std::string& viewId);

    // ── view-side protocol (called by maps_view) ─────────────────────────

    /// A view process announces itself. capabilitiesJson per M11.1.
    /// value = {"viewId", "revision", "basemap", "limits"}.
    StdLogosResult openView(const std::string& viewId, const std::string& surfaceToken, const std::string& capabilitiesJson);
    bool closeView(const std::string& viewId);
    /// M6.7 acknowledgement; emits revisionApplied.
    bool ackRevision(const std::string& viewId, uint64_t revision);
    /// The view's current camera/size/gesture state; stored and republished.
    bool reportViewState(const std::string& viewId, const std::string& stateJson);
    /// A view-originated event (frameRendered, cameraChanged, cameraIdle,
    /// gestureBegan/Ended, featurePicked, trackedMoved, basemapLoading/
    /// Loaded/Failed, snapshotReady, providerRequest, attributionChanged).
    /// Re-emitted on the bus with viewId stamped in.
    bool reportViewEvent(const std::string& viewId, const std::string& topic, const std::string& payloadJson);

    // ── providers ────────────────────────────────────────────────────────

    /// Declare that `spec.module` implements the tileProvider contract
    /// (§12.5) for logos://<module>/<prefix>. Only scheme "logos".
    StdLogosResult registerProvider(const std::string& session, const std::string& scheme, const std::string& specJson);
    bool unregisterProvider(const std::string& session, const std::string& scheme);
    /// {"providers":[…]} -- views consult this to route logos:// requests.
    std::string listProviders();
    /// Ask every open view to clear the engine cache (cacheClearRequested). false when no view is open.
    bool clearCache();
    /// Last cacheStats reported by any view, or {} .
    std::string cacheStats();

    // ── meta ─────────────────────────────────────────────────────────────

    /// Aggregated over open views, or one view's (M11.1).
    StdLogosResult getCapabilities(const std::optional<std::string>& viewId);
    /// "maps/1"
    std::string schemaVersion();

logos_events:
    /// {revision, sessions:[…]} -- a new revision exists; views pull getChanges.
    void revisionPublished(const std::string& json);
    /// {viewId, revision}
    void revisionApplied(const std::string& json);
    /// {viewId, id} -- the view must load this basemap.
    void basemapChanged(const std::string& json);
    /// {viewId, basemap, anchor}
    void anchorMissing(const std::string& json);
    /// {viewId, capabilities}
    void viewOpened(const std::string& json);
    /// {viewId}
    void viewClosed(const std::string& json);
    /// {viewId, state}
    void viewStateChanged(const std::string& json);
    /// {viewId, intent}
    void cameraIntent(const std::string& json);
    /// {viewId, options}
    void viewOptionsChanged(const std::string& json);
    /// {viewId, session, tracked}
    void trackRequested(const std::string& json);
    /// {viewId, requestId, options}
    void snapshotRequested(const std::string& json);
    /// {session, sourceId, featureId|null, state|null}
    void featureStateChanged(const std::string& json);
    /// {viewId, requestId}
    void cacheClearRequested(const std::string& json);
    /// {viewId, topic, payload} -- every view-originated event (§12.4).
    void viewEvent(const std::string& json);
    /// {id, action: "added"|"removed"}
    void catalogueChanged(const std::string& json);

protected:
    void onContextReady() override;

private:
    maps::engine::Limits m_limits;
    maps::engine::Store m_store;
    maps::engine::Catalogue m_catalogue;
    maps::engine::Views m_views;
    maps::engine::Providers m_providers;
    std::map<std::string, nlohmann::json> m_userBasemaps;              // id -> descriptor (persisted)
    std::map<std::string, nlohmann::json> m_patchedStyles;             // fingerprint -> style
    std::map<std::string, std::map<std::string, nlohmann::json>> m_featureState;  // "<session>:<source>" -> featureId -> state
    nlohmann::json m_cacheStats = nlohmann::json::object();
    std::uint64_t m_requestCounter = 0;

    std::string callerModule() const;
    bool sessionAllowed(const std::string& session, std::string* error) const;
    std::string basemapsFile() const;
    void loadUserBasemaps();
    void saveUserBasemaps() const;
    nlohmann::json anchorsFor(const maps::engine::ViewRecord& view) const;
    void emitAnchorMissing(const maps::engine::ViewRecord& view, const std::vector<std::string>& anchors);
    std::string nextRequestId();
};
