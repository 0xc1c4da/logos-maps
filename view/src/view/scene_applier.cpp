#include "view/scene_applier.h"

#include "view/capabilities.h"

#include <QMapLibre/Map>
#if __has_include(<QMapLibre/qmaplibre_patchset.h>)
#include <QMapLibre/qmaplibre_patchset.h>
#endif

#include <QImage>
#include <QJsonDocument>
#include <QVariantMap>

namespace maps::view {

namespace {

QString compact(const QJsonValue& v)
{
    if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    return v.toString();
}

const QString kEmptyCollection = QStringLiteral("{\"type\":\"FeatureCollection\",\"features\":[]}");

// The binding's conversion treats a QString `data` as a URL and only parses
// inline GeoJSON from a QByteArray (conversion_p.hpp); hand it the right one.
QVariant geoJsonData(const QString& data)
{
    const QString trimmed = data.trimmed();
    if (trimmed.startsWith(QLatin1Char('{')) || trimmed.startsWith(QLatin1Char('['))) return trimmed.toUtf8();
    return data;
}

}  // namespace

SceneApplier::SceneApplier(QMapLibre::Map* map, CoreClient* core, QObject* parent)
    : QObject(parent), m_map(map), m_core(core) {}

QStringList SceneApplier::knownSources() const
{
    QStringList out;
    for (const auto& [id, s] : m_sources) out << id;
    return out;
}

std::vector<SceneApplier::LayerInfo> SceneApplier::layerInfos() const
{
    std::vector<LayerInfo> out;
    auto push = [&](const QString& id) {
        auto it = m_layers.find(id);
        if (it == m_layers.end()) return;
        LayerInfo info;
        info.id = id;
        info.source = it->second.source;
        info.sourceLayer = it->second.params.value(QStringLiteral("source-layer")).toString();
        info.type = it->second.params.value(QStringLiteral("type")).toString();
        out.push_back(std::move(info));
    };
    for (const QString& id : m_order) push(id);
    for (const auto& [id, l] : m_layers) {
        if (!m_order.contains(id)) push(id);
    }
    return out;
}

QStringList SceneApplier::knownLayers() const
{
    QStringList out;
    for (const auto& [id, l] : m_layers) out << id;
    return out;
}

// ── lifecycle hooks ──────────────────────────────────────────────────────

void SceneApplier::onStyleWillLoad()
{
    m_blocked = true;
    // The engine drops every source/layer/image with the old style; our
    // mirror stays so the replay can restore it even if the core is slow.
}

void SceneApplier::onStyleLoaded()
{
    m_blocked = false;
    replay();
}

void SceneApplier::onFrameFullyRendered()
{
    if (!m_renderPending) return;
    const quint64 rev = *m_renderPending;
    m_renderPending.reset();
    emit frameRendered(rev);
    if (m_core && !m_viewId.isEmpty()) {
        const QJsonObject payload{{QStringLiteral("v"), 1}, {QStringLiteral("revision"), static_cast<double>(rev)}};
        m_core->reportViewEvent(m_viewId, QStringLiteral("frameRendered"), compact(payload), [](const CoreReply&) {});
    }
}

void SceneApplier::onRevisionPublished(const QJsonObject& payload)
{
    const quint64 rev = static_cast<quint64>(payload.value(QStringLiteral("revision")).toDouble());
    if (rev != 0 && rev <= m_appliedRevision) return;
    pullChanges();
}

void SceneApplier::onFeatureStateChanged(const QJsonObject& payload)
{
    const QString sourceId = payload.value(QStringLiteral("sourceId")).toString();
    const QJsonValue featureIdV = payload.value(QStringLiteral("featureId"));
    const QJsonValue stateV = payload.value(QStringLiteral("state"));
    const QString featureId = featureIdV.isNull() ? QString() : featureIdV.toVariant().toString();
    if (!m_nativeFeatureState || !hasFeatureState() || !m_map) {
        emit featureStateFallback(sourceId, featureId, stateV);
        return;
    }
#ifdef MAPS_ENGINE_HAS_P2_FEATURE_STATE
    if (stateV.isNull() || stateV.isUndefined()) {
        m_map->removeFeatureState(sourceId, QString(), featureId, QString());
    } else {
        m_map->setFeatureState(sourceId, QString(), featureId, stateV.toObject().toVariantMap());
    }
#endif
}

// ── fetching ─────────────────────────────────────────────────────────────

void SceneApplier::pullChanges()
{
    if (!m_core) return;
    if (m_blocked || m_inFlight || !m_map) {
        m_pendingPull = true;
        return;
    }
    m_inFlight = true;
    m_pendingPull = false;
    m_core->getChanges(m_appliedRevision, [this](const CoreReply& reply) {
        if (!reply.ok) {
            m_inFlight = false;
            emit sceneError(reply.code(), reply.error);
            return;
        }
        applyChanges(reply.object());
    });
}

void SceneApplier::replay()
{
    if (!m_core) return;
    if (m_blocked || !m_map) {
        m_pendingPull = true;
        return;
    }
    if (m_inFlight) {
        m_pendingPull = true;
        return;
    }
    m_inFlight = true;
    m_pendingPull = false;
    m_core->getScene([this](const CoreReply& reply) {
        if (!reply.ok) {
            m_inFlight = false;
            emit sceneError(reply.code(), reply.error);
            return;
        }
        applyScene(reply.object());
    });
}

// ── application ──────────────────────────────────────────────────────────

void SceneApplier::applyScene(const QJsonObject& scene)
{
    const quint64 revision = static_cast<quint64>(scene.value(QStringLiteral("revision")).toDouble());

    // Start from nothing: a replay follows a style load that dropped
    // everything, a full change set follows a log-window miss.
    if (m_map) {
        removeSceneLayers();
        for (const auto& [id, s] : m_sources) if (m_map->sourceExists(id)) m_map->removeSource(id);
        for (const auto& [id, img] : m_images) m_map->removeImage(id);
    }
    m_sources.clear();
    m_layers.clear();
    m_images.clear();

    // M6.5 order: sources, data, images, then layers (ordered by the core).
    const QJsonObject sessions = scene.value(QStringLiteral("sessions")).toObject();
    for (auto s = sessions.begin(); s != sessions.end(); ++s) {
        const QJsonObject session = s.value().toObject();
        const QJsonObject sources = session.value(QStringLiteral("sources")).toObject();
        for (auto it = sources.begin(); it != sources.end(); ++it) {
            QJsonObject op = it.value().toObject();
            op.insert(QStringLiteral("op"), QStringLiteral("putSource"));
            putSource(op);
            if (op.value(QStringLiteral("type")).toString() == QLatin1String("geojson") && op.contains(QStringLiteral("data"))) {
                replaceGeoJson(QJsonObject{{QStringLiteral("sourceId"), it.key()}, {QStringLiteral("data"), op.value(QStringLiteral("data"))}});
            }
        }
        const QJsonObject images = session.value(QStringLiteral("images")).toObject();
        for (auto it = images.begin(); it != images.end(); ++it) putImage(it.value().toObject());
    }
    for (auto s = sessions.begin(); s != sessions.end(); ++s) {
        const QJsonArray layers = s.value().toObject().value(QStringLiteral("layers")).toArray();
        for (const QJsonValue& l : layers) putLayer(l.toObject());
    }
    m_layersDirty = true;
    reconcileLayers([this, revision] { finishRevision(revision); });
}

void SceneApplier::applyChanges(const QJsonObject& changes)
{
    if (changes.value(QStringLiteral("full")).toBool()) {
        QJsonObject scene = changes.value(QStringLiteral("scene")).toObject();
        if (!scene.contains(QStringLiteral("revision"))) scene.insert(QStringLiteral("revision"), changes.value(QStringLiteral("to")));
        applyScene(scene);
        return;
    }
    const quint64 to = static_cast<quint64>(changes.value(QStringLiteral("to")).toDouble());
    const QJsonArray ops = changes.value(QStringLiteral("ops")).toArray();
    for (const QJsonValue& v : ops) applyOp(v.toObject());
    if (m_layersDirty) {
        reconcileLayers([this, to] { finishRevision(to); });
    } else {
        finishRevision(to);
    }
}

void SceneApplier::applyOp(const QJsonObject& op)
{
    const QString kind = op.value(QStringLiteral("op")).toString();
    const QString id = op.value(QStringLiteral("id")).toString();
    if (kind == QLatin1String("removeLayer")) {
        if (m_map && m_map->layerExists(id)) m_map->removeLayer(id);
        m_layers.erase(id);
        m_order.removeAll(id);
        m_layersDirty = true;
    } else if (kind == QLatin1String("removeSource")) {
        // Layers on it were removed by preceding ops (core ordering); be safe.
        for (auto it = m_layers.begin(); it != m_layers.end();) {
            if (it->second.source == id) {
                if (m_map && m_map->layerExists(it->first)) m_map->removeLayer(it->first);
                m_order.removeAll(it->first);
                it = m_layers.erase(it);
                m_layersDirty = true;
            } else {
                ++it;
            }
        }
        if (m_map && m_map->sourceExists(id)) m_map->removeSource(id);
        m_sources.erase(id);
    } else if (kind == QLatin1String("removeImage")) {
        if (m_map) m_map->removeImage(id);
        m_images.erase(id);
    } else if (kind == QLatin1String("putSource")) {
        putSource(op);
    } else if (kind == QLatin1String("replaceGeoJson")) {
        replaceGeoJson(op);
    } else if (kind == QLatin1String("putImage")) {
        putImage(op);
    } else if (kind == QLatin1String("putLayer")) {
        putLayer(op);
    } else {
        emit sceneError(QStringLiteral("maps.badArgument"), QStringLiteral("unknown op '%1'").arg(kind));
    }
}

QVariantMap SceneApplier::sourceParams(const QString& id) const
{
    auto it = m_sources.find(id);
    if (it == m_sources.end()) return {};
    QVariantMap params = it->second.def.toVariantMap();
    params.insert(QStringLiteral("type"), it->second.type);
    if (it->second.type == QLatin1String("geojson")) {
        if (!it->second.data.isEmpty()) {
            params.insert(QStringLiteral("data"), geoJsonData(it->second.data));
        } else if (params.contains(QStringLiteral("data"))) {
            params.insert(QStringLiteral("data"), geoJsonData(compact(it->second.def.value(QStringLiteral("data")))));
        } else {
            params.insert(QStringLiteral("data"), geoJsonData(kEmptyCollection));
        }
    }
    return params;
}

void SceneApplier::putSource(const QJsonObject& op)
{
    const QString id = op.value(QStringLiteral("id")).toString();
    SourceDef def;
    def.type = op.value(QStringLiteral("type")).toString();
    def.def = op.value(QStringLiteral("def")).toObject();
    auto existing = m_sources.find(id);
    if (existing != m_sources.end()) def.data = existing->second.data;
    if (op.contains(QStringLiteral("data")) && !op.value(QStringLiteral("data")).isNull())
        def.data = compact(op.value(QStringLiteral("data")));

    // A definition change recreates the source (M6.3); layers on it come
    // back with the reconcile that follows.
    if (m_map && m_map->sourceExists(id)) {
        for (const auto& [lid, l] : m_layers) {
            if (l.source == id && m_map->layerExists(lid)) m_map->removeLayer(lid);
        }
        m_map->removeSource(id);
        m_layersDirty = true;
    }
    m_sources[id] = std::move(def);
    if (m_map) m_map->addSource(id, sourceParams(id));
    emit sourceUpdated(id);
}

void SceneApplier::replaceGeoJson(const QJsonObject& op)
{
    const QString id = op.value(QStringLiteral("sourceId")).toString();
    const QString data = compact(op.value(QStringLiteral("data")));
    auto it = m_sources.find(id);
    if (it == m_sources.end()) {
        // Data before definition: keep it for the definition that follows.
        SourceDef def;
        def.type = QStringLiteral("geojson");
        def.data = data;
        m_sources[id] = std::move(def);
        it = m_sources.find(id);
        if (m_map && !m_map->sourceExists(id)) m_map->addSource(id, sourceParams(id));
        return;
    }
    it->second.data = data;
    if (!m_map) return;
    if (m_map->sourceExists(id)) {
        m_map->updateSource(id, QVariantMap{{QStringLiteral("data"), geoJsonData(data)}});
    } else {
        m_map->addSource(id, sourceParams(id));
    }
    emit sourceUpdated(id);
}

void SceneApplier::putImage(const QJsonObject& op)
{
    const QString id = op.value(QStringLiteral("id")).toString();
    m_images[id] = op;
    if (!m_map) return;
    const QByteArray png = QByteArray::fromBase64(op.value(QStringLiteral("bytes")).toString().toLatin1());
    const QImage image = QImage::fromData(png);
    if (image.isNull()) {
        emit sceneError(QStringLiteral("maps.badArgument"), QStringLiteral("image '%1' is not a decodable PNG").arg(id));
        return;
    }
#ifdef MAPS_ENGINE_HAS_P4_ADD_IMAGE_METADATA
    const qreal pixelRatio = op.value(QStringLiteral("pixelRatio")).toDouble(1.0);
    const bool sdf = op.value(QStringLiteral("sdf")).toBool(false);
    auto pairs = [](const QJsonValue& v) {
        QList<QPair<float, float>> out;
        for (const QJsonValue& p : v.toArray()) {
            const QJsonArray a = p.toArray();
            if (a.size() == 2) out.append({static_cast<float>(a[0].toDouble()), static_cast<float>(a[1].toDouble())});
        }
        return out;
    };
    QRectF content;
    const QJsonArray c = op.value(QStringLiteral("content")).toArray();
    if (c.size() == 4) content = QRectF(QPointF(c[0].toDouble(), c[1].toDouble()), QPointF(c[2].toDouble(), c[3].toDouble()));
    m_map->addImage(id, image, pixelRatio, sdf, pairs(op.value(QStringLiteral("stretchX"))),
                    pairs(op.value(QStringLiteral("stretchY"))), content);
#else
    // M5.2: pixel ratio fixed at 1.0 without P4; images must be pre-scaled.
    m_map->addImage(id, image);
#endif
}

void SceneApplier::putLayer(const QJsonObject& op)
{
    const QString id = op.value(QStringLiteral("id")).toString();
    LayerDef def;
    def.source = op.value(QStringLiteral("source")).toString();
    QJsonObject params;
    params.insert(QStringLiteral("type"), op.value(QStringLiteral("type")));
    if (!def.source.isEmpty()) params.insert(QStringLiteral("source"), def.source);
    if (op.contains(QStringLiteral("sourceLayer"))) params.insert(QStringLiteral("source-layer"), op.value(QStringLiteral("sourceLayer")));
    if (op.contains(QStringLiteral("minzoom"))) params.insert(QStringLiteral("minzoom"), op.value(QStringLiteral("minzoom")));
    if (op.contains(QStringLiteral("maxzoom"))) params.insert(QStringLiteral("maxzoom"), op.value(QStringLiteral("maxzoom")));
    if (op.contains(QStringLiteral("filter")) && !op.value(QStringLiteral("filter")).isNull())
        params.insert(QStringLiteral("filter"), op.value(QStringLiteral("filter")));
    params.insert(QStringLiteral("layout"), op.value(QStringLiteral("layout")).toObject());
    params.insert(QStringLiteral("paint"), op.value(QStringLiteral("paint")).toObject());
    def.params = params;
    m_layers[id] = std::move(def);
    m_layersDirty = true;
}

// ── layer order (M6.10) ──────────────────────────────────────────────────

void SceneApplier::removeSceneLayers()
{
    if (!m_map) return;
    for (const auto& [id, l] : m_layers) {
        if (m_map->layerExists(id)) m_map->removeLayer(id);
    }
}

void SceneApplier::insertLayers(const QJsonArray& resolved)
{
    if (!m_map) return;
    removeSceneLayers();
    m_order.clear();
    for (const QJsonValue& v : resolved) {
        const QJsonObject r = v.toObject();
        const QString id = r.value(QStringLiteral("layerId")).toString();
        auto it = m_layers.find(id);
        if (it == m_layers.end()) continue;  // the core knows a layer we have not received yet
        const QString before = r.value(QStringLiteral("before")).toString();
        // Inserting each layer before the same anchor, bottom-to-top, yields
        // the resolved order; an empty `before` appends (top).
        m_map->addLayer(id, it->second.params.toVariantMap(),
                        (!before.isEmpty() && m_map->layerExists(before)) ? before : QString());
        m_order << id;
    }
}

void SceneApplier::reconcileLayers(std::function<void()> then)
{
    if (!m_core || m_viewId.isEmpty() || !m_map) {
        // No order authority (tests without a core): session/batch order.
        QJsonArray resolved;
        for (const auto& [id, l] : m_layers) resolved.append(QJsonObject{{QStringLiteral("layerId"), id}, {QStringLiteral("before"), QString()}});
        insertLayers(resolved);
        m_layersDirty = false;
        emit layersReconciled(m_order, m_missingAnchors);
        then();
        return;
    }
    m_core->getLayerOrder(m_viewId, [this, then](const CoreReply& reply) {
        if (!reply.ok) {
            emit sceneError(reply.code(), reply.error);
            then();
            return;
        }
        const QJsonObject order = reply.object();
        insertLayers(order.value(QStringLiteral("layers")).toArray());
        m_missingAnchors.clear();
        for (const QJsonValue& a : order.value(QStringLiteral("missingAnchors")).toArray()) m_missingAnchors << a.toString();
        m_layersDirty = false;
        emit layersReconciled(m_order, m_missingAnchors);
        then();
    });
}

void SceneApplier::finishRevision(quint64 revision)
{
    m_inFlight = false;
    if (revision > m_appliedRevision) m_appliedRevision = revision;
    m_renderPending = m_appliedRevision;
    emit revisionApplied(m_appliedRevision);
    if (m_core && !m_viewId.isEmpty()) {
        m_core->ackRevision(m_viewId, m_appliedRevision, [this](const CoreReply& reply) {
            if (!reply.ok) emit sceneError(reply.code(), reply.error);
        });
    }
    if (m_pendingPull) pullChanges();
}

}  // namespace maps::view
