import QtQuick

// One tile of the layers panel (M8.20): a picture with its label under it.
// The chosen or switched-on tile wears an accent ring on the picture and an
// accent label (Vela `LayersPanel.kt` Tile: 68 dp wide, 48 dp picture,
// 2.5 dp primary ring; Google Maps' layer sheet is the same shape). The
// picture is, in order of preference, an image, a legend of up to four
// swatches, or a glyph; a tile that cannot be switched draws no ring and
// takes no taps (legend-only groups).
Item {
    id: tile
    property var theme: ({})
    property string label: ""
    property string image: ""
    property var legend: []          // [{color?, image?, label}]
    property string glyph: ""        // fallback: a letter or short mark
    property bool on: false
    property bool toggleable: true
    property string tooltip: ""
    signal clicked()

    readonly property int pictureHeight: 48
    readonly property real radius: theme.radius !== undefined ? Math.min(theme.radius * 2, 12) : 12
    width: 68
    implicitHeight: pictureHeight + 4 + caption.implicitHeight
    height: implicitHeight

    Rectangle {
        id: picture
        width: parent.width
        height: tile.pictureHeight
        radius: tile.radius
        color: tile.theme.surface
        border.color: tile.on && tile.toggleable ? tile.theme.accent : tile.theme.border
        border.width: tile.on && tile.toggleable ? 2.5 : 1
        clip: true

        Image {
            anchors.fill: parent
            anchors.margins: picture.border.width
            source: tile.image
            visible: tile.image.length > 0
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }
        // Legend swatches: 2x2 at most, as Vela's glyph discs stand in for a picture.
        Grid {
            anchors.centerIn: parent
            visible: tile.image.length === 0 && (tile.legend || []).length > 0
            columns: (tile.legend || []).length > 1 ? 2 : 1
            spacing: 6
            Repeater {
                model: (tile.legend || []).slice(0, 4)
                delegate: Item {
                    width: 14; height: 14
                    Rectangle {
                        anchors.fill: parent
                        radius: 7
                        color: modelData.color || "transparent"
                        border.color: tile.theme.border
                        visible: !modelData.image
                    }
                    Image {
                        anchors.fill: parent
                        source: modelData.image || ""
                        visible: !!modelData.image
                        fillMode: Image.PreserveAspectFit
                    }
                }
            }
        }
        Text {
            anchors.centerIn: parent
            visible: tile.image.length === 0 && (tile.legend || []).length === 0
            text: tile.glyph
            font.pixelSize: 20
            font.family: tile.theme.fontFamily || ""
            color: tile.theme.foreground
            opacity: tile.on || !tile.toggleable ? 0.9 : 0.5
        }
        // Switched-off pictures are dimmed, so state reads without the ring.
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: tile.theme.background
            opacity: tile.toggleable && !tile.on ? 0.45 : 0
        }
    }
    Text {
        id: caption
        anchors.top: picture.bottom
        anchors.topMargin: 4
        width: parent.width
        text: tile.label
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        font.pixelSize: Math.max(10, (tile.theme.fontPixelSize || 13) - 2)
        font.family: tile.theme.fontFamily || ""
        color: tile.on && tile.toggleable ? tile.theme.accent : tile.theme.foreground
    }
    TapHandler {
        enabled: tile.toggleable
        // Exclusive grab: a tap on chrome must not also reach the map.
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: tile.clicked()
    }
    Accessible.role: tile.toggleable ? Accessible.CheckBox : Accessible.StaticText
    Accessible.name: tile.tooltip.length > 0 ? tile.label + ". " + tile.tooltip : tile.label
}
