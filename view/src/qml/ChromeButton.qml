import QtQuick

// A themed square chrome button (M8.23): every colour comes from
// `chrome.theme`, nothing is hard-coded. Content goes in the default slot.
Item {
    id: button
    property var theme: chrome ? chrome.theme : ({})
    property bool enabled: true
    property bool active: false       // accent ring (e.g. follow on)
    property string tooltip: ""
    default property alias content: slot.data
    signal clicked()

    width: 36
    height: 36
    opacity: enabled ? 1.0 : 0.45

    Rectangle {
        anchors.fill: parent
        radius: button.theme.radius !== undefined ? button.theme.radius : 6
        color: tap.pressed && button.enabled ? button.theme.surface : button.theme.background
        border.color: button.active ? button.theme.accent : button.theme.border
        border.width: button.active ? 2 : 1
    }
    Item {
        id: slot
        anchors.fill: parent
    }
    TapHandler {
        id: tap
        enabled: button.enabled
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: button.clicked()
    }
    HoverHandler { id: hover }
    Accessible.role: Accessible.Button
    Accessible.name: button.tooltip
}
