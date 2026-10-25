#include "input/picker.h"

#include <QMapLibre/Map>
#include <QMapLibre/Types>
#if __has_include(<QMapLibre/qmaplibre_patchset.h>)
#include <QMapLibre/qmaplibre_patchset.h>
#endif

#include <QJsonDocument>
#include <QLineF>
#include <QRectF>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace maps::view {

namespace {

QString sessionOf(const QString& layerId, bool sceneLayer)
{
    // Scene ids are "<session>:<local-id>" with S = <module>/<name> (M6.1);
    // basemap layers have no session, whatever their id looks like.
    if (!sceneLayer) return QString();
    const int colon = layerId.indexOf(QLatin1Char(':'));
    return colon <= 0 ? QString() : layerId.left(colon);
}

QJsonValue featureIdOf(const QJsonObject& feature, const QJsonObject& props)
{
    if (feature.contains(QStringLiteral("id"))) return feature.value(QStringLiteral("id"));
    // promoteId'd sources surface the id as a property for picking (M6.2).
    if (props.contains(QStringLiteral("id"))) return props.value(QStringLiteral("id"));
    return QJsonValue();
}

}  // namespace

Picker::Picker(QMapLibre::Map* map, QObject* parent) : QObject(parent), m_map(map) {}

void Picker::setMap(QMapLibre::Map* map)
{
    m_map = map;
    m_tableDirty = true;
}

void Picker::setStyleLoaded(bool loaded)
{
    m_styleLoaded = loaded;
    m_tableDirty = true;
}

void Picker::setSceneLayers(std::vector<SceneLayer> layers)
{
    m_sceneLayers = std::move(layers);
    m_tableDirty = true;
}

bool Picker::renderedQueriesAvailable()
{
#ifdef MAPS_ENGINE_HAS_P1_QUERY_RENDERED_FEATURES
    return true;
#else
    return false;
#endif
}

void Picker::ensureLayerTable()
{
    if (!m_tableDirty || !m_map) return;
    m_tableDirty = false;
    m_layers.clear();
    const QJsonObject style = QJsonDocument::fromJson(m_map->styleJson().toUtf8()).object();
    for (const QJsonValue& v : style.value(QStringLiteral("layers")).toArray()) {
        const QJsonObject l = v.toObject();
        StyleLayer sl;
        sl.id = l.value(QStringLiteral("id")).toString();
        sl.source = l.value(QStringLiteral("source")).toString();
        sl.sourceLayer = l.value(QStringLiteral("source-layer")).toString();
        sl.type = l.value(QStringLiteral("type")).toString();
        if (!sl.id.isEmpty() && sl.type != QLatin1String("background")) m_layers.push_back(sl);
    }
    for (const SceneLayer& s : m_sceneLayers) {
        const bool known = std::any_of(m_layers.begin(), m_layers.end(), [&](const StyleLayer& l) { return l.id == s.id; });
        if (!known) m_layers.push_back(StyleLayer{s.id, s.source, s.sourceLayer, s.type, true});
    }
}

double Picker::distanceTo(const QJsonObject& geometry, const QPointF& px) const
{
    if (!m_map) return 0.0;
    const QString type = geometry.value(QStringLiteral("type")).toString();
    auto toPx = [this](const QJsonArray& lonLat) {
        return m_map->pixelForCoordinate(QMapLibre::Coordinate(lonLat[1].toDouble(), lonLat[0].toDouble()));
    };
    if (type == QLatin1String("Point")) {
        return QLineF(px, toPx(geometry.value(QStringLiteral("coordinates")).toArray())).length();
    }
    if (type == QLatin1String("MultiPoint")) {
        double best = 1e9;
        for (const QJsonValue& c : geometry.value(QStringLiteral("coordinates")).toArray()) best = std::min(best, QLineF(px, toPx(c.toArray())).length());
        return best;
    }
    // Lines and polygons under the box: the finger is on them.
    return 0.0;
}

QJsonValue Picker::cappedProperties(const QJsonValue& props) const
{
    if (!props.isObject()) return props;
    const QByteArray bytes = QJsonDocument(props.toObject()).toJson(QJsonDocument::Compact);
    if (bytes.size() <= m_propertyCap) return props;
    QJsonObject out;
    int used = 2;
    const QJsonObject in = props.toObject();
    for (auto it = in.begin(); it != in.end(); ++it) {
        const int cost = QJsonDocument(QJsonObject{{it.key(), it.value()}}).toJson(QJsonDocument::Compact).size();
        if (used + cost > m_propertyCap) break;
        out.insert(it.key(), it.value());
        used += cost;
    }
    out.insert(QStringLiteral("_truncated"), true);
    return out;
}

