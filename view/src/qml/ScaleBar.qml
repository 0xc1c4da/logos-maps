import QtQuick

// Scale bar (M6.11): width = distance / metersPerPixel at the padded centre,
// distance picked from a 1-2-5 series so the bar stays between 60 and 160 px.
Item {
    id: bar
    property real metersPerPixel: 0
    property real maxWidth: 160
    property real distanceMeters: 0
    property string label: ""

    width: metersPerPixel > 0 ? distanceMeters / metersPerPixel : 0
    height: 18
    visible: metersPerPixel > 0 && distanceMeters > 0

    onMetersPerPixelChanged: recompute()
    Component.onCompleted: recompute()

    function recompute() {
        if (metersPerPixel <= 0) { distanceMeters = 0; label = ""; return }
        const maxMeters = maxWidth * metersPerPixel
        if (maxMeters < 1) { distanceMeters = 0; label = ""; return }
        const pow = Math.pow(10, Math.floor(Math.log10(maxMeters)))  // <= maxMeters
        let best = pow
        for (const s of [1, 2, 5]) if (s * pow <= maxMeters) best = s * pow
        distanceMeters = best
        label = best >= 1000 ? (best / 1000) + " km" : best + " m"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 4
        color: "#222"
        border.color: "#fff"
        border.width: 1
    }
    Rectangle { anchors.left: parent.left; anchors.bottom: parent.bottom; width: 2; height: 10; color: "#222" }
    Rectangle { anchors.right: parent.right; anchors.bottom: parent.bottom; width: 2; height: 10; color: "#222" }
    Text {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        text: bar.label
        font.pixelSize: 11
        color: "#222"
        style: Text.Outline
        styleColor: "#fff"
    }
}
