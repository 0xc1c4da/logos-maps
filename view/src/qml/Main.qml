import QtQuick
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

// Maps -- the view. Pure rendering: the layer registry lives in maps_core
// (so any module can add layers programmatically); this view mirrors it
// through the maps_view backend replica and refetches on layersChanged.
// Peer-supplied strings render as plain text, never as rich text.
Item {
    id: root

    // Typed replica of the C++ backend (see src/maps_view.rep).
    readonly property var backend: logos.module("maps_view")
    property bool ready: false
    readonly property string status: backend ? backend.status : ""
    readonly property int layerCount: backend ? backend.layerCount : 0

    // [{id, layer}] from maps_core.listLayers(), parsed in the view.
    property var layers: []

    function refresh() {
        if (!root.ready || !root.backend)
            return
        logos.watch(root.backend.listLayers(), function (json) {
            try {
                root.layers = JSON.parse(String(json))
            } catch (e) {
                root.layers = []
            }
        }, function (error) {
            root.layers = []
        })
    }

    function addLayerFromForm() {
        var id = layerIdField.text.trim()
        if (id.length === 0)
            return
        var title = layerTitleField.text.trim()
        var layer = { title: title.length > 0 ? title : id, kind: "markers", visible: true }
        logos.watch(root.backend.addLayer(id, JSON.stringify(layer)), function (ok) {
            if (ok) {
                layerIdField.text = ""
                layerTitleField.text = ""
            }
        }, function (error) {})
    }

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "maps_view") {
                root.ready = isReady && root.backend !== null
                root.refresh()
            }
        }
    }
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onLayersChanged() { root.refresh() }
    }
    Component.onCompleted: {
        root.ready = root.backend !== null && logos.isViewModuleReady("maps_view")
        root.refresh()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xlarge
        spacing: Theme.spacing.large

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.medium

            LogosText {
                objectName: "maps_view.title"
                text: qsTr("Maps")
                textFormat: Text.PlainText
                color: Theme.palette.text
                font.pixelSize: Theme.typography.panelTitleText
                font.weight: Theme.typography.weightBold
            }
            Item { Layout.fillWidth: true }
            LogosText {
                objectName: "maps_view.connection"
                text: root.ready ? qsTr("%n layer(s)", "", root.layerCount) : qsTr("Connecting to backend...")
                textFormat: Text.PlainText
                color: root.ready ? Theme.palette.textSecondary : Theme.palette.warning
            }
        }

        // The map canvas goes here. Until a tile/vector renderer is wired in,
        // this frame stands in for it and lists what the registry holds.
        LogosFrame {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacing.large
                spacing: Theme.spacing.small

                LogosText {
                    text: qsTr("Layers")
                    textFormat: Text.PlainText
                    color: Theme.palette.textSecondary
                    font.weight: Theme.typography.weightBold
                }

                LogosText {
                    visible: root.layers.length === 0
                    text: root.ready ? qsTr("No layers yet. Add one below, or from any module via maps_core.addLayer().")
                                     : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    color: Theme.palette.textSecondary
                }

                LogosListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.layers
                    delegate: LogosListRow {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : implicitWidth
                        objectName: "maps_view.layer." + modelData.id
                        title: modelData.layer && modelData.layer.title ? String(modelData.layer.title) : String(modelData.id)
                        subtitle: String(modelData.id) + (modelData.layer && modelData.layer.kind ? "  -  " + String(modelData.layer.kind) : "")
                        accessory: LogosButton {
                            text: qsTr("Remove")
                            compact: true
                            onClicked: logos.watch(root.backend.removeLayer(modelData.id), function (ok) {}, function (e) {})
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.medium

            LogosTextField {
                id: layerIdField
                objectName: "maps_view.layerId"
                Layout.preferredWidth: 160
                placeholderText: qsTr("layer id")
                Keys.onReturnPressed: root.addLayerFromForm()
                Keys.onEnterPressed: root.addLayerFromForm()
            }
            LogosTextField {
                id: layerTitleField
                objectName: "maps_view.layerTitle"
                Layout.fillWidth: true
                placeholderText: qsTr("title (optional)")
                Keys.onReturnPressed: root.addLayerFromForm()
                Keys.onEnterPressed: root.addLayerFromForm()
            }
            LogosButton {
                objectName: "maps_view.addLayer"
                text: qsTr("Add layer")
                variant: LogosButton.Variant.Primary
                enabled: root.ready && layerIdField.text.trim().length > 0
                onClicked: root.addLayerFromForm()
            }
        }

        LogosText {
            objectName: "maps_view.status"
            text: qsTr("maps_core: ") + root.status
            textFormat: Text.PlainText
            color: Theme.palette.textSecondary
        }
    }
}
