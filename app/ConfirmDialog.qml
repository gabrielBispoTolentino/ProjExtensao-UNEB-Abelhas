import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// "Tem certeza?" antes de uma ação que não pode ser desfeita. O botão de
// confirmar leva o nome da ação ("Reiniciar", "Apagar"); danger o deixa vermelho.
Dialog {
    id: root

    property string message
    property string acceptText: "Confirmar"
    property bool danger: false

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(400, parent ? parent.width - 32 : 400)
    modal: true
    padding: 20
    // Título e mensagem ficam no conteúdo (um Text com quebra de linha no
    // header cria um laço na altura do diálogo).
    header: null
    contentWidth: availableWidth
    contentHeight: body.implicitHeight

    background: Rectangle {
        radius: Theme.radius + 4
        color: Theme.paper
    }

    Overlay.modal: Rectangle {
        color: Qt.rgba(0.17, 0.13, 0.09, 0.45)   // tinta, translúcida
    }

    ColumnLayout {
        id: body
        width: root.availableWidth
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: root.title
            color: Theme.ink
            font.pixelSize: Theme.lead + 2
            font.bold: true
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: root.message
            color: Theme.ink
            font.pixelSize: Theme.body
            lineHeight: 1.2
            wrapMode: Text.WordWrap
        }
    }

    // Botões próprios, sempre "Cancelar" e depois a ação (os padrão do Qt
    // mostram textos em inglês e mudam de ordem conforme a plataforma).
    footer: Item {
        implicitHeight: buttons.implicitHeight + 24

        RowLayout {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.top: parent.top
            anchors.topMargin: 4
            spacing: 10

            ActionButton {
                text: "Cancelar"
                onClicked: root.reject()
            }
            ActionButton {
                text: root.acceptText
                kind: root.danger ? "danger" : "primary"
                onClicked: root.accept()
            }
        }
    }
}
