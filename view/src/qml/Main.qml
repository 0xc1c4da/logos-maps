import QtQuick
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls
import Logos.Surface

// Maps -- the host-side view (docs/architecture.md section 12.3).
//
// The map is rendered by the maps_view process (RFC-1); this QML only hosts
// its surface. The surface arrives through the host's Logos.Surface item
// (RFC-3): Basecamp composites the backend's buffer into this tab, and
// nothing of the engine -- and none of the map's chrome (section 8.4) --
// runs in the host process. The one thing the host contributes is its
// theme, forwarded as the `theme` view option (M8.23) so the view's chrome
// matches the shell. On a host without a compositor (RFC-3 "noCompositor")
// the backend opens its own window instead and the slot below shows the
// view's status. Peer-supplied strings render as plain text, never as rich
// text.
Item {
    id: root

    // Typed replica of the C++ backend (see src/maps_view.rep).
    readonly property var backend: logos.module("maps_view")
    property bool ready: false
    readonly property string status: backend ? backend.status : ""
    readonly property string viewId: backend ? backend.viewId : ""
    // Last Logos.Surface error code ("" while the surface is fine).
    property string surfaceError: ""

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "maps_view") {
                root.ready = isReady && root.backend !== null
                if (root.ready) {
                    root.backend.setSize(surface.width, surface.height, Screen.devicePixelRatio)
                    root.pushTheme()
                }
            }
        }
    }
    // M8.23: the subset of Logos.Theme the view's chrome needs, re-sent on
    // every theme change. Colours travel as "#aarrggbb" strings.
    Connections {
        target: Theme
        function onCurrentThemeChanged() { root.pushTheme() }
    }

    function themeOptions() {
        const p = Theme.palette
        return {
            dark: Theme.isDark,
            accent: String(p.primary),
            foreground: String(p.text),
            background: String(p.background),
            surface: String(p.surface),
            border: String(p.border),
            radius: Theme.spacing.radiusMedium,
            fontFamily: Theme.typography.publicSans,
            fontPixelSize: Theme.typography.primaryText
        }
    }
    function pushTheme() {
        if (!root.ready || !root.backend) return
        root.backend.setOptions(JSON.stringify({ v: 1, theme: root.themeOptions() }))
    }

    // Section 12.2 shareRequested: the view's window is render-controlled
    // and unfocused under the channel transport, so copy/share from the
    // chrome (coordinate readout, dropped pin) is completed here, in the
    // host, through a hidden TextEdit -- the one clipboard path plain QML
    // has. The text is a coordinate or a geo: URI the view composed.
    Connections {
        target: root.backend
        ignoreUnknownSignals: true
        function onShareRequested(json) {
            let req
            try { req = JSON.parse(json) } catch (e) { return }
            if (!req || typeof req.text !== "string" || req.text.length === 0) return
            clipboardSink.text = req.text
            clipboardSink.selectAll()
            clipboardSink.copy()
            clipboardSink.deselect()
        }
    }
    TextEdit {
        id: clipboardSink
        visible: false
        width: 1; height: 1
        textFormat: TextEdit.PlainText
    }

    // The surface slot. Size and DPR reach the backend as xdg configure
    // events from the host (RFC-5 rule 2); setSize is informative and
    // only matters on the no-compositor fallback.
    Surface {
        id: surface
        objectName: "maps.surface"   // tests/basecamp-surface.mjs
        anchors.fill: parent
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
    }
}
