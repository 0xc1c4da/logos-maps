#include "view/camera_intents.h"

#include <QMapLibre/Map>
#include <QMapLibre/Types>

#include <QJsonArray>
#include <QPointF>

#include <algorithm>
#include <cmath>

namespace maps::view {

namespace {

bool nearly(double a, double b) { return std::fabs(a - b) < 1e-9; }

bool readLonLat(const QJsonValue& v, double& lon, double& lat)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 2 || !a[0].isDouble() || !a[1].isDouble()) return false;
    lon = a[0].toDouble();
    lat = a[1].toDouble();
    return std::fabs(lon) <= 180.0 && std::fabs(lat) <= 90.0;
}

QMargins readPadding(const QJsonValue& v, const QMargins& fallback)
{
    if (!v.isObject()) return fallback;
    const QJsonObject p = v.toObject();
    return QMargins(p.value(QStringLiteral("left")).toInt(fallback.left()), p.value(QStringLiteral("top")).toInt(fallback.top()),
                    p.value(QStringLiteral("right")).toInt(fallback.right()), p.value(QStringLiteral("bottom")).toInt(fallback.bottom()));
}

QMapLibre::AnimationOptions readAnimation(const QJsonObject& intent, qint64 defaultMs)
{
    const QJsonObject a = intent.value(QStringLiteral("animation")).toObject();
    QMapLibre::AnimationOptions opts(static_cast<qint64>(a.value(QStringLiteral("duration")).toDouble(static_cast<double>(defaultMs))));
    if (a.contains(QStringLiteral("velocity"))) opts.velocity = a.value(QStringLiteral("velocity")).toDouble();
    if (a.contains(QStringLiteral("minZoom"))) opts.minZoom = a.value(QStringLiteral("minZoom")).toDouble();
    return opts;
}

}  // namespace

CameraState CameraState::fromMap(const QMapLibre::Map& map)
{
    CameraState s;
    s.lat = map.latitude();
    s.lon = map.longitude();
    s.zoom = map.zoom();
    s.bearing = map.bearing();
    s.pitch = map.pitch();
    s.padding = map.margins();
    return s;
}

QJsonObject CameraState::toJson() const
{
    return QJsonObject{
        {QStringLiteral("center"), QJsonArray{lon, lat}},
        {QStringLiteral("zoom"), zoom},
        {QStringLiteral("bearing"), bearing},
        {QStringLiteral("pitch"), pitch},
        {QStringLiteral("padding"), QJsonObject{{QStringLiteral("top"), padding.top()}, {QStringLiteral("right"), padding.right()},
                                                {QStringLiteral("bottom"), padding.bottom()}, {QStringLiteral("left"), padding.left()}}},
    };
}

QJsonObject CameraState::boundsJson(const QMapLibre::Map& map, const QSize& size)
{
    const QMargins m = map.margins();
    const QPointF corners[4] = {
        QPointF(m.left(), m.top()),
        QPointF(size.width() - m.right(), m.top()),
        QPointF(size.width() - m.right(), size.height() - m.bottom()),
        QPointF(m.left(), size.height() - m.bottom()),
    };
    double w = 180, e = -180, s = 90, n = -90;
    for (const QPointF& c : corners) {
        const QMapLibre::Coordinate ll = map.coordinateForPixel(c);
        s = std::min(s, ll.first);
        n = std::max(n, ll.first);
        w = std::min(w, ll.second);
        e = std::max(e, ll.second);
    }
    return QJsonObject{{QStringLiteral("bounds"), QJsonArray{QJsonArray{w, s}, QJsonArray{e, n}}}};
}

bool CameraState::operator==(const CameraState& o) const
{
    return nearly(lon, o.lon) && nearly(lat, o.lat) && nearly(zoom, o.zoom) && nearly(bearing, o.bearing) && nearly(pitch, o.pitch) &&
           padding == o.padding;
}

