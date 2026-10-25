// Camera state <-> JSON and intent application (docs/architecture.md M7.1,
// M7.4). Public surfaces use [lon, lat]; the binding's Coordinate is
// (lat, lon) and the conversion happens here, at the edge.
#pragma once

#include <QJsonObject>
#include <QMargins>
#include <QSize>
#include <QString>

namespace QMapLibre { class Map; }

namespace maps::view {

struct CameraState {
    double lon = 0, lat = 0;
    double zoom = 0, bearing = 0, pitch = 0;
    QMargins padding;

    static CameraState fromMap(const QMapLibre::Map& map);
    /// {"center":[lon,lat],"zoom","bearing","pitch","padding":{top,right,bottom,left}}
    QJsonObject toJson() const;
    /// Padded visible region {"bounds":[[w,s],[e,n]]} for the given size.
    static QJsonObject boundsJson(const QMapLibre::Map& map, const QSize& size);
    bool operator==(const CameraState& o) const;
    bool operator!=(const CameraState& o) const { return !(*this == o); }
};

struct IntentResult {
    bool accepted = false;
    QString error;  // maps.badArgument text when !accepted
    bool animated = false;
};

/// Apply one intent ({"v":1,"type":...}) to the map. `gestureInProgress`
/// rejects non-forced intents (M7.4). `size` is the viewport in logical px.
IntentResult applyCameraIntent(QMapLibre::Map& map, const QJsonObject& intent, const QSize& size, bool gestureInProgress);

}  // namespace maps::view
