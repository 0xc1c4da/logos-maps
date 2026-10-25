#include "view/selection_overlay.h"

#include <QMapLibre/Map>
#if __has_include(<QMapLibre/qmaplibre_patchset.h>)
#include <QMapLibre/qmaplibre_patchset.h>
#endif

#include <QJsonArray>
#include <QJsonDocument>

namespace maps::view {

namespace {

const QString kPrefix = QStringLiteral("maps/builtin:");
const QString kSource = kPrefix + QStringLiteral("selection");
const QString kFillLayer = kPrefix + QStringLiteral("selection-fill");
const QString kLineLayer = kPrefix + QStringLiteral("selection-line");
const QString kCircleLayer = kPrefix + QStringLiteral("selection-circle");

QString idOf(const QJsonObject& feature)
{
    if (feature.contains(QStringLiteral("id"))) return feature.value(QStringLiteral("id")).toVariant().toString();
    const QJsonObject props = feature.value(QStringLiteral("properties")).toObject();
    if (props.contains(QStringLiteral("id"))) return props.value(QStringLiteral("id")).toVariant().toString();
    return QString();
}

// Highlight colour: hover is lighter than selection; anything else mirrored
// (custom state keys) gets the selection look.
QVariantList colourExpr(const QString& selected, const QString& hover)
{
    const QVariantList isHover{QStringLiteral("to-boolean"), QVariantList{QStringLiteral("get"), QStringLiteral("hover"), QVariantList{QStringLiteral("get"), QStringLiteral("__state")}}};
    const QVariantList isSelected{QStringLiteral("to-boolean"), QVariantList{QStringLiteral("get"), QStringLiteral("selected"), QVariantList{QStringLiteral("get"), QStringLiteral("__state")}}};
    return {QStringLiteral("case"), isSelected, selected, isHover, hover, selected};
}

}  // namespace

SelectionOverlay::SelectionOverlay(QMapLibre::Map* map, QObject* parent) : QObject(parent), m_map(map) {}

void SelectionOverlay::setMap(QMapLibre::Map* map)
{
    m_map = map;
    m_installed = false;
}

QString SelectionOverlay::sourceId() const { return kSource; }

QStringList SelectionOverlay::layerIds() const { return {kFillLayer, kLineLayer, kCircleLayer}; }

void SelectionOverlay::apply(const QString& sourceId, const QString& featureId, const QJsonValue& state)
{
    const Key key{sourceId, featureId};
    if (state.isNull() || state.isUndefined()) {
        m_entries.erase(key);
    } else {
        Entry& e = m_entries[key];
        const QJsonObject add = state.toObject();
        for (auto it = add.begin(); it != add.end(); ++it) e.state.insert(it.key(), it.value());
        if (!e.resolved && m_styleReady) resolve(sourceId, e, featureId);
    }
    if (!m_styleReady) return;
    if (!m_installed) install();
    else updateSource();
}

QJsonObject SelectionOverlay::stateFor(const QString& sourceId, const QString& featureId) const
{
    const auto it = m_entries.find(Key{sourceId, featureId});
    return it == m_entries.end() ? QJsonObject() : it->second.state;
}

void SelectionOverlay::resolve(const QString& sourceId, Entry& e, const QString& featureId)
{
#ifdef MAPS_ENGINE_HAS_P1_QUERY_RENDERED_FEATURES
    if (!m_map || !m_map->sourceExists(sourceId)) return;
    const QJsonArray features = QJsonDocument::fromJson(m_map->querySourceFeatures(sourceId)).object().value(QStringLiteral("features")).toArray();
    for (const QJsonValue& v : features) {
        const QJsonObject f = v.toObject();
        if (idOf(f) != featureId) continue;
        e.feature = f;
        e.resolved = true;
        return;
    }
#else
    Q_UNUSED(sourceId);
    Q_UNUSED(e);
    Q_UNUSED(featureId);
#endif
}

QJsonObject SelectionOverlay::featureCollection() const
{
    QJsonArray features;
    for (const auto& [key, e] : m_entries) {
        if (!e.resolved) continue;
        QJsonObject props = e.feature.value(QStringLiteral("properties")).toObject();
        props.insert(QStringLiteral("__state"), e.state);
        props.insert(QStringLiteral("__sourceId"), key.first);
        props.insert(QStringLiteral("__featureId"), key.second);
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("Feature")},
                                    {QStringLiteral("properties"), props},
                                    {QStringLiteral("geometry"), e.feature.value(QStringLiteral("geometry"))}});
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("FeatureCollection")}, {QStringLiteral("features"), features}};
}

