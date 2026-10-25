#include "view/builtins.h"

#include <QMapLibre/Map>

#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <cmath>

namespace maps::view {

namespace {

const QString kPrefix = QStringLiteral("maps/builtin:");
const QString kSource = kPrefix + QStringLiteral("puck");
const QString kAccuracyLayer = kPrefix + QStringLiteral("puck-accuracy");
const QString kHaloLayer = kPrefix + QStringLiteral("puck-halo");
const QString kDotLayer = kPrefix + QStringLiteral("puck-dot");
const QString kHeadingLayer = kPrefix + QStringLiteral("puck-heading");
const QString kHeadingImage = kPrefix + QStringLiteral("heading");

QImage headingImage()
{
    // A 48 px chevron pointing up, drawn once; icon-rotate turns it.
    QImage img(48, 48, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.moveTo(24, 2);
    path.lineTo(36, 22);
    path.lineTo(24, 16);
    path.lineTo(12, 22);
    path.closeSubpath();
    p.fillPath(path, QColor(0, 122, 255));
    p.setPen(QPen(Qt::white, 1.5));
    p.drawPath(path);
    return img;
}

// Geodesic-ish circle of `radiusM` around (lon, lat), 48 vertices.
QJsonArray circleRing(double lon, double lat, double radiusM)
{
    QJsonArray ring;
    const double dLat = radiusM / 111320.0;
    const double dLon = radiusM / (111320.0 * std::max(0.01, std::cos(qDegreesToRadians(lat))));
    for (int i = 0; i <= 48; ++i) {
        const double a = 2.0 * M_PI * i / 48.0;
        ring.append(QJsonArray{lon + dLon * std::cos(a), lat + dLat * std::sin(a)});
    }
    return ring;
}

}  // namespace

Builtins::Builtins(QMapLibre::Map* map, QObject* parent) : QObject(parent), m_map(map) {}

void Builtins::setMap(QMapLibre::Map* map)
{
    m_map = map;
    m_installed = false;
}

QString Builtins::sourceId() const { return kSource; }

QStringList Builtins::layerIds() const { return {kAccuracyLayer, kHaloLayer, kDotLayer, kHeadingLayer}; }

void Builtins::setLocation(const QJsonValue& location)
{
    m_location = location.isObject() ? location : QJsonValue();
    if (!m_styleReady) return;
    if (!m_installed) install();
    else updateSource();
}

void Builtins::setPuckOptions(const QJsonObject& options)
{
    if (options.value(QStringLiteral("accuracyThreshold")).isDouble()) m_accuracyThreshold = options.value(QStringLiteral("accuracyThreshold")).toDouble();
    if (options.value(QStringLiteral("visible")).isBool()) m_visible = options.value(QStringLiteral("visible")).toBool();
    if (m_styleReady) {
        if (!m_installed) install();
        else updateSource();
    }
}

QJsonObject Builtins::featureCollection() const
{
    QJsonArray features;
    if (m_visible && m_location.isObject()) {
        const QJsonObject loc = m_location.toObject();
        const double lon = loc.value(QStringLiteral("lon")).toDouble();
        const double lat = loc.value(QStringLiteral("lat")).toDouble();
        const double accuracy = loc.value(QStringLiteral("accuracy")).toDouble(0.0);
        QJsonObject props{{QStringLiteral("accuracy"), accuracy}};
        if (loc.value(QStringLiteral("heading")).isDouble()) props.insert(QStringLiteral("heading"), loc.value(QStringLiteral("heading")).toDouble());
        if (accuracy > m_accuracyThreshold) {
            features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("Feature")},
                                        {QStringLiteral("properties"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("accuracy")}}},
                                        {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Polygon")},
                                                                                 {QStringLiteral("coordinates"), QJsonArray{circleRing(lon, lat, accuracy)}}}}});
        }
        features.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("Feature")},
                                    {QStringLiteral("properties"), props},
                                    {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("type"), QStringLiteral("Point")},
                                                                             {QStringLiteral("coordinates"), QJsonArray{lon, lat}}}}});
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("FeatureCollection")}, {QStringLiteral("features"), features}};
}

