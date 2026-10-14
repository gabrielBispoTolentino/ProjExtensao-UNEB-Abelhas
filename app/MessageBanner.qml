import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Resultado / andamento de uma ação, no topo da tela para o teclado nunca
// escondê-lo. Fica oculto enquanto o texto está vazio.
Rectangle {
    id: root

    property string text
    property bool failed: false
    property bool busy: false

    Layout.fillWidth: true
    visible: text !== ""
    implicitHeight: row.implicitHeight + 20
    radius: Theme.radius
    color: failed ? Theme.stingPale : Theme.honeyPale

    // Barra à esquerda: vermelha para erros, cor de mel nos outros casos.
    Rectangle {
        width: 4
        height: parent.height
        radius: 2
        color: root.failed ? Theme.sting : Theme.honey
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 12
        anchors.topMargin: 10
        anchors.bottomMargin: 10
        spacing: 10

        BusyIndicator {
            visible: root.busy
            running: root.busy
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24
        }

        Text {
            Layout.fillWidth: true
            text: root.text
            color: root.failed ? Theme.sting : Theme.ink
            font.pixelSize: Theme.body
            wrapMode: Text.WordWrap
        }
    }
}
