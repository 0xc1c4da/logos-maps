import QtQuick
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

// Maps -- the host-side view (docs/architecture.md section 12.3).
//
// The map itself is rendered by the maps_view process into its own window
// (RFC-1); this QML only hosts that surface and the chrome around it. Until
// the host ships Logos.Surface (RFC-3, M7) the surface slot below shows the
// view's status and the attribution the backend reports, so the module is
// loadable on today's Basecamp without drawing anything it should not.
// Peer-supplied strings render as plain text, never as rich text.
Item {
    id: root

    // Typed replica of the C++ backend (see src/maps_view.rep).
    readonly property var backend: logos.module("maps_view")
    property bool ready: false
    readonly property string status: backend ? backend.status : ""
    readonly property string viewId: backend ? backend.viewId : ""
    readonly property string surfaceToken: backend ? backend.surfaceToken : ""
    property string attribution: ""
    property var viewState: ({})

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "maps_view") {
                root.ready = isReady && root.backend !== null
                if (root.ready)
                    root.backend.setSize(surfaceSlot.width, surfaceSlot.height, Screen.devicePixelRatio)
            }
        }
    }
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onAttributionChanged(html) { root.attribution = String(html) }
        function onViewStateJsonChanged() {
            try { root.viewState = JSON.parse(String(root.backend.viewStateJson)) } catch (e) { root.viewState = ({}) }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // RFC-3 slot: M7 replaces this Item with
        //   Logos.Surface { module: "maps_view"; session: "default" }
        Rectangle {
            id: surfaceSlot
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.palette.background
            onWidthChanged: if (root.ready) root.backend.setSize(width, height, Screen.devicePixelRatio)
            onHeightChanged: if (root.ready) root.backend.setSize(width, height, Screen.devicePixelRatio)

            Column {
                anchors.centerIn: parent
                spacing: 8
                Text {
                    text: root.ready ? qsTr("maps_view: %1").arg(root.status) : qsTr("Starting maps view…")
                    color: Theme.palette.text
                    textFormat: Text.PlainText
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Text {
                    visible: root.viewId.length > 0
                    text: root.viewId
                    color: Theme.palette.textSecondary
                    textFormat: Text.PlainText
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Text {
                    visible: root.viewState && root.viewState.camera !== undefined
                    text: root.viewState && root.viewState.camera
                          ? qsTr("z %1  @ %2, %3").arg(Number(root.viewState.camera.zoom).toFixed(2))
                                .arg(Number(root.viewState.camera.center[0]).toFixed(4))
                                .arg(Number(root.viewState.camera.center[1]).toFixed(4))
                          : ""
                    color: Theme.palette.textSecondary
                    textFormat: Text.PlainText
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }

        // Attribution bar: the backend composes it (section 9.9); links are
        // the descriptor's, rendered as plain text until the host vets them.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: attributionText.implicitHeight + 8
            visible: root.attribution.length > 0
            color: Theme.palette.backgroundSecondary
            Text {
                id: attributionText
                anchors.fill: parent
                anchors.margins: 4
                text: root.attribution.replace(/<[^>]*>/g, "")
                color: Theme.palette.textTertiary
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: 11
            }
        }
    }
}