void Builtins::install()
{
    if (!m_map || m_installed) return;
    const QByteArray data = QJsonDocument(featureCollection()).toJson(QJsonDocument::Compact);
    if (!m_map->sourceExists(kSource)) m_map->addSource(kSource, {{QStringLiteral("type"), QStringLiteral("geojson")}, {QStringLiteral("data"), data}});
    else m_map->updateSource(kSource, {{QStringLiteral("data"), data}});
    m_map->addImage(kHeadingImage, headingImage());

    auto add = [this](const QString& id, const QVariantMap& params) {
        if (m_map->layerExists(id)) m_map->removeLayer(id);
        m_map->addLayer(id, params);
    };
    add(kAccuracyLayer, {{QStringLiteral("type"), QStringLiteral("fill")},
                         {QStringLiteral("source"), kSource},
                         {QStringLiteral("filter"), QVariantList{QStringLiteral("=="), QVariantList{QStringLiteral("geometry-type")}, QStringLiteral("Polygon")}},
                         {QStringLiteral("paint"), QVariantMap{{QStringLiteral("fill-color"), QStringLiteral("rgba(0,122,255,0.15)")},
                                                               {QStringLiteral("fill-outline-color"), QStringLiteral("rgba(0,122,255,0.4)")}}}});
    const QVariantList pointFilter{QStringLiteral("=="), QVariantList{QStringLiteral("geometry-type")}, QStringLiteral("Point")};
    add(kHaloLayer, {{QStringLiteral("type"), QStringLiteral("circle")},
                     {QStringLiteral("source"), kSource},
                     {QStringLiteral("filter"), pointFilter},
                     {QStringLiteral("paint"), QVariantMap{{QStringLiteral("circle-radius"), 12}, {QStringLiteral("circle-color"), QStringLiteral("#ffffff")},
                                                           {QStringLiteral("circle-opacity"), 0.9}, {QStringLiteral("circle-pitch-alignment"), QStringLiteral("map")}}}});
    add(kDotLayer, {{QStringLiteral("type"), QStringLiteral("circle")},
                    {QStringLiteral("source"), kSource},
                    {QStringLiteral("filter"), pointFilter},
                    {QStringLiteral("paint"), QVariantMap{{QStringLiteral("circle-radius"), 8}, {QStringLiteral("circle-color"), QStringLiteral("#007aff")},
                                                          {QStringLiteral("circle-pitch-alignment"), QStringLiteral("map")}}}});
    add(kHeadingLayer, {{QStringLiteral("type"), QStringLiteral("symbol")},
                        {QStringLiteral("source"), kSource},
                        {QStringLiteral("filter"), QVariantList{QStringLiteral("all"), pointFilter, QVariantList{QStringLiteral("has"), QStringLiteral("heading")}}},
                        {QStringLiteral("layout"), QVariantMap{{QStringLiteral("icon-image"), kHeadingImage},
                                                               {QStringLiteral("icon-rotate"), QVariantList{QStringLiteral("get"), QStringLiteral("heading")}},
                                                               {QStringLiteral("icon-rotation-alignment"), QStringLiteral("map")},
                                                               {QStringLiteral("icon-pitch-alignment"), QStringLiteral("map")},
                                                               {QStringLiteral("icon-allow-overlap"), true},
                                                               {QStringLiteral("icon-ignore-placement"), true},
                                                               {QStringLiteral("icon-offset"), QVariantList{0, -14}},
                                                               {QStringLiteral("icon-size"), 0.6}}}});
    m_installed = true;
}

void Builtins::uninstall()
{
    if (!m_map) return;
    for (const QString& id : layerIds()) if (m_map->layerExists(id)) m_map->removeLayer(id);
    if (m_map->sourceExists(kSource)) m_map->removeSource(kSource);
    m_installed = false;
}

void Builtins::updateSource()
{
    if (!m_map || !m_installed) return;
    m_map->updateSource(kSource, {{QStringLiteral("data"), QJsonDocument(featureCollection()).toJson(QJsonDocument::Compact)}});
}

void Builtins::onStyleLoaded()
{
    m_styleReady = true;
    m_installed = false;  // the new style has none of it
    install();
}

void Builtins::onSceneLayersReconciled()
{
    if (!m_map || !m_styleReady) return;
    // Scene layers were appended above us; put the puck back on top.
    uninstall();
    install();
}

}  // namespace maps::view