QJsonArray Picker::hitsFor(const QPointF& px)
{
    QJsonArray hits;
#ifdef MAPS_ENGINE_HAS_P1_QUERY_RENDERED_FEATURES
    ensureLayerTable();
    const double half = m_boxDp / 2.0;
    const QRectF box(px.x() - half, px.y() - half, m_boxDp, m_boxDp);
    const QJsonArray all = QJsonDocument::fromJson(m_map->queryRenderedFeatures(box)).object().value(QStringLiteral("features")).toArray();
    if (all.isEmpty()) return hits;

    struct Hit {
        QJsonObject json;
        double distance;
        int priority;
    };
    std::vector<Hit> found;
    // Resolve each feature's style layer: candidates draw the same
    // source(-layer); when several do, ask the renderer per candidate.
    std::map<QString, QJsonArray> perLayerCache;
    // The renderer answers once per layer that draws a feature, without
    // saying which; collapse those to one feature before resolving layers.
    std::vector<QJsonObject> unique;
    for (const QJsonValue& fv : all) {
        const QJsonObject f = fv.toObject();
        const bool dup = std::any_of(unique.begin(), unique.end(), [&](const QJsonObject& u) {
            return u.value(QStringLiteral("source")) == f.value(QStringLiteral("source")) && u.value(QStringLiteral("sourceLayer")) == f.value(QStringLiteral("sourceLayer")) &&
                   u.value(QStringLiteral("id")) == f.value(QStringLiteral("id")) && u.value(QStringLiteral("geometry")) == f.value(QStringLiteral("geometry")) &&
                   u.value(QStringLiteral("properties")) == f.value(QStringLiteral("properties"));
        });
        if (!dup) unique.push_back(f);
    }
    for (const QJsonObject& f : unique) {
        const QString source = f.value(QStringLiteral("source")).toString();
        if (!m_overlaySource.isEmpty() && source == m_overlaySource) continue;  // the view's own highlight mirror
        const QString sourceLayer = f.value(QStringLiteral("sourceLayer")).toString();
        const QJsonObject props = f.value(QStringLiteral("properties")).toObject();
        const QJsonValue id = featureIdOf(f, props);
        // Feature state: native (P2) comes back on the feature; the overlay
        // fallback keeps it in the view.
        QJsonObject state = f.value(QStringLiteral("state")).toObject();
        if (state.isEmpty() && m_stateResolver && !id.isNull() && !id.isUndefined()) state = m_stateResolver(source, id.toVariant().toString());
        QStringList candidates;
        std::map<QString, bool> isScene;
        for (const StyleLayer& l : m_layers) {
            if (l.source == source && l.sourceLayer == sourceLayer) {
                candidates << l.id;
                isScene[l.id] = l.scene;
            }
        }
        QStringList layers;
        if (candidates.size() <= 1) {
            layers = candidates;
        } else {
            for (const QString& c : candidates) {
                if (!perLayerCache.count(c)) perLayerCache[c] = QJsonDocument::fromJson(m_map->queryRenderedFeatures(box, QStringList{c})).object().value(QStringLiteral("features")).toArray();
                for (const QJsonValue& pv : perLayerCache[c]) {
                    const QJsonObject p = pv.toObject();
                    if (featureIdOf(p, p.value(QStringLiteral("properties")).toObject()) == id && p.value(QStringLiteral("geometry")) == f.value(QStringLiteral("geometry"))) {
                        layers << c;
                        break;
                    }
                }
            }
        }
        if (layers.isEmpty()) layers << QString();
        const double distance = distanceTo(f.value(QStringLiteral("geometry")).toObject(), px);
        for (const QString& layerId : layers) {
            Hit h;
            h.json = QJsonObject{
                {QStringLiteral("session"), sessionOf(layerId, isScene[layerId])},
                {QStringLiteral("layerId"), layerId},
                {QStringLiteral("sourceId"), source},
                {QStringLiteral("sourceLayer"), sourceLayer},
                {QStringLiteral("featureId"), id},
                {QStringLiteral("properties"), cappedProperties(props)},
                {QStringLiteral("state"), state},
                {QStringLiteral("distancePx"), std::round(distance * 100.0) / 100.0},
            };
            h.distance = distance;
            const int pri = m_priority.indexOf(layerId);
            h.priority = pri < 0 ? m_priority.size() : pri;
            found.push_back(std::move(h));
        }
    }
    // Distance first, then the priority list (stable): listed layers come
    // forward in list order, unlisted ones keep their distance order.
    std::stable_sort(found.begin(), found.end(), [](const Hit& a, const Hit& b) { return a.distance < b.distance; });
    std::stable_sort(found.begin(), found.end(), [](const Hit& a, const Hit& b) { return a.priority < b.priority; });
    for (const Hit& h : found) {
        if (hits.size() >= m_maxHits) break;
        hits.append(h.json);
    }
#else
    Q_UNUSED(px);
#endif
    return hits;
}

std::optional<QJsonObject> Picker::pickNow(const QString& kind, const QPointF& px, QString* code, QString* message)
{
    if (!m_map) {
        if (code) *code = QStringLiteral("maps.styleNotLoaded");
        if (message) *message = QStringLiteral("no map");
        return std::nullopt;
    }
    if (!m_styleLoaded) {
        if (code) *code = QStringLiteral("maps.styleNotLoaded");
        if (message) *message = QStringLiteral("pick before the style loaded");
        return std::nullopt;
    }
    const QMapLibre::Coordinate geo = m_map->coordinateForPixel(px);
    QJsonObject payload{
        {QStringLiteral("v"), 1},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("lonLat"), QJsonArray{geo.second, geo.first}},
        {QStringLiteral("px"), QJsonArray{px.x(), px.y()}},
        {QStringLiteral("hits"), renderedQueriesAvailable() ? hitsFor(px) : QJsonArray()},
    };
    if (!renderedQueriesAvailable()) payload.insert(QStringLiteral("rendered"), false);
    return payload;
}

void Picker::pick(const QString& kind, const QPointF& px)
{
    // M8.14: the answer is an event, delivered from the event loop.
    QTimer::singleShot(0, this, [this, kind, px] {
        QString code, message;
        if (auto payload = pickNow(kind, px, &code, &message)) emit picked(*payload);
        else emit failed(code, message);
    });
}

}  // namespace maps::view
