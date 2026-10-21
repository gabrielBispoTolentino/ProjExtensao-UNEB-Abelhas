import QtQuick
import QtQuick.Controls.Basic

// Botão padrão do app. kind: "primary" (mel, a ação principal da tela),
// "danger" (apaga algo) ou "secondary" (só contorno).
Button {
    id: control

    property string kind: "secondary"

    readonly property color fillColor: kind === "primary" ? (down ? Theme.honeyDeep : Theme.honey)
                                     : kind === "danger" ? (down ? Qt.darker(Theme.sting, 1.15) : Theme.sting)
                                     : (down ? Theme.honeyPale : Theme.paper)
    readonly property color labelColor: kind === "danger" ? Theme.paper : Theme.ink

    implicitHeight: Theme.touch
    implicitWidth: Math.max(Theme.touch, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: 18
    rightPadding: 18
    opacity: enabled ? 1 : 0.45

    contentItem: Text {
        text: control.text
        color: control.labelColor
        font.pixelSize: Theme.body
        font.bold: control.kind !== "secondary"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.fillColor
        border.width: control.kind === "secondary" ? 1.5 : 0
        border.color: Theme.ink

        // Contorno de foco do teclado.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: Theme.radius + 4
            color: "transparent"
            border.width: 2
            border.color: Theme.ink
            visible: control.visualFocus
        }
    }
}
