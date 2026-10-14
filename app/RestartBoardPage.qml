import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Reinicia uma placa à distância (ex.: após uma falha do sensor), com confirmação.
BoardPage {
    id: root

    BoardPicker {
        Layout.fillWidth: true
    }

    SectionHeading {
        text: "2. Reinicie"
        dimmed: !root.hasBoard
    }

    HelpText {
        text: "A placa reinicia em alguns segundos. O nome, a rede Wi-Fi e as leituras "
              + "guardadas no cartão SD são mantidos. A conexão Bluetooth cai e volta "
              + "sozinha."
    }

    ActionButton {
        Layout.fillWidth: true
        kind: "primary"
        text: boardControl.restarting ? "Reiniciando..." : "Reiniciar placa"
        enabled: root.hasBoard && !root.busy
        onClicked: confirmDialog.open()
    }

    ConfirmDialog {
        id: confirmDialog
        title: "Reiniciar a placa?"
        message: "\"" + (root.board.name || "") + "\" vai reiniciar agora."
        acceptText: "Reiniciar"
        onAccepted: boardControl.restartBoard()
    }
}
