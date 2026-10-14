import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Conecta uma ESP32 à internet: o app fala com a placa por Bluetooth,
// pergunta quais redes Wi-Fi ela enxerga e envia a escolhida.
BoardPage {
    id: root

    function signalLabel(rssi) {
        if (rssi >= -60) return "Sinal forte"
        if (rssi >= -75) return "Sinal bom"
        return "Sinal fraco"
    }

    BoardPicker {
        Layout.fillWidth: true
    }

    SectionHeading {
        text: "2. Escolha a rede Wi-Fi"
        dimmed: !root.hasBoard
    }

    ActionButton {
        text: boardControl.scanning ? "Procurando..." : "Procurar redes"
        enabled: root.hasBoard && !root.busy
        onClicked: boardControl.scanNetworks()
    }

    Repeater {
        model: boardControl.networks

        delegate: ItemDelegate {
            id: network
            required property var modelData
            readonly property bool chosen: ssidField.text === modelData.ssid

            Layout.fillWidth: true
            implicitHeight: Theme.touch
            leftPadding: 14
            rightPadding: 14
            enabled: !root.busy
            Accessible.name: modelData.ssid
            onClicked: {
                ssidField.text = modelData.ssid
                passwordField.text = ""
                if (modelData.secure)
                    passwordField.forceActiveFocus()
            }

            background: Rectangle {
                radius: Theme.radius
                color: network.chosen ? Theme.honeyPale : (network.down ? Theme.wing : Theme.paper)
                border.width: network.chosen || network.visualFocus ? 2 : 1
                border.color: network.chosen || network.visualFocus ? Theme.ink : Theme.rule
            }

            contentItem: RowLayout {
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: network.modelData.ssid
                    color: Theme.ink
                    font.pixelSize: Theme.body
                    font.bold: network.chosen
                    elide: Text.ElideRight
                }
                Text {
                    text: root.signalLabel(network.modelData.rssi)
                          + (network.modelData.secure ? "" : ", aberta")
                    color: Theme.inkSoft
                    font.pixelSize: Theme.small
                }
            }
        }
    }

    TextField {
        id: ssidField
        Layout.fillWidth: true
        implicitHeight: Theme.touch
        enabled: root.hasBoard && !root.busy
        placeholderText: "Nome da rede (SSID)"
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        maximumLength: 32
    }

    TextField {
        id: passwordField
        Layout.fillWidth: true
        implicitHeight: Theme.touch
        enabled: root.hasBoard && !root.busy
        placeholderText: "Senha (vazia para redes abertas)"
        echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                          | Qt.ImhSensitiveData
        maximumLength: 63
        onAccepted: if (connectButton.enabled) connectButton.clicked()
    }

    CheckBox {
        id: showPassword
        text: "Mostrar senha"
    }

    ActionButton {
        id: connectButton
        Layout.fillWidth: true
        kind: "primary"
        text: boardControl.connecting ? "Conectando..." : "Conectar a placa a esta rede"
        enabled: root.hasBoard && !root.busy && ssidField.text.trim() !== ""
        onClicked: boardControl.connectToWifi(ssidField.text, passwordField.text)
    }

    HelpText {
        text: "O ESP32 só funciona com redes de 2,4 GHz. A rede só é salva na placa "
              + "se a conexão der certo."
    }
}
