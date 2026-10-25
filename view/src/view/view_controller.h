// ViewController: one maps_view instance end to end (docs/architecture.md
// sections 5-7, 12.1 view-side protocol).
//
//   core events ──► SceneApplier / StyleLoader / camera intents ──► Map
//   Map changes ──► MapChange mapping ──► applier hooks, view state, events
//
// Owns the ViewWindow (RFC-1 surface), the SceneApplier, the StyleLoader and
// the logos:// provider; speaks to maps_core only through the CoreClient it
// is given, always asynchronously. The backend (.rep) and the dev launcher
// are thin shells around it.
#pragma once

#include "view/camera_intents.h"
#include "view/core_client.h"
#include "view/engine_config.h"

#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <memory>

namespace maps::view {

class LogosFileSource;
class MapItem;
class SceneApplier;
class StyleLoader;
class ViewWindow;

class ViewController : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString moduleName = QStringLiteral("maps_view");
        QString session = QStringLiteral("default");
        QString surfaceTransport = QStringLiteral("none");  // capabilities.surface.transport
        QString qmlDir;                                      // override for MapWindow.qml lookup
        QString moduleDir;                                   // plugin install dir (assets/)
        QSize initialSize = QSize(800, 600);
        bool showWindow = true;
        bool registerLogosProvider = true;
    };

    ViewController(CoreClient* core, Options options, QObject* parent = nullptr);
    ~ViewController() override;

    /// Create the window, open the view with the core, load the basemap.
    void start();
    /// closeView and tear the window down.
    void stop();

    QString viewId() const { return m_viewId; }
    QString surfaceToken() const { return m_surfaceToken; }
    QString status() const { return m_status; }
    QJsonObject capabilities() const { return m_capabilities; }
    QJsonObject viewState() const { return m_viewState; }
    QString basemap() const { return m_basemap; }
    quint64 appliedRevision() const;

    ViewWindow* window() const { return m_window.get(); }
    MapItem* mapItem() const { return m_mapItem; }
    SceneApplier* sceneApplier() const { return m_applier.get(); }
    StyleLoader* styleLoader() const { return m_styles.get(); }

    /// The RFC-2 token this process was spawned with (ui-host --gui sets the
    /// desktop file name to logos-view-<token>), or "" standalone.
    static QString surfaceTokenFromProcess();

public slots:
    void setSize(int width, int height, double devicePixelRatio);
    void setOptions(const QString& optionsJson);
    void camera(const QString& intentJson);
    void setSession(const QString& session);
    void setGestureInProgress(bool inProgress);

signals:
    void statusChanged(const QString& status);
    void viewIdChanged(const QString& viewId);
    void viewStateChanged(const QString& viewStateJson);
    void capabilitiesChanged(const QString& capabilitiesJson);
    void cameraIdle(const QString& json);
    void featurePicked(const QString& json);
    void attributionChanged(const QString& html);
    void basemapChanged(const QString& id);
    void error(const QString& code, const QString& message);

private:
    void setStatus(const QString& status);
    void onCoreEvent(const QString& topic, const QJsonObject& payload);
    void onMapChange(int change);
    void openView();
    void loadBasemap(const QString& id);
    void chooseDefaultBasemap();
    void refreshViewState(bool idle);
    void publishViewState();
    void applyOptions(const QJsonObject& options);
    void installProviders();
    void reportEvent(const QString& topic, const QJsonObject& payload);
    QSize viewportSize() const;

    CoreClient* m_core;
    Options m_options;
    EngineConfig m_engine;
    std::unique_ptr<ViewWindow> m_window;
    MapItem* m_mapItem = nullptr;
    std::unique_ptr<SceneApplier> m_applier;
    std::unique_ptr<StyleLoader> m_styles;
    std::shared_ptr<LogosFileSource> m_logosProvider;

    QString m_viewId;
    QString m_surfaceToken;
    QString m_status = QStringLiteral("starting");
    QString m_basemap;
    QJsonObject m_capabilities;
    QJsonObject m_viewState;
    QJsonObject m_options_;  // accumulated setViewOptions
    QJsonObject m_limits;
    CameraState m_lastCamera;
    bool m_opened = false;
    bool m_gestureInProgress = false;
    bool m_stateDirty = false;
    QTimer m_stateTimer;
    int m_instance = 0;
    static int s_instances;
};

}  // namespace maps::view
