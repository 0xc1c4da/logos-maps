// SceneApplier: applies maps_core's published scene to the engine
// (docs/architecture.md section 6.4-6.6, M6.7-M6.10, M11.3).
//
// Revisions arrive as op bursts (getChanges) or as the full scene (getScene,
// or getChanges when the log window was exceeded). Ops are applied in the
// order the core publishes them (removes, sources, GeoJSON data, images,
// layers); layer *order* is never interpreted here -- after any layer change
// the core's getLayerOrder() resolves placements to concrete `before` ids and
// the scene layers are re-inserted in that order (M6.10).
//
// Application is blocked between WillStartLoadingMap and DidFinishLoadingStyle
// (M6.8); the style load that ends the window triggers a full replay from the
// core's current scene. Every applied revision is acknowledged with
// ackRevision; the first fully rendered frame afterwards is reported as
// frameRendered(revision) (M6.7).
#pragma once

#include "view/capabilities.h"
#include "view/core_client.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <map>
#include <optional>
#include <vector>

namespace QMapLibre { class Map; }

namespace maps::view {

class SceneApplier : public QObject {
    Q_OBJECT
public:
    SceneApplier(QMapLibre::Map* map, CoreClient* core, QObject* parent = nullptr);

    void setViewId(const QString& viewId) { m_viewId = viewId; }
    QString viewId() const { return m_viewId; }
    /// Replace the engine (tests re-use an applier across maps).
    void setMap(QMapLibre::Map* map) { m_map = map; }

    quint64 appliedRevision() const { return m_appliedRevision; }
    bool blocked() const { return m_blocked; }
    bool busy() const { return m_inFlight; }

    /// Scene layer ids in current bottom-to-top order (after reconcile).
    QStringList layerOrder() const { return m_order; }
    QStringList missingAnchors() const { return m_missingAnchors; }
    QStringList knownSources() const;
    /// Scene layers as {id, source, sourceLayer, type}, in current order
    /// (the engine's style JSON is the loaded document and does not list
    /// runtime layers, so picking resolves them from here -- M8.13).
    struct LayerInfo {
        QString id, source, sourceLayer, type;
    };
    std::vector<LayerInfo> layerInfos() const;
    QStringList knownLayers() const;

    // Driven by MapItem::mapChanged through ViewController, or directly in tests.
    void onStyleWillLoad();      // WillStartLoadingMap: open the blocking window
    void onStyleLoaded();        // DidFinishLoadingStyle: replay (M6.8)
    void onFrameFullyRendered(); // DidFinishRenderingFrameFullyRendered: frameRendered(revision)

    /// A revisionPublished event: pull changes unless blocked.
    void onRevisionPublished(const QJsonObject& payload);
    /// featureStateChanged event: native path (P2) or overlay fallback.
    void onFeatureStateChanged(const QJsonObject& payload);
    /// M8.15: native feature state (P2) or the overlay fallback. Defaults to
    /// the compiled capability; tests toggle it to compare the two paths.
    void setNativeFeatureState(bool native) { m_nativeFeatureState = native; }
    bool nativeFeatureState() const { return m_nativeFeatureState; }

    /// Apply a full {"v":1,"revision":N,"sessions":{...}} scene (replay).
    void applyScene(const QJsonObject& scene);
    /// Apply one published op burst ({"from","to","ops":[...]}) or full change set.
    void applyChanges(const QJsonObject& changes);
    /// Apply a single op (exposed for tests).
    void applyOp(const QJsonObject& op);

public slots:
    /// Fetch and apply everything since appliedRevision().
    void pullChanges();
    /// Fetch the full scene and apply it, then acknowledge.
    void replay();

signals:
    void revisionApplied(quint64 revision);
    void frameRendered(quint64 revision);
    void layersReconciled(const QStringList& order, const QStringList& missingAnchors);
    void sceneError(const QString& code, const QString& message);
    void featureStateFallback(const QString& sourceId, const QString& featureId, const QJsonValue& state);
    /// A scene source was (re)defined or its GeoJSON replaced.
    void sourceUpdated(const QString& sourceId);

private:
    struct LayerDef {
        QJsonObject params;  // style-spec layer object minus id/placement
        QString source;
    };

    void finishRevision(quint64 revision);
    void reconcileLayers(std::function<void()> then);
    void insertLayers(const QJsonArray& resolved);
    void removeSceneLayers();
    QVariantMap sourceParams(const QString& id) const;
    void putSource(const QJsonObject& op);
    void replaceGeoJson(const QJsonObject& op);
    void putImage(const QJsonObject& op);
    void putLayer(const QJsonObject& op);

    QMapLibre::Map* m_map;
    CoreClient* m_core;
    QString m_viewId;
    quint64 m_appliedRevision = 0;
    std::optional<quint64> m_renderPending;  // revision awaiting its first full frame
    bool m_blocked = false;
    bool m_pendingPull = false;
    bool m_inFlight = false;
    bool m_layersDirty = false;

    struct SourceDef {
        QString type;
        QJsonObject def;
        QString data;  // geojson text, "" when none yet
    };
    std::map<QString, SourceDef> m_sources;
    std::map<QString, LayerDef> m_layers;
    std::map<QString, QJsonObject> m_images;
    bool m_nativeFeatureState = hasFeatureState();
    QStringList m_order;
    QStringList m_missingAnchors;
};

}  // namespace maps::view
