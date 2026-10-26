import QtQuick

// Attribution (M9.9, M8.17): a compact button that opens the full list
// composed by StyleLoader (descriptor attribution + engine copyrights).
// Hosts can move it to another corner through the `attributionCorner`
// option, never hide it. Themed (M8.23).
Item {
    id: attribution
    property string html: ""
    property string corner: "bottom-right"
    property bool open: false
    property var theme: chrome ? chrome.theme : ({})
    property var padding: chrome ? chrome.padding : ({})

    readonly property bool atBottom: corner.indexOf("bottom") === 0
    readonly property bool atRight: corner.indexOf("right") >= 0

    anchors.bottom: atBottom ? parent.bottom : undefined
    anchors.top: atBottom ? undefined : parent.top
    anchors.right: atRight ? parent.right : undefined
    anchors.left: atRight ? undefined : parent.left
    anchors.bottomMargin: 8 + (padding.bottom || 0)
    anchors.topMargin: 8 + (padding.top || 0)
    anchors.rightMargin: 8 + (padding.right || 0)
    anchors.leftMargin: 8 + (padding.left || 0)
    width: open ? Math.min(panel.implicitWidth + 16, parent.width - 16) : 22
    height: open ? panel.implicitHeight + 16 : 22

    Rectangle {
        anchors.fill: parent
        radius: attribution.open ? (attribution.theme.radius !== undefined ? attribution.theme.radius : 6) : 11
        color: attribution.theme.background
        border.color: attribution.theme.border
    }
    Text {
        visible: !attribution.open
        anchors.centerIn: parent
        text: "i"
        font.bold: true
        font.pixelSize: attribution.theme.fontPixelSize || 13
        font.family: attribution.theme.fontFamily || ""
        color: attribution.theme.foreground
    }
    Text {
        id: panel
        visible: attribution.open
        anchors.centerIn: parent
        width: Math.min(implicitWidth, attribution.parent ? attribution.parent.width - 32 : 320)
        textFormat: Text.RichText
        wrapMode: Text.Wrap
        font.pixelSize: Math.max(10, (attribution.theme.fontPixelSize || 13) - 2)
        font.family: attribution.theme.fontFamily || ""
        color: attribution.theme.foreground
        linkColor: attribution.theme.accent
        text: attribution.html.length > 0 ? attribution.html : qsTr("Map data: see basemap descriptor")
        onLinkActivated: (link) => Qt.openUrlExternally(link)
    }
    TapHandler {
        onTapped: attribution.open = !attribution.open
    }
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Attribution")
}
