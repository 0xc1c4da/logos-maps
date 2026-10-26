// ViewWindow: the QQuickWindow the view plugin renders into (RFC-1, M5.1;
// RFC-8 for where it comes from).
//
// Under ui-host the window is the host's: `LogosViewSurface::window()`
// (<logos/view_surface.h>) hands out the one window the runtime presents to
// Basecamp over whatever SurfaceTransport is in use — a Wayland toplevel of
// the nested compositor on Linux, a channel ring elsewhere. Its geometry is
// the host's: ViewWindow never shows, resizes or deletes it. Under the dev
// launcher, in tests, or on a ui-host that predates RFC-8 there is no such
// window and ViewWindow opens a plain top-level of its own.
//
// Either way the window loads <plugin dir>/qml/MapWindow.qml into its
// contentItem() with a QQmlEngine ViewWindow owns (the gesture layer, M5,
// extends that QML); when the QML cannot be found (packaging error) a
// C++-built MapItem fills the window so the module still renders.
#pragma once

#include <QList>
#include <QObject>
#include <QQmlError>
#include <QSize>
#include <QString>

class QQmlComponent;
class QQmlEngine;
class QQuickItem;
class QQuickWindow;

namespace maps::view {

class GestureController;
class MapItem;

class ViewWindow : public QObject {
    Q_OBJECT
public:
    /// `hostWindow` is the runtime's window (see hostWindow()); not owned.
    /// nullptr → ViewWindow creates and owns a top-level QQuickWindow.
    explicit ViewWindow(QQuickWindow* hostWindow = nullptr, QObject* parent = nullptr);
    ~ViewWindow() override;

    /// The window the runtime renders for the host (RFC-8), or nullptr when
    /// this process has no SurfaceTransport.
    static QQuickWindow* hostWindow();
    /// RFC-8 `LogosViewSurface::transport()`: "wayland", "channel-shm",
    /// "channel-d3d11", "channel-iosurface" or "none".
    static QString hostTransport();

    /// Directory of the shared object this code is linked into (dladdr).
    static QString pluginDirectory();
    /// <pluginDir>/qml, or the MAPS_VIEW_QML_DIR override.
    static QString qmlDirectory();

    QQuickWindow* window() const { return m_window; }
    /// true when the window is ViewWindow's own top-level (dev launcher,
    /// tests); false when it is the host's and geometry is not ours.
    bool ownsWindow() const { return m_owned; }
    QQmlEngine* engine() const { return m_engine; }

    /// Exposed to MapWindow.qml as the context property `gestures`; set
    /// before load().
    void setGestureController(GestureController* controller);
    /// Any other QObject for MapWindow.qml (the view itself as `view`).
    void setContextObject(const QString& name, QObject* object);

    /// Load qml/MapWindow.qml (or the fallback) into the window. Returns the
    /// map item.
    MapItem* load(const QString& qmlDir = QString());
    MapItem* mapItem() const { return m_mapItem; }
    /// The loaded QML root (or the fallback root), filling the window.
    QQuickItem* rootObject() const { return m_root; }
    bool usedFallback() const { return m_fallback; }
    QList<QQmlError> errors() const { return m_errors; }

    // Geometry helpers for the OWN-window case. On a host window they are
    // no-ops: the host sizes and shows the surface (RFC-5).
    void setTitle(const QString& title);
    void resize(int width, int height);
    void resize(const QSize& size);
    void show();

private:
    void clearRoot();
    void fitRoot();

    QQuickWindow* m_window = nullptr;
    bool m_owned = false;
    QQmlEngine* m_engine = nullptr;
    QQmlComponent* m_component = nullptr;
    QQuickItem* m_root = nullptr;
    MapItem* m_mapItem = nullptr;
    bool m_fallback = false;
    QList<QQmlError> m_errors;
};

}  // namespace maps::view
