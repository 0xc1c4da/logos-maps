import QtQuick

// Zoom buttons (M8.17): off by default, bottom-right above attribution.
// +/- zoom by one level about the padded centre over 500 ms through the
// same GestureController::zoomStep the keyboard uses (M8.22); disabled at
// the zoom limits.
Column {
    id: zoom
    property var theme: chrome ? chrome.theme : ({})
    spacing: 4

    ChromeButton {
        objectName: "zoomIn"
        theme: zoom.theme
        enabled: chrome ? chrome.canZoomIn : false
        tooltip: qsTr("Zoom in")
        onClicked: chrome.zoomIn()
        Text {
            anchors.centerIn: parent
            text: "+"
            font.pixelSize: 20
            font.family: zoom.theme.fontFamily || ""
            color: zoom.theme.foreground
        }
    }
    ChromeButton {
        objectName: "zoomOut"
        theme: zoom.theme
        enabled: chrome ? chrome.canZoomOut : false
        tooltip: qsTr("Zoom out")
        onClicked: chrome.zoomOut()
        Text {
            anchors.centerIn: parent
            text: "−"
            font.pixelSize: 20
            font.family: zoom.theme.fontFamily || ""
            color: zoom.theme.foreground
        }
    }
}
