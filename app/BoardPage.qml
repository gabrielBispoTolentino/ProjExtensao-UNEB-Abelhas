import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Layout comum das ferramentas do menu lateral: aviso no topo e corpo com
// rolagem. Os itens declarados em cada tela entram no corpo.
Item {
    id: root

    default property alias content: body.data

    // A placa em que as ferramentas agem (escolhida no BoardPicker).
    readonly property var board: boardControl.selectedDevice
    readonly property bool hasBoard: !!board.key
    readonly property bool busy: boardControl.busy

    // Não mostra o resultado da ferramenta anterior ao abrir esta.
    Component.onCompleted: boardControl.clearMessage()

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        // O aviso fica no topo para o teclado nunca escondê-lo.
        MessageBanner {
            text: boardControl.message
            failed: boardControl.messageIsError
            busy: root.busy
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: body.implicitHeight + 24
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: body
                width: flick.width
                spacing: 12
            }
        }
    }
}
