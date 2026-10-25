import QtQuick
import Maps.View 1.0

// The root of the view's own QQuickWindow (RFC-1). Loaded by ViewWindow
// from <plugin dir>/qml/MapWindow.qml, inside the maps_view process -- this
// file never runs in the host. The item named "map" is what ViewController
// drives; `gestures` (GestureController) and `view` (ViewController) are
// context properties set before load.
//
// Input (docs/architecture.md section 8.1): two handler stacks per device
// class (M8.1). Touch points go raw to the controller, which runs the
// detector (pan / pinch / rotate / shove / two-finger pan / taps / quick
// zoom / fling) with the Appendix B thresholds -- Qt's PinchHandler does
// not expose individual points, so a MultiPointTouchArea feeds them. The
// pointer stack (mouse, touchpad, stylus) uses handlers with target: null,
// dragThreshold 5 and permissive grabs so a pinch can take over a drag.
Item {
    id: root
    focus: true

    MapItem {
        id: map
        objectName: "map"
        anchors.fill: parent
    }

    // ── touch stack ──────────────────────────────────────────────────────
    MultiPointTouchArea {
        id: touch
        anchors.fill: parent
        objectName: "touchArea"
        mouseEnabled: gestures ? gestures.mouseAsTouchScreen : false
        minimumTouchPoints: 1
        maximumTouchPoints: 3
        onPressed: (points) => { for (const p of points) gestures.touchPressed(p.pointId, Qt.point(p.x, p.y)) }
        onUpdated: (points) => {
            gestures.touchBatchBegin()
            for (const p of points) gestures.touchMoved(p.pointId, Qt.point(p.x, p.y))
            gestures.touchBatchEnd()
        }
        onReleased: (points) => { for (const p of points) gestures.touchReleased(p.pointId, Qt.point(p.x, p.y)) }
        onCanceled: () => gestures.touchCancelled()
    }

    // ── pointer stack (M8.1, M8.2 pointer column, M8.9) ──────────────────
    DragHandler {
        id: pointerDrag
        objectName: "pointerDrag"
        target: null
        enabled: gestures ? !gestures.mouseAsTouchScreen : true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        dragThreshold: 5
        grabPermissions: PointerHandler.CanTakeOverFromAnything | PointerHandler.ApprovesTakeOverByAnything
        property int button: Qt.NoButton
        onActiveChanged: {
            if (active) {
                button = centroid.pressedButtons
                gestures.pointerDragBegan(centroid.pressPosition, button, centroid.modifiers)
            } else {
                gestures.pointerDragEnded(centroid.position)
            }
        }
        onCentroidChanged: if (active) gestures.pointerDragMoved(centroid.position)
    }

    WheelHandler {
        objectName: "wheel"
        target: null
        enabled: gestures ? !gestures.mouseAsTouchScreen : true
        // QTBUG-87646 / QTBUG-112394: trackpads are indistinguishable from
        // mice on cocoa and wayland, so TouchPad must be accepted too.
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => gestures.wheel(point.position, event.angleDelta.y, event.pixelDelta.y, event.modifiers)
    }

    TapHandler {
        objectName: "leftTap"
        enabled: gestures ? !gestures.mouseAsTouchScreen : true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.WithinBounds
        exclusiveSignals: TapHandler.SingleTap | TapHandler.DoubleTap
        longPressThreshold: 0.5
        onSingleTapped: (eventPoint) => gestures.clicked(eventPoint.position, Qt.LeftButton)
        onDoubleTapped: (eventPoint) => gestures.doubleClicked(eventPoint.position, Qt.LeftButton)
        onLongPressed: () => gestures.pointerLongPressed(point.position)
    }

    TapHandler {
        objectName: "rightTap"
        enabled: gestures ? !gestures.mouseAsTouchScreen : true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        acceptedButtons: Qt.RightButton
        gesturePolicy: TapHandler.WithinBounds
        onTapped: (eventPoint) => gestures.clicked(eventPoint.position, Qt.RightButton)
    }

    // ── keyboard / D-pad (M8.8) ──────────────────────────────────────────
    Keys.onPressed: (event) => { if (!event.isAutoRepeat || event.key !== Qt.Key_Return) event.accepted = gestures.keyPressed(event.key, event.modifiers) }
    Keys.onReleased: (event) => { if (!event.isAutoRepeat) event.accepted = gestures.keyReleased(event.key, event.modifiers) }

    // ── built-in chrome (M6.11, M9.9): drawn in the view's own window ────
    ScaleBar {
        objectName: "scaleBar"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 12
        metersPerPixel: view ? view.metersPerPixel : 0
    }

    Compass {
        objectName: "compass"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        bearing: view ? view.bearing : 0
        visible: view ? view.compassVisible : false
        onClicked: view.resetNorth()
    }

    AttributionButton {
        objectName: "attribution"
        html: view ? view.attributionHtml : ""
        corner: view ? view.attributionCorner : "bottom-right"
    }
}
