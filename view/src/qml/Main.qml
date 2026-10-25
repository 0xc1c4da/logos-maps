import QtQuick
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls
import Logos.Surface

// Maps -- the host-side view (docs/architecture.md section 12.3).
//
// The map is rendered by the maps_view process (RFC-1); this QML only hosts
// its surface and the chrome around it. The surface arrives through the
// host's Logos.Surface item (RFC-3): Basecamp composites the backend's
// buffer into this tab, and nothing of the engine runs in the host process.
// On a host without a compositor (RFC-3 "noCompositor") the backend opens
// its own window instead and the slot below shows the view's status.
// Peer-supplied strings render as plain text, never as rich text.
Item {
    id: root

    // Typed replica of the C++ backend (see src/maps_view.rep).
    readonly property var backend: logos.module("maps_view")
    property bool ready: false
    readonly property string status: backend ? backend.status : ""
    readonly property string viewId: backend ? backend.viewId : ""
    property string attribution: ""
    // Parsed section 7.5 view state; a binding, so the initial value counts too.
    readonly property var viewState: parseState(backend ? backend.viewStateJson : "")
    // Last Logos.Surface error code ("" while the surface is fine).
    property string surfaceError: ""

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "maps_view") {
                root.ready = isReady && root.backend !== null
                if (root.ready)
                    root.backend.setSize(surface.width, surface.height, Screen.devicePixelRatio)
            }
        }
    }
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onAttributionChanged(html) { root.attribution = String(html) }
    }

    function parseState(json) {
        try { return json ? JSON.parse(String(json)) : ({}) } catch (e) { return ({}) }
    }

    // The attribution is HTML (links, entities); shown as plain text.
    function plainText(html) {
        return String(html).replace(/<[^>]*>/g, "")
            .replace(/&copy;/g, "\u00a9").replace(/&amp;/g, "&").replace(/&lt;/g, "<")
            .replace(/&gt;/g, ">").replace(/&quot;/g, "\"").replace(/&#39;/g, "'").replace(/&nbsp;/g, " ")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The surface slot. Size and DPR reach the backend as xdg configure
        // events from the host (RFC-5 rule 2); setSize is informative and
        // only matters on the no-compositor fallback.
        Surface {
            id: surface
            objectName: "maps.surface"   // tests/basecamp-surface.mjs
            Layout.fillWidth: true
            Layout.fillHeight: true
            module: "maps_view"
            session: "basecamp/main"
            placeholderColor: Theme.palette.background
            onWidthChanged: if (root.ready) root.backend.setSize(width, height, Screen.devicePixelRatio)
            onHeightChanged: if (root.ready) root.backend.setSize(width, height, Screen.devicePixelRatio)
            onError: (code) => root.surfaceError = code
            onReadyChanged: if (ready) root.surfaceError = ""

            // Status until the first frame, or when the surface is gone.
            Column {
                anchors.centerIn: parent
                spacing: 8
                visible: !surface.ready
                Text {
                    text: root.surfaceError.length > 0
                          ? qsTr("maps_view surface: %1").arg(root.surfaceError)
                          : root.ready ? qsTr("maps_view: %1").arg(root.status) : qsTr("Starting maps view…")
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
            }

            // Camera readout over the map (section 7.5 view state).
            Rectangle {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 8
                visible: surface.ready && root.viewState && root.viewState.camera !== undefined
                width: cameraText.implicitWidth + 12
                height: cameraText.implicitHeight + 6
                radius: 3
                color: Theme.palette.backgroundSecondary
                opacity: 0.85
                Text {
                    id: cameraText
                    anchors.centerIn: parent
                    text: root.viewState && root.viewState.camera
                          ? qsTr("z %1  @ %2, %3").arg(Number(root.viewState.camera.zoom).toFixed(2))
                                .arg(Number(root.viewState.camera.center[0]).toFixed(4))
                                .arg(Number(root.viewState.camera.center[1]).toFixed(4))
                          : ""
                    color: Theme.palette.textSecondary
                    textFormat: Text.PlainText
                    font.pixelSize: 11
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
                text: root.plainText(root.attribution)
                color: Theme.palette.textTertiary
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: 11
            }
        }
    }
}
