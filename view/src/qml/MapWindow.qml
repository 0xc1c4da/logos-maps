import QtQuick
import Maps.View 1.0

// The root of the view's own QQuickWindow (RFC-1). Loaded by ViewWindow
// from <plugin dir>/qml/MapWindow.qml, inside the maps_view process -- this
// file never runs in the host. M5 adds the gesture handler stacks and the
// pick overlay here; the item named "map" is what ViewController drives.
Item {
    id: root

    MapItem {
        id: map
        objectName: "map"
        anchors.fill: parent
    }

    // Attribution is composed by StyleLoader and reported through the
    // backend's attributionChanged signal; the host renders it in its own
    // chrome (section 9.9). Nothing is drawn over the map here.
}
