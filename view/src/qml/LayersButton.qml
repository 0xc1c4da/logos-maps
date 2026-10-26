import QtQuick

// Layers button (M8.17 / M8.20): right edge below the compass, shown when a
// session has published a group or the basemap declares details (or the
// catalogue has more than one basemap). Opens the LayersPanel.
ChromeButton {
    id: layers
    objectName: "layersButton"
    active: chrome ? chrome.panelOpen : false
    tooltip: qsTr("Layers")
    onClicked: chrome.setPanelOpen(!chrome.panelOpen)

    Canvas {
        anchors.centerIn: parent
        width: 22
        height: 22
        property color fg: layers.theme.foreground
        property color accent: layers.theme.accent
        property bool on: layers.active
        onFgChanged: requestPaint()
        onAccentChanged: requestPaint()
        onOnChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.lineWidth = 1.6
            ctx.lineJoin = "round"
            // three stacked rhombi
            const layer = (cy, col) => {
                ctx.strokeStyle = col
                ctx.beginPath()
                ctx.moveTo(11, cy - 4); ctx.lineTo(19, cy); ctx.lineTo(11, cy + 4); ctx.lineTo(3, cy); ctx.closePath()
                ctx.stroke()
            }
            layer(15, fg); layer(11, fg); layer(7, on ? accent : fg)
        }
    }
}
