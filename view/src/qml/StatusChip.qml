import QtQuick

// Status chip (M8.17): a non-modal pill at the top centre for basemap
// loading (> 1 s), basemap failure, provider offline and missing anchors;
// disappears on recovery. Driven entirely by `chrome.status`.
Item {
    id: chip
    property var theme: chrome ? chrome.theme : ({})
    property var padding: chrome ? chrome.padding : ({})
    readonly property var status: chrome ? chrome.status : ({ visible: false })
    visible: !!status.visible && !!status.text
    width: label.implicitWidth + 24
    height: label.implicitHeight + 12

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: chip.status.kind === "error" ? chip.theme.accent : chip.theme.background
        border.color: chip.status.kind === "error" ? chip.theme.accent : chip.theme.border
        opacity: 0.95
    }
    Text {
        id: label
        anchors.centerIn: parent
        text: chip.status.text || ""
        font.pixelSize: Math.max(10, (chip.theme.fontPixelSize || 13) - 1)
        font.family: chip.theme.fontFamily || ""
        color: chip.status.kind === "error" ? chip.theme.background : chip.theme.foreground
    }
    Accessible.role: Accessible.StatusBar
    Accessible.name: chip.status.text || ""
}
