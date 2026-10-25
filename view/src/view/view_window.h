// ViewWindow: the top-level QQuickWindow the view plugin owns (RFC-1, M5.1).
//
// Under ui-host --gui the window is the Wayland surface Basecamp composites
// (section 6 of RFC-2); under the dev launcher it is a plain desktop window.
// The window loads <plugin dir>/qml/MapWindow.qml, which the gesture layer
// (M5) extends; when the QML cannot be found (packaging error) a C++-built
// MapItem fills the window so the module still renders.
#pragma once

#include <QQuickView>
#include <QString>

namespace maps::view {

class MapItem;

class ViewWindow : public QQuickView {
    Q_OBJECT
public:
    explicit ViewWindow(QWindow* parent = nullptr);

    /// Directory of the shared object this code is linked into (dladdr).
    static QString pluginDirectory();
    /// <pluginDir>/qml, or the MAPS_VIEW_QML_DIR override.
    static QString qmlDirectory();

    /// Load qml/MapWindow.qml (or the fallback). Returns the map item.
    MapItem* load(const QString& qmlDir = QString());
    MapItem* mapItem() const { return m_mapItem; }
    bool usedFallback() const { return m_fallback; }

private:
    MapItem* m_mapItem = nullptr;
    bool m_fallback = false;
};

}  // namespace maps::view
