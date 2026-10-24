import QtQuick
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

// Maps -- the view. Pure rendering: state comes from the backend replica,
// actions go to its slots. Every peer-supplied string is rendered as plain
// text (textFormat below) so nothing is ever parsed as rich text.
Item {
    id: root

    // Typed replica of the C++ backend (see src/maps_ui.rep).
    readonly property var backend: logos.module("maps_ui")
    property bool ready: false
    readonly property string status: backend ? backend.status : ""
    property string lastReply: ""

    Connections {
        target: logos
        function onViewModuleReadyChanged(moduleName, isReady) {
            if (moduleName === "maps_ui")
                root.ready = isReady && root.backend !== null
        }
    }
    Component.onCompleted: {
        root.ready = root.backend !== null && logos.isViewModuleReady("maps_ui")
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xlarge
        spacing: Theme.spacing.large

        LogosText {
            objectName: "maps_ui.title"
            text: "Maps"
            textFormat: Text.PlainText
            color: Theme.palette.text
            font.pixelSize: Theme.typography.panelTitleText
            font.weight: Theme.typography.weightBold
        }

        LogosText {
            objectName: "maps_ui.connection"
            text: root.ready ? qsTr("Backend connected") : qsTr("Connecting to backend...")
            textFormat: Text.PlainText
            color: root.ready ? Theme.palette.success : Theme.palette.warning
        }

        RowLayout {
            spacing: Theme.spacing.medium
            Layout.fillWidth: true

            LogosTextField {
                id: messageField
                objectName: "maps_ui.message"
                Layout.fillWidth: true
                placeholderText: qsTr("Say something to the backend")
            }

            LogosButton {
                objectName: "maps_ui.ping"
                text: qsTr("Ping")
                enabled: root.ready
                onClicked: {
                    // Replies are pending QtRO calls; logos.watch() delivers them.
                    logos.watch(root.backend.ping(messageField.text), function (value) {
                        root.lastReply = String(value)
                    }, function (error) {
                        root.lastReply = qsTr("Error: ") + String(error)
                    })
                }
            }
        }

        LogosText {
            objectName: "maps_ui.reply"
            text: root.lastReply.length > 0 ? root.lastReply : qsTr("Press Ping to call the backend")
            textFormat: Text.PlainText
            color: Theme.palette.text
        }

        LogosText {
            objectName: "maps_ui.status"
            text: qsTr("Backend status: ") + root.status
            textFormat: Text.PlainText
            color: Theme.palette.textSecondary
        }

        Item { Layout.fillHeight: true }
    }
}
