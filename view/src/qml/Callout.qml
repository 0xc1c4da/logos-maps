import QtQuick

// Callout (M8.19): one card per view anchored at a geographic point that
// ChromeController tracks per frame (`chrome.callout.px`). Prefers to sit
// above the point; flips below / left / right when it would leave the
// padded viewport (gl-js auto-anchor). Max width 240 dp. Title, subtitle,
// icon, up to 8 actions; `{prop}` templates were expanded by the controller.
// Never part of the scene or the pick results.
Item {
    id: callout
    property var theme: chrome ? chrome.theme : ({})
    property var padding: chrome ? chrome.padding : ({})
    readonly property var model: chrome ? chrome.callout : ({ open: false })
    readonly property bool open: !!model.open
    readonly property real ax: model.px ? model.px[0] : 0
    readonly property real ay: model.px ? model.px[1] : 0
    readonly property real tip: 8       // pointer triangle height
    readonly property real gap: 10      // distance from the anchor point
    readonly property bool onScreen: parent ? (ax >= -width && ay >= -height && ax <= parent.width + width && ay <= parent.height + height) : false
    visible: open && onScreen

    // Which side of the anchor the card sits on.
    readonly property string side: {
        if (!parent) return "top"
        const l = padding.left || 0, t = padding.top || 0
        const r = parent.width - (padding.right || 0), b = parent.height - (padding.bottom || 0)
        if (ay - gap - card.height >= t) return "top"
        if (ay + gap + card.height <= b) return "bottom"
        if (ax + gap + card.width <= r) return "right"
        if (ax - gap - card.width >= l) return "left"
        return "top"
    }

    width: card.width
    height: card.height
    x: {
        if (!parent) return 0
        const l = padding.left || 0, r = parent.width - (padding.right || 0)
        let cx
        if (side === "right") cx = ax + gap
        else if (side === "left") cx = ax - gap - card.width
        else cx = ax - card.width / 2
        return Math.max(l + 4, Math.min(r - card.width - 4, cx))
    }
    y: {
        if (!parent) return 0
        const t = padding.top || 0, b = parent.height - (padding.bottom || 0)
        let cy
        if (side === "top") cy = ay - gap - card.height
        else if (side === "bottom") cy = ay + gap
        else cy = ay - card.height / 2
        return Math.max(t + 4, Math.min(b - card.height - 4, cy))
    }

    // Pointer triangle towards the anchor.
    Canvas {
        id: pointer
        width: 16
        height: 16
        x: callout.side === "right" ? -callout.tip : callout.side === "left" ? card.width - 0.5 : Math.max(6, Math.min(card.width - 22, callout.ax - callout.x - 8))
        y: callout.side === "bottom" ? -callout.tip : callout.side === "top" ? card.height - 0.5 : Math.max(6, Math.min(card.height - 22, callout.ay - callout.y - 8))
        property string side: callout.side
        property color bg: callout.theme.background
        property color br: callout.theme.border
        onSideChanged: requestPaint()
        onBgChanged: requestPaint()
        onBrChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = bg
            ctx.strokeStyle = br
            ctx.lineWidth = 1
            ctx.beginPath()
            if (side === "top") { ctx.moveTo(0, 0); ctx.lineTo(16, 0); ctx.lineTo(8, callout.tip) }
            else if (side === "bottom") { ctx.moveTo(0, callout.tip); ctx.lineTo(16, callout.tip); ctx.lineTo(8, 0) }
            else if (side === "right") { ctx.moveTo(callout.tip, 0); ctx.lineTo(callout.tip, 16); ctx.lineTo(0, 8) }
            else { ctx.moveTo(0, 0); ctx.lineTo(0, 16); ctx.lineTo(callout.tip, 8) }
            ctx.closePath()
            ctx.fill()
            ctx.stroke()
        }
    }

    Rectangle {
        id: card
        width: Math.min(240, Math.max(120, body.implicitWidth + 24))
        height: body.implicitHeight + 20
        radius: callout.theme.radius !== undefined ? callout.theme.radius : 6
        color: callout.theme.background
        border.color: callout.theme.border

        // Taps on the card stay on the card.
        TapHandler { gesturePolicy: TapHandler.WithinBounds }

        Column {
            id: body
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 10
            width: parent.width - 20
            spacing: 6

            Row {
                width: parent.width
                spacing: 8
                Image {
                    width: callout.model.icon ? 20 : 0
                    height: 20
                    source: callout.model.icon || ""
                    visible: !!callout.model.icon
                    fillMode: Image.PreserveAspectFit
                    anchors.verticalCenter: parent.verticalCenter
                }
                Column {
                    width: parent.width - (callout.model.icon ? 28 : 0) - 20
                    Text {
                        objectName: "calloutTitle"
                        width: parent.width
                        text: callout.model.title || ""
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        font.pixelSize: callout.theme.fontPixelSize || 13
                        font.bold: true
                        font.family: callout.theme.fontFamily || ""
                        color: callout.theme.foreground
                    }
                    Text {
                        width: parent.width
                        visible: !!callout.model.subtitle
                        text: callout.model.subtitle || ""
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        font.pixelSize: Math.max(10, (callout.theme.fontPixelSize || 13) - 1)
                        font.family: callout.theme.fontFamily || ""
                        color: callout.theme.foreground
                        opacity: 0.75
                    }
                }
                Text {
                    text: "✕"
                    font.pixelSize: callout.theme.fontPixelSize || 13
                    color: callout.theme.foreground
                    opacity: 0.7
                    TapHandler { onTapped: chrome.closeCallout("tap") }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Close")
                }
            }

            Flow {
                width: parent.width
                spacing: 6
                visible: (callout.model.actions || []).length > 0
                Repeater {
                    model: callout.model.actions || []
                    delegate: Rectangle {
                        width: actionLabel.implicitWidth + 16
                        height: actionLabel.implicitHeight + 8
                        radius: callout.theme.radius !== undefined ? callout.theme.radius : 6
                        color: actionTap.pressed ? callout.theme.accent : callout.theme.surface
                        border.color: callout.theme.accent
                        Text {
                            id: actionLabel
                            anchors.centerIn: parent
                            text: modelData.label || modelData.id
                            font.pixelSize: Math.max(10, (callout.theme.fontPixelSize || 13) - 1)
                            font.family: callout.theme.fontFamily || ""
                            color: actionTap.pressed ? callout.theme.background : callout.theme.accent
                        }
                        TapHandler {
                            id: actionTap
                            gesturePolicy: TapHandler.ReleaseWithinBounds
                            onTapped: chrome.calloutAction(modelData.id)
                        }
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.label || modelData.id
                    }
                }
            }
        }
    }
}
