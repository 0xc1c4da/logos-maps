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

    // Pointer hover feeds the coordinate readout and M8.15a hover state.
    HoverHandler {
        objectName: "hover"
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        onPointChanged: if (chrome && hovered) chrome.pointerMoved(point.position.x, point.position.y)
        onHoveredChanged: if (chrome && !hovered) chrome.pointerLeft()
    }

    // ── keyboard / D-pad (M8.8, M8.22) ───────────────────────────────────
    Keys.onPressed: (event) => { if (!event.isAutoRepeat || event.key !== Qt.Key_Return) event.accepted = gestures.keyPressed(event.key, event.modifiers) }
    Keys.onReleased: (event) => { if (!event.isAutoRepeat) event.accepted = gestures.keyReleased(event.key, event.modifiers) }

    // ── map chrome (section 8.4): drawn in the view's own window ─────────
    // Every item is switched by `chrome.items`, themed by `chrome.theme`
    // and keeps clear of the host's `padding` (M7.3).
    readonly property var items: chrome ? chrome.items : ({})
    readonly property var pad: chrome ? chrome.padding : ({})
    readonly property int edge: 12

    // bottom-left: scale bar, coordinate readout above it
    ScaleBar {
        id: scaleBar
        objectName: "scaleBar"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.edge + (root.pad.left || 0)
        anchors.bottomMargin: root.edge + (root.pad.bottom || 0)
        metersPerPixel: (view && root.items.scaleBar !== false) ? view.metersPerPixel : 0
    }
    CoordinateReadout {
        objectName: "coordinateReadout"
        anchors.left: parent.left
        anchors.bottom: scaleBar.visible ? scaleBar.top : parent.bottom
        anchors.leftMargin: root.edge + (root.pad.left || 0)
        anchors.bottomMargin: scaleBar.visible ? 8 : root.edge + (root.pad.bottom || 0)
        visible: root.items.coordinateReadout === true && !!coordinate.text
    }

    // top-right: compass, layers button below it
    Compass {
        id: compass
        objectName: "compass"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: root.edge + (root.pad.right || 0)
        anchors.topMargin: root.edge + (root.pad.top || 0)
        bearing: view ? view.bearing : 0
        visible: root.items.compass !== false && (view ? view.compassVisible : false)
        onClicked: view.resetNorth()
    }
    LayersButton {
        id: layersButton
        anchors.right: parent.right
        anchors.top: compass.visible ? compass.bottom : parent.top
        anchors.rightMargin: root.edge + (root.pad.right || 0)
        anchors.topMargin: compass.visible ? 8 : root.edge + (root.pad.top || 0)
        visible: root.items.layersButton !== false && chrome && chrome.layersAvailable
    }

    // bottom-right, stacked upward: attribution, zoom buttons, locate button
    AttributionButton {
        id: attribution
        objectName: "attribution"
        html: view ? view.attributionHtml : ""
        corner: view ? view.attributionCorner : "bottom-right"
        visible: root.items.attribution !== false  // M9.9: hosts move it; `false` is for tests only
    }
    ZoomButtons {
        id: zoomButtons
        objectName: "zoomButtons"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: root.edge + (root.pad.right || 0)
        anchors.bottomMargin: root.edge + (root.pad.bottom || 0) + (attribution.visible && attribution.atBottom && attribution.atRight ? 30 : 0)
        visible: root.items.zoomButtons === true
    }
    LocateButton {
        anchors.right: parent.right
        anchors.bottom: zoomButtons.visible ? zoomButtons.top : parent.bottom
        anchors.rightMargin: root.edge + (root.pad.right || 0)
        anchors.bottomMargin: zoomButtons.visible ? 8 : root.edge + (root.pad.bottom || 0) + (attribution.visible && attribution.atBottom && attribution.atRight ? 30 : 0)
        visible: root.items.locateButton !== false && chrome && chrome.locationAvailable
    }

    // top-centre: status chip
    StatusChip {
        objectName: "statusChip"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: root.edge + (root.pad.top || 0)
        visible: root.items.statusChip !== false && !!status.visible && !!status.text
    }

    // anchored: callout; sheet: layers panel
    Callout {
        objectName: "callout"
        visible: root.items.callouts !== false && open && onScreen
    }
    LayersPanel {
        objectName: "layersPanel"
        anchors.right: parent.right
        anchors.top: layersButton.visible ? layersButton.bottom : parent.top
        anchors.rightMargin: root.edge + (root.pad.right || 0)
        anchors.topMargin: layersButton.visible ? 8 : root.edge + (root.pad.top || 0)
    }
}
