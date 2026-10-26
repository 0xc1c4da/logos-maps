import QtQuick

// Layers panel (M8.20): a sheet with (a) the basemap's details, (b) every
// group from every session (toggle + legend; non-toggleable groups list the
// legend only) and (c) the basemap catalogue as thumbnail tiles with a
// selection ring. Everything it shows is scene metadata (M6.3a, §9.1); it
// never knows module data. Toggles go through ChromeController, which calls
// the core so other views and modules see the change.
Item {
    id: panel
    property var theme: chrome ? chrome.theme : ({})
    property var padding: chrome ? chrome.padding : ({})
    readonly property bool open: chrome ? chrome.panelOpen : false
    visible: open
    width: Math.min(300, parent.width - 24 - (padding.left || 0) - (padding.right || 0))
    height: Math.min(content.implicitHeight + 24, parent.height - 24 - (padding.top || 0) - (padding.bottom || 0))

    // Taps inside stay inside; the map below does not get them.
    TapHandler { gesturePolicy: TapHandler.WithinBounds }
    WheelHandler { target: null; onWheel: (ev) => flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY - ev.angleDelta.y)) }

    Rectangle {
        anchors.fill: parent
        radius: panel.theme.radius !== undefined ? panel.theme.radius : 6
        color: panel.theme.background
        border.color: panel.theme.border
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 12
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            width: flick.width
            spacing: 10

            Row {
                width: parent.width
                Text {
                    width: parent.width - closeBtn.width
                    text: qsTr("Layers")
                    font.pixelSize: (panel.theme.fontPixelSize || 13) + 2
                    font.bold: true
                    font.family: panel.theme.fontFamily || ""
                    color: panel.theme.foreground
                }
                Text {
                    id: closeBtn
                    text: "✕"
                    font.pixelSize: panel.theme.fontPixelSize || 13
                    color: panel.theme.foreground
                    TapHandler { onTapped: chrome.setPanelOpen(false) }
                }
            }

            // (a) basemap details
            Column {
                width: parent.width
                spacing: 4
                visible: chrome && chrome.details.length > 0
                Text {
                    text: qsTr("Map details")
                    font.pixelSize: Math.max(10, (panel.theme.fontPixelSize || 13) - 1)
                    font.family: panel.theme.fontFamily || ""
                    color: panel.theme.foreground
                    opacity: 0.7
                }
                Repeater {
                    model: chrome ? chrome.details : []
                    delegate: ToggleRow {
                        width: content.width
                        theme: panel.theme
                        title: modelData.title || modelData.id
                        checked: modelData.visible
                        toggleable: true
                        onToggled: (on) => chrome.setDetailVisible(modelData.id, on)
                    }
                }
            }

            // (b) groups
            Column {
                width: parent.width
                spacing: 4
                visible: chrome && chrome.groups.length > 0
                Text {
                    text: qsTr("Content")
                    font.pixelSize: Math.max(10, (panel.theme.fontPixelSize || 13) - 1)
                    font.family: panel.theme.fontFamily || ""
                    color: panel.theme.foreground
                    opacity: 0.7
                }
                Repeater {
                    model: chrome ? chrome.groups : []
                    delegate: Column {
                        width: content.width
                        spacing: 2
                        ToggleRow {
                            width: parent.width
                            theme: panel.theme
                            title: modelData.title || modelData.id
                            subtitle: modelData.description || ""
                            icon: modelData.icon || ""
                            checked: modelData.visible
                            toggleable: modelData.toggleable !== false
                            onToggled: (on) => chrome.setGroupVisible(modelData.id, on)
                        }
                        Flow {
                            width: parent.width
                            leftPadding: 28
                            spacing: 8
                            visible: (modelData.legend || []).length > 0
                            Repeater {
                                model: modelData.legend || []
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

            // (c) basemap catalogue
            Column {
                width: parent.width
                spacing: 4
                visible: chrome && chrome.basemaps.length > 0
                Text {
                    text: qsTr("Basemap")
                    font.pixelSize: Math.max(10, (panel.theme.fontPixelSize || 13) - 1)
                    font.family: panel.theme.fontFamily || ""
                    color: panel.theme.foreground
                    opacity: 0.7
                }
                Flow {
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: chrome ? chrome.basemaps : []
                        delegate: Column {
                            spacing: 2
                            readonly property bool current: chrome && chrome.basemap === modelData.id
                            Rectangle {
                                width: 64; height: 64
                                radius: panel.theme.radius !== undefined ? panel.theme.radius : 6
                                color: panel.theme.surface
                                border.color: current ? panel.theme.accent : panel.theme.border
                                border.width: current ? 3 : 1
                                Image {
                                    anchors.fill: parent
                                    anchors.margins: current ? 3 : 1
                                    source: modelData.thumbnail || ""
                                    fillMode: Image.PreserveAspectCrop
                                    visible: !!modelData.thumbnail
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: !modelData.thumbnail
                                    text: (modelData.name || modelData.id || "?").substring(0, 1).toUpperCase()
                                    font.pixelSize: 22
                                    color: panel.theme.foreground
                                    opacity: 0.6
                                }
                                TapHandler { onTapped: chrome.selectBasemap(modelData.id) }
                            }
                            Text {
                                width: 64
                                text: modelData.name || modelData.id
                                elide: Text.ElideRight
                                horizontalAlignment: Text.AlignHCenter
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