IntentResult applyCameraIntent(QMapLibre::Map& map, const QJsonObject& intent, const QSize& size, bool gestureInProgress)
{
    IntentResult r;
    const QString type = intent.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("cancel")) {
        // The binding has no cancelTransitions; jumping to the current camera
        // ends any running transition at its current frame.
        QMapLibre::CameraOptions here;
        here.center = QVariant::fromValue(QMapLibre::Coordinate(map.latitude(), map.longitude()));
        here.zoom = map.zoom();
        here.bearing = map.bearing();
        here.pitch = map.pitch();
        map.jumpTo(here);
        r.accepted = true;
        return r;
    }
    if (gestureInProgress && !intent.value(QStringLiteral("force")).toBool()) {
        r.error = QStringLiteral("maps.gestureInProgress: intent rejected while a gesture is in progress");
        return r;
    }

    if (intent.contains(QStringLiteral("padding"))) map.setMargins(readPadding(intent.value(QStringLiteral("padding")), map.margins()));

    QMapLibre::CameraOptions camera;
    if (type == QLatin1String("jumpTo") || type == QLatin1String("easeTo") || type == QLatin1String("flyTo")) {
        if (intent.contains(QStringLiteral("center"))) {
            double lon, lat;
            if (!readLonLat(intent.value(QStringLiteral("center")), lon, lat)) {
                r.error = QStringLiteral("maps.badArgument: center must be [lon, lat]");
                return r;
            }
            camera.center = QVariant::fromValue(QMapLibre::Coordinate(lat, lon));
        }
        if (intent.contains(QStringLiteral("zoom"))) camera.zoom = std::clamp(intent.value(QStringLiteral("zoom")).toDouble(), 0.0, 25.5);
        if (intent.contains(QStringLiteral("bearing"))) camera.bearing = intent.value(QStringLiteral("bearing")).toDouble();
        if (intent.contains(QStringLiteral("pitch"))) camera.pitch = std::clamp(intent.value(QStringLiteral("pitch")).toDouble(), 0.0, 85.0);
        if (intent.contains(QStringLiteral("anchor"))) {
            const QJsonArray a = intent.value(QStringLiteral("anchor")).toArray();
            if (a.size() == 2) camera.anchor = QPointF(a[0].toDouble(), a[1].toDouble());
        }
    } else if (type == QLatin1String("fitBounds") || type == QLatin1String("fitCoordinates")) {
        double w = 180, e = -180, s = 90, n = -90;
        bool any = false;
        if (type == QLatin1String("fitBounds")) {
            const QJsonArray b = intent.value(QStringLiteral("bounds")).toArray();
            double lon0, lat0, lon1, lat1;
            if (b.size() != 2 || !readLonLat(b[0], lon0, lat0) || !readLonLat(b[1], lon1, lat1)) {
                r.error = QStringLiteral("maps.badArgument: bounds must be [[w,s],[e,n]]");
                return r;
            }
            w = std::min(lon0, lon1); e = std::max(lon0, lon1);
            s = std::min(lat0, lat1); n = std::max(lat0, lat1);
            any = true;
        } else {
            for (const QJsonValue& c : intent.value(QStringLiteral("coordinates")).toArray()) {
                double lon, lat;
                if (!readLonLat(c, lon, lat)) {
                    r.error = QStringLiteral("maps.badArgument: coordinates must be [[lon,lat],...]");
                    return r;
                }
                w = std::min(w, lon); e = std::max(e, lon);
                s = std::min(s, lat); n = std::max(n, lat);
                any = true;
            }
        }
        if (!any) {
            r.error = QStringLiteral("maps.badArgument: nothing to fit");
            return r;
        }
        // Fit padding is additive to the view's own margins (M7.3).
        const QMargins extra = readPadding(intent.value(QStringLiteral("fitPadding")), QMargins());
        const QMargins saved = map.margins();
        map.setMargins(saved + extra);
        const double bearing = intent.value(QStringLiteral("bearing")).toDouble(map.bearing());
        const double pitch = intent.value(QStringLiteral("pitch")).toDouble(map.pitch());
        const QMapLibre::CoordinateZoom cz = map.coordinateZoomForBounds(QMapLibre::Coordinate(s, w), QMapLibre::Coordinate(n, e), bearing, pitch);
        map.setMargins(saved);
        double zoom = cz.second;
        if (intent.contains(QStringLiteral("maxZoom"))) zoom = std::min(zoom, intent.value(QStringLiteral("maxZoom")).toDouble());
        camera.center = QVariant::fromValue(cz.first);
        camera.zoom = zoom;
        camera.bearing = bearing;
        camera.pitch = pitch;
        Q_UNUSED(size);
    } else {
        r.error = QStringLiteral("maps.badArgument: unknown intent type '%1'").arg(type);
        return r;
    }

    const bool animate = type != QLatin1String("jumpTo") && intent.value(QStringLiteral("animation")).toObject().value(QStringLiteral("duration")).toDouble(1) > 0;
    if (!animate) {
        map.jumpTo(camera);
    } else if (type == QLatin1String("flyTo")) {
        map.flyTo(camera, readAnimation(intent, 1200));
        r.animated = true;
    } else {
        map.easeTo(camera, readAnimation(intent, 300));
        r.animated = true;
    }
    r.accepted = true;
    return r;
}

}  // namespace maps::view
