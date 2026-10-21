import QtQuick
import QtQuick.Controls.Basic

// Uma opção de um conjunto pequeno (período, modo do gráfico): mel quando escolhida.
Button {
    id: control

    checkable: true
    implicitHeight: 40
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    leftPadding: 14
    rightPadding: 14

    contentItem: Text {
        text: control.text
        color: Theme.ink
        font.pixelSize: Theme.small + 1
        font.bold: control.checked
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: height / 2
        color: control.checked ? Theme.honey : (control.down ? Theme.honeyPale : "transparent")
        border.width: control.checked ? 0 : 1
        border.color: Theme.inkFaint

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: Theme.ink
            visible: control.visualFocus
        }
    }
}
