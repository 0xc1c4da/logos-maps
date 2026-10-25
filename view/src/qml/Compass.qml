import QtQuick

// Compass (M6.11): shown while bearing != 0; a tap eases back to north.
Item {
    id: compass
    property real bearing: 0
    signal clicked()
    width: 36
    height: 36

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: "#e6ffffff"
        border.color: "#999"
    }
    Item {
        anchors.centerIn: parent
        width: 20
        height: 20
        // The needle points to north: rotate the drawing by -bearing.
        rotation: -compass.bearing
        Canvas {
            anchors.fill: parent
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = "#d33"
                ctx.beginPath(); ctx.moveTo(10, 0); ctx.lineTo(14, 10); ctx.lineTo(6, 10); ctx.closePath(); ctx.fill()
                ctx.fillStyle = "#555"
                ctx.beginPath(); ctx.moveTo(10, 20); ctx.lineTo(14, 10); ctx.lineTo(6, 10); ctx.closePath(); ctx.fill()
            }
        }
    }
    TapHandler { onTapped: compass.clicked() }
}
