import QtQuick

// Layers panel (M8.20), in the form Vela's `LayersPanel.kt` and Google
// Maps' layer sheet share: a popup under the layers button with titled
// sections of picture tiles, four to a row, the chosen or switched-on tile
// ringed in the accent colour.
//
//   Map type     the basemap catalogue (thumbnail or glyph, `label`)
//   Map details  the current basemap's own switchable parts (§9.1a)
//   Content      every session's groups (icon, legend swatches or glyph);
//                a group's legend, when it has one, is spelled out under
//                the tiles; non-toggleable groups show legend only
//
// Everything here is scene metadata (M6.3a, §9.1); the panel never knows
// module data. Toggles go through ChromeController, which calls the core so
// other views and modules see the change.
Item {
    id: panel
    property var theme: chrome ? chrome.theme : ({})
    property var padding: chrome ? chrome.padding : ({})
    readonly property bool open: chrome ? chrome.panelOpen : false
    visible: open

    readonly property int columns: 4
    readonly property int tileWidth: 68
    readonly property int tileGap: 6
    readonly property int inset: 12
    readonly property int gridWidth: columns * tileWidth + (columns - 1) * tileGap
    width: Math.min(gridWidth + inset * 2, parent.width - 24 - (padding.left || 0) - (padding.right || 0))
    height: Math.min(content.implicitHeight + inset * 2, parent.height - 24 - (padding.top || 0) - (padding.bottom || 0))

    // Taps inside stay inside; the map below does not get them.
    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds }
    WheelHandler { target: null; onWheel: (ev) => flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY - ev.angleDelta.y)) }

    Rectangle {
        anchors.fill: parent
        radius: panel.theme.radius !== undefined ? panel.theme.radius : 6
        color: panel.theme.background
        border.color: panel.theme.border
    }

    component SectionTitle: Text {
        font.pixelSize: Math.max(10, (panel.theme.fontPixelSize || 13) - 1)
        font.family: panel.theme.fontFamily || ""
        color: panel.theme.foreground
        opacity: 0.7
        topPadding: 6
        bottomPadding: 2
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: panel.inset
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            width: flick.width
            spacing: 2

            // Header: title and close (Google's sheet; Vela's popup relies on tap-outside, which we also do).
            Item {
                width: parent.width
                height: Math.max(title.implicitHeight, closeBtn.implicitHeight)
                Text {
                    id: title
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Layers")
                    font.pixelSize: (panel.theme.fontPixelSize || 13) + 2
                    font.bold: true
                    font.family: panel.theme.fontFamily || ""
                    color: panel.theme.foreground
                }
                Text {
                    id: closeBtn
                    objectName: "layersPanelClose"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\u2715"
                    font.pixelSize: panel.theme.fontPixelSize || 13
                    color: panel.theme.foreground
                    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds; onTapped: chrome.setPanelOpen(false) }
                }
            }

            // Map type: the basemap catalogue.
            SectionTitle { text: qsTr("Map type"); visible: chrome && chrome.basemaps.length > 0 }
            Grid {
                objectName: "basemapTiles"
                visible: chrome && chrome.basemaps.length > 0
                columns: panel.columns
                columnSpacing: panel.tileGap
                rowSpacing: panel.tileGap
                Repeater {
                    model: chrome ? chrome.basemaps : []
                    delegate: LayerTile {
                        theme: panel.theme
                        label: modelData.label || modelData.name || modelData.id
                        tooltip: modelData.name || ""
                        image: modelData.thumbnail || ""
                        glyph: (modelData.label || modelData.name || modelData.id || "?").substring(0, 1).toUpperCase()
                        on: chrome && chrome.basemap === modelData.id
                        onClicked: chrome.selectBasemap(modelData.id)
                    }
                }
            }

            // Map details: the basemap's own switchable parts.
            SectionTitle { text: qsTr("Map details"); visible: chrome && chrome.details.length > 0 }
            Grid {
                objectName: "detailTiles"
                visible: chrome && chrome.details.length > 0
                columns: panel.columns
                columnSpacing: panel.tileGap
                rowSpacing: panel.tileGap
                Repeater {
                    model: chrome ? chrome.details : []
                    delegate: LayerTile {
                        theme: panel.theme
                        label: modelData.title || modelData.id
                        image: modelData.icon || ""
                        glyph: (modelData.title || modelData.id || "?").substring(0, 1).toUpperCase()
                        on: modelData.visible
                        onClicked: chrome.setDetailVisible(modelData.id, !modelData.visible)
                    }
                }
            }

            // Content: every session's groups.
            SectionTitle { text: qsTr("Content"); visible: chrome && chrome.groups.length > 0 }
            Grid {
                objectName: "groupTiles"
                visible: chrome && chrome.groups.length > 0
                columns: panel.columns
                columnSpacing: panel.tileGap
                rowSpacing: panel.tileGap
                Repeater {
                    model: chrome ? chrome.groups : []
                    delegate: LayerTile {
                        theme: panel.theme
                        label: modelData.title || modelData.id
                        tooltip: modelData.description || ""
                        image: modelData.icon || ""
                        legend: modelData.legend || []
                        glyph: (modelData.title || modelData.id || "?").substring(0, 1).toUpperCase()
                        on: modelData.visible
                        toggleable: modelData.toggleable !== false
                        onClicked: chrome.setGroupVisible(modelData.id, !modelData.visible)
                    }
                }
            }
            // Legends, spelled out: "<group>: <swatch> label, <swatch> label".
            Column {
                width: parent.width
                spacing: 2
                topPadding: 4
                visible: chrome && chrome.groups.some((g) => (g.legend || []).length > 0)
                Repeater {
                    model: chrome ? chrome.groups.filter((g) => (g.legend || []).length > 0) : []
                    delegate: Flow {
                        width: content.width
                        spacing: 8
                        Text {
                            text: (modelData.title || modelData.id) + ":"
                            font.pixelSize: Math.max(10, (panel.theme.fontPixelSize || 13) - 2)
                            font.family: panel.theme.fontFamily || ""
                            color: panel.theme.foreground
                            opacity: 0.7
                        }
                        Repeater {
                            model: modelData.legend
                            delegate: Row {
                                spacing: 4
                                Rectangle {
                                    width: 10; height: 10; radius: 5
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: modelData.color || "transparent"
                                    border.color: panel.theme.border
                                    visible: !modelData.image
                                }
                                Image {
                                    width: 12; height: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    source: modelData.image || ""
                                    visible: !!modelData.image
                                }
                                Text {
                                    text: modelData.label || ""
                                    font.pixelSize: Math.max(10, (panel.theme.fontPixelSize || 13) - 2)
                                    font.family: panel.theme.fontFamily || ""
                                    color: panel.theme.foreground
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
