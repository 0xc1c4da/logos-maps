import QtQuick

// Coordinate readout (M8.17): off by default; bottom-left above the scale
// bar. Shows the centre coordinate, or the pointer's on pointer devices, to
// 5 decimals; a tap copies it.
Item {
    id: readout
    property var theme: chrome ? chrome.theme : ({})
    readonly property var coordinate: chrome ? chrome.coordinate : ({})
    visible: !!coordinate.text
    width: label.implicitWidth + 16
    height: label.implicitHeight + 8

    Rectangle {
        anchors.fill: parent
        radius: readout.theme.radius !== undefined ? readout.theme.radius : 6
        color: readout.theme.background
        border.color: readout.theme.border
        opacity: 0.9
    }
    Text {
        id: label
        objectName: "coordinateText"
        anchors.centerIn: parent
        text: readout.coordinate.text || ""
        font.pixelSize: Math.max(10, (readout.theme.fontPixelSize || 13) - 2)
        font.family: readout.theme.fontFamily || ""
        color: readout.theme.foreground
    }
    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: chrome.copyCoordinate() }
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Copy coordinate %1").arg(readout.coordinate.text || "")
}
