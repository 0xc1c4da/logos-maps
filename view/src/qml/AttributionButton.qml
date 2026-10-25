import QtQuick

// Attribution (M9.9): a compact button that opens the full list composed by
// StyleLoader (descriptor attribution + engine copyrights). Hosts can move
// it to another corner through the `attributionCorner` option, never hide it.
Item {
    id: attribution
    property string html: ""
    property string corner: "bottom-right"
    property bool open: false

    readonly property bool atBottom: corner.indexOf("bottom") === 0
    readonly property bool atRight: corner.indexOf("right") >= 0

    anchors.bottom: atBottom ? parent.bottom : undefined
    anchors.top: atBottom ? undefined : parent.top
    anchors.right: atRight ? parent.right : undefined
    anchors.left: atRight ? undefined : parent.left
    anchors.margins: 8
    width: open ? Math.min(panel.implicitWidth + 16, parent.width - 16) : 22
    height: open ? panel.implicitHeight + 16 : 22

    Rectangle {
        anchors.fill: parent
        radius: attribution.open ? 6 : 11
        color: "#ccffffff"
        border.color: "#999"
    }
    Text {
        visible: !attribution.open
        anchors.centerIn: parent
        text: "i"
        font.bold: true
        font.pixelSize: 13
        color: "#333"
    }
    Text {
        id: panel
        visible: attribution.open
        anchors.centerIn: parent
        width: Math.min(implicitWidth, attribution.parent ? attribution.parent.width - 32 : 320)
        textFormat: Text.RichText
        wrapMode: Text.Wrap
        font.pixelSize: 11
        color: "#222"
        text: attribution.html.length > 0 ? attribution.html : "Map data: see basemap descriptor"
        onLinkActivated: (link) => Qt.openUrlExternally(link)
    }
    TapHandler {
        onTapped: attribution.open = !attribution.open
    }
}
