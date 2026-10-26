import QtQuick

// Compass (M8.17): shown while bearing != 0 or pitch != 0; a tap eases back
// to north and flat. Colours from the theme (M8.23).
Item {
    id: compass
    property real bearing: 0
    property var theme: chrome ? chrome.theme : ({})
    signal clicked()
    width: 36
    height: 36

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: compass.theme.background
        border.color: compass.theme.border
    }
    Item {
        anchors.centerIn: parent
        width: 20
        height: 20
        // The needle points to north: rotate the drawing by -bearing.
        rotation: -compass.bearing
        Canvas {
            anchors.fill: parent
            property color accent: compass.theme.accent
            property color fg: compass.theme.foreground
            onAccentChanged: requestPaint()
            onFgChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = accent
                ctx.beginPath(); ctx.moveTo(10, 0); ctx.lineTo(14, 10); ctx.lineTo(6, 10); ctx.closePath(); ctx.fill()
                ctx.fillStyle = fg
                ctx.beginPath(); ctx.moveTo(10, 20); ctx.lineTo(14, 10); ctx.lineTo(6, 10); ctx.closePath(); ctx.fill()
            }
        }
    }
    TapHandler { onTapped: compass.clicked() }
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Reset north")
}
