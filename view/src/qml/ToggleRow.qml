import QtQuick

// One row of the layers panel: optional icon, title, subtitle and a themed
// switch. Non-toggleable rows show no switch (legend-only groups, M8.20).
Item {
    id: row
    property var theme: ({})
    property string title: ""
    property string subtitle: ""
    property string icon: ""
    property bool checked: true
    property bool toggleable: true
    signal toggled(bool on)

    implicitHeight: Math.max(28, labels.implicitHeight + 6)
    height: implicitHeight

    Image {
        id: iconItem
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: row.icon ? 18 : 0
        height: 18
        source: row.icon
        visible: !!row.icon
        fillMode: Image.PreserveAspectFit
    }
    Column {
        id: labels
        anchors.left: iconItem.right
        anchors.leftMargin: row.icon ? 8 : 0
        anchors.right: toggle.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        Text {
            width: parent.width
            text: row.title
            elide: Text.ElideRight
            font.pixelSize: row.theme.fontPixelSize || 13
            font.family: row.theme.fontFamily || ""
            color: row.theme.foreground
        }
        Text {
            width: parent.width
            visible: row.subtitle.length > 0
            text: row.subtitle
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            font.pixelSize: Math.max(10, (row.theme.fontPixelSize || 13) - 2)
            font.family: row.theme.fontFamily || ""
            color: row.theme.foreground
            opacity: 0.7
        }
    }
    Rectangle {
        id: toggle
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: row.toggleable ? 34 : 0
        height: 18
        radius: 9
        visible: row.toggleable
        color: row.checked ? row.theme.accent : row.theme.surface
        border.color: row.checked ? row.theme.accent : row.theme.border
        Rectangle {
            width: 14; height: 14; radius: 7
            anchors.verticalCenter: parent.verticalCenter
            x: row.checked ? parent.width - width - 2 : 2
            color: row.theme.background
            border.color: row.theme.border
            Behavior on x { NumberAnimation { duration: 120 } }
        }
    }
    TapHandler {
        enabled: row.toggleable
        onTapped: row.toggled(!row.checked)
    }
    Accessible.role: Accessible.CheckBox
    Accessible.name: row.title
    Accessible.checked: row.checked
}