void SelectionOverlay::install()
{
    if (!m_map || m_installed) return;
    const QByteArray data = QJsonDocument(featureCollection()).toJson(QJsonDocument::Compact);
    if (!m_map->sourceExists(kSource)) m_map->addSource(kSource, {{QStringLiteral("type"), QStringLiteral("geojson")}, {QStringLiteral("data"), data}});
    else m_map->updateSource(kSource, {{QStringLiteral("data"), data}});

    auto add = [this](const QString& id, const QVariantMap& params) {
        if (m_map->layerExists(id)) m_map->removeLayer(id);
        m_map->addLayer(id, params);
    };
    auto geometryIs = [](const char* type) {
        return QVariantList{QStringLiteral("=="), QVariantList{QStringLiteral("geometry-type")}, QString::fromLatin1(type)};
    };
    add(kFillLayer, {{QStringLiteral("type"), QStringLiteral("fill")},
                     {QStringLiteral("source"), kSource},
                     {QStringLiteral("filter"), QVariantList{QStringLiteral("any"), geometryIs("Polygon"), geometryIs("MultiPolygon")}},
                     {QStringLiteral("paint"), QVariantMap{{QStringLiteral("fill-color"), colourExpr(QStringLiteral("rgba(255,149,0,0.35)"), QStringLiteral("rgba(255,149,0,0.18)"))},
                                                           {QStringLiteral("fill-outline-color"), QStringLiteral("#ff9500")}}}});
    add(kLineLayer, {{QStringLiteral("type"), QStringLiteral("line")},
                     {QStringLiteral("source"), kSource},
                     {QStringLiteral("filter"), QVariantList{QStringLiteral("any"), geometryIs("LineString"), geometryIs("MultiLineString"), geometryIs("Polygon"), geometryIs("MultiPolygon")}},
                     {QStringLiteral("paint"), QVariantMap{{QStringLiteral("line-color"), colourExpr(QStringLiteral("#ff9500"), QStringLiteral("rgba(255,149,0,0.6)"))},
                                                           {QStringLiteral("line-width"), 4}}}});
    add(kCircleLayer, {{QStringLiteral("type"), QStringLiteral("circle")},
                       {QStringLiteral("source"), kSource},
                       {QStringLiteral("filter"), QVariantList{QStringLiteral("any"), geometryIs("Point"), geometryIs("MultiPoint")}},
                       {QStringLiteral("paint"), QVariantMap{{QStringLiteral("circle-radius"), 14},
                                                             {QStringLiteral("circle-color"), QStringLiteral("rgba(0,0,0,0)")},
                                                             {QStringLiteral("circle-stroke-width"), 3},
                                                             {QStringLiteral("circle-stroke-color"), colourExpr(QStringLiteral("#ff9500"), QStringLiteral("rgba(255,149,0,0.6)"))}}}});
    m_installed = true;
}

void SelectionOverlay::uninstall()
{
    if (!m_map) return;
    for (const QString& id : layerIds()) if (m_map->layerExists(id)) m_map->removeLayer(id);
    if (m_map->sourceExists(kSource)) m_map->removeSource(kSource);
    m_installed = false;
}

void SelectionOverlay::updateSource()
{
    if (!m_map || !m_installed) return;
    m_map->updateSource(kSource, {{QStringLiteral("data"), QJsonDocument(featureCollection()).toJson(QJsonDocument::Compact)}});
}

void SelectionOverlay::onStyleLoaded()
{
    m_styleReady = true;
    m_installed = false;  // the new style has none of it
    for (auto& [key, e] : m_entries) {
        e.resolved = false;
        resolve(key.first, e, key.second);
    }
    if (!m_entries.empty()) install();
}

void SelectionOverlay::onSceneLayersReconciled()
{
    if (!m_map || !m_styleReady || m_entries.empty()) return;
    // Scene layers were appended above us; go back on top. Scene sources
    // may also have (re)appeared, so unresolved entries get another try.
    for (auto& [key, e] : m_entries) if (!e.resolved) resolve(key.first, e, key.second);
    uninstall();
    install();
}

void SelectionOverlay::onSourceUpdated(const QString& sourceId)
{
    if (!m_styleReady) return;
    bool changed = false;
    for (auto& [key, e] : m_entries) {
        if (key.first != sourceId) continue;
        e.resolved = false;
        resolve(key.first, e, key.second);
        changed = true;
    }
    if (!changed) return;
    if (!m_installed) install();
    else updateSource();
}

}  // namespace maps::view
