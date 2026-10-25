// Builtins: the location puck of docs/architecture.md M6.11, drawn as
// ordinary layers in the reserved session maps/builtin (circle + heading
// symbol + accuracy polygon shown only past a threshold, default 100 m).
//
// The puck is per view (it follows this view's location option) so it is
// applied straight to this view's map, not published through maps_core, and
// re-added on every style load and raised above the scene after every layer
// reconcile. Screen-anchored chrome (scale bar, compass, attribution button)
// is QML in MapWindow.qml, not layers -- see the M6.11 note in the spec.
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QStringList>

#include <optional>

namespace QMapLibre { class Map; }

namespace maps::view {

class Builtins : public QObject {
    Q_OBJECT
public:
    static constexpr const char* kSession = "maps/builtin";
    static constexpr double kDefaultAccuracyThresholdM = 100.0;

    explicit Builtins(QMapLibre::Map* map, QObject* parent = nullptr);

    void setMap(QMapLibre::Map* map);

    /// {"lon","lat","accuracy":m,"heading":deg} or null to hide the puck.
    void setLocation(const QJsonValue& location);
    QJsonValue location() const { return m_location; }
    /// {"accuracyThreshold":m,"visible":bool}
    void setPuckOptions(const QJsonObject& options);
    double accuracyThreshold() const { return m_accuracyThreshold; }
    bool puckVisible() const { return m_visible; }

    /// DidFinishLoadingStyle: re-add image, source and layers.
    void onStyleLoaded();
    /// Scene layers were re-inserted: raise the built-ins back to the top.
    void onSceneLayersReconciled();

    QStringList layerIds() const;
    QString sourceId() const;
    bool installed() const { return m_installed; }

private:
    void install();
    void uninstall();
    void updateSource();
    QJsonObject featureCollection() const;

    QMapLibre::Map* m_map;
    QJsonValue m_location;
    double m_accuracyThreshold = kDefaultAccuracyThresholdM;
    bool m_visible = true;
    bool m_installed = false;
    bool m_styleReady = false;
};

}  // namespace maps::view
