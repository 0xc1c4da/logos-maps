import QtQuick

// Attribution (M9.9, M8.17): a compact button that opens the full list
// composed by StyleLoader (descriptor attribution + engine copyrights).
// Hosts can move it to another corner through the `attributionCorner`
// option, never hide it. Themed (M8.23).
//
// Open state follows gl-js's `maplibregl-compact-show`: the text unfolds
// beside the button, which stays put and toggles; a drag or tap on the map
// folds it again (gl-js `_updateCompactMinimize`). The text is mostly links,
// which Text consumes on tap, so the button is the one reliable toggle.
Item {
    id: attribution
    property string html: ""
    property string corner: "bottom-right"
    property bool open: false
    property var theme: chrome ? chrome.theme : ({})
    property var padding: chrome ? chrome.padding : ({})

    readonly property bool atBottom: corner.indexOf("bottom") === 0
    readonly property bool atRight: corner.indexOf("right") >= 0
    readonly property int buttonSize: 22
    readonly property int gap: 6

    anchors.bottom: atBottom ? parent.bottom : undefined
    anchors.top: atBottom ? undefined : parent.top
    anchors.right: atRight ? parent.right : undefined
    anchors.left: atRight ? undefined : parent.left
    anchors.bottomMargin: 8 + (padding.bottom || 0)
    anchors.topMargin: 8 + (padding.top || 0)
    anchors.rightMargin: 8 + (padding.right || 0)
    anchors.leftMargin: 8 + (padding.left || 0)
    width: open ? Math.min(panel.implicitWidth + buttonSize + gap * 3, parent.width - 16) : buttonSize
    height: open ? Math.max(buttonSize, panel.implicitHeight + gap * 2) : buttonSize

    // A gesture on the map folds the text away.
    Connections {
        target: typeof gestures !== "undefined" ? gestures : null
        ignoreUnknownSignals: true
        function onGestureBegan() { attribution.open = false }
        function onTapped() { attribution.open = false }
    }

    Rectangle {
        anchors.fill: parent
        radius: attribution.open ? (attribution.theme.radius !== undefined ? attribution.theme.radius : 6) : attribution.buttonSize / 2
        color: attribution.theme.background
        border.color: attribution.theme.border
    }
    Text {
        id: panel
        visible: attribution.open
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: attribution.atRight ? parent.left : undefined
        anchors.right: attribution.atRight ? undefined : parent.right
        anchors.leftMargin: attribution.gap
        anchors.rightMargin: attribution.gap
        width: Math.min(implicitWidth, attribution.width - attribution.buttonSize - attribution.gap * 3)
        textFormat: Text.RichText
        wrapMode: Text.Wrap
        font.pixelSize: Math.max(10, (attribution.theme.fontPixelSize || 13) - 2)
        font.family: attribution.theme.fontFamily || ""
        color: attribution.theme.foreground
        linkColor: attribution.theme.accent
        text: attribution.html.length > 0 ? attribution.html : qsTr("Map data: see basemap descriptor")
        onLinkActivated: (link) => Qt.openUrlExternally(link)
    }
    // The toggle: ⓘ when folded, × when unfolded, always at the outer edge.
    Item {
        id: button
        objectName: "attributionToggle"
        width: attribution.buttonSize
        height: attribution.buttonSize
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: attribution.atRight ? parent.right : undefined
        anchors.left: attribution.atRight ? undefined : parent.left
        Text {
            anchors.centerIn: parent
            text: attribution.open ? "\u00d7" : "i"
            font.bold: !attribution.open
            font.pixelSize: attribution.theme.fontPixelSize || 13
            font.family: attribution.theme.fontFamily || ""
            color: attribution.theme.foreground
        }
        TapHandler {
            // Exclusive grab: otherwise the map's own TapHandler fires too and
            // folds the text straight back.
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: attribution.open = !attribution.open
        }
    }
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Attribution")
}
