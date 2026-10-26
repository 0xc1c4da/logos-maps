import QtQuick

// Locate button (M8.17 / M8.18): visible once a location source is set;
// a tap cycles off -> follow -> followHeading -> off. The icon reflects the
// mode: hollow ring (off), filled dot (follow), dot with a heading wedge
// (followHeading). Disabled while there is no fix.
ChromeButton {
    id: locate
    objectName: "locateButton"
    readonly property string mode: chrome ? chrome.followMode : "off"
    enabled: chrome ? chrome.locationAvailable : false
    active: mode !== "off"
    tooltip: mode === "off" ? qsTr("Show my location") : mode === "follow" ? qsTr("Follow with heading") : qsTr("Stop following")
    onClicked: chrome.cycleFollow()

    Canvas {
        anchors.centerIn: parent
        width: 22
        height: 22
        property string mode: locate.mode
        property color fg: locate.theme.foreground
        property color accent: locate.theme.accent
        onModeChanged: requestPaint()
        onFgChanged: requestPaint()
        onAccentChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = 11
            const col = mode === "off" ? fg : accent
            ctx.strokeStyle = col
            ctx.fillStyle = col
            ctx.lineWidth = 1.6
            // crosshair ticks
            ctx.beginPath()
            ctx.moveTo(c, 0); ctx.lineTo(c, 4)
            ctx.moveTo(c, 18); ctx.lineTo(c, 22)
            ctx.moveTo(0, c); ctx.lineTo(4, c)
            ctx.moveTo(18, c); ctx.lineTo(22, c)
            ctx.stroke()
            // ring
            ctx.beginPath(); ctx.arc(c, c, 6, 0, Math.PI * 2); ctx.stroke()
            if (mode !== "off") { ctx.beginPath(); ctx.arc(c, c, 3, 0, Math.PI * 2); ctx.fill() }
            if (mode === "followHeading") {
                // heading wedge above the dot
                ctx.beginPath(); ctx.moveTo(c, 2); ctx.lineTo(c + 3.5, 7); ctx.lineTo(c - 3.5, 7); ctx.closePath(); ctx.fill()
            }
        }
    }
}
