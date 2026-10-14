import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Passo "qual placa?" de todas as ferramentas: lista as placas conectadas
// por Bluetooth com o estado de cada uma; tocar numa a seleciona.
ColumnLayout {
    id: root

    spacing: 10

    function boardSummary(device) {
        if (!device.statusKnown)
            return "Lendo o estado da placa..."
        if (device.wifi)
            return "Online em \"" + device.ssid + "\" (" + device.ip + ")"
                   + (device.cloud ? ", enviando para a nuvem" : "")
                   + root.intervalText(device)
        if (device.savedSsid)
            return "Offline: sem acesso à rede salva \"" + device.savedSsid + "\""
                   + root.intervalText(device)
        return "Offline: nenhuma rede Wi-Fi configurada" + root.intervalText(device)
    }

    function intervalText(device) {
        if (!device.interval)
            return ""
        return device.interval === 1 ? "\nRegistro a cada 1 minuto"
                                     : "\nRegistro a cada " + device.interval + " minutos"
    }

    SectionHeading {
        Layout.topMargin: 0
        text: "1. Escolha a placa"
    }

    HelpText {
        visible: boardControl.devices.length === 0
        text: "Nenhuma placa conectada ainda. Ligue a placa, deixe-a perto do celular "
              + "e verifique se o Bluetooth está ligado.\n" + sensorMonitor.bleStatusText
    }

    ActionButton {
        visible: boardControl.devices.length === 0
        text: "Procurar de novo"
        onClicked: boardControl.searchDevices()
    }

    Repeater {
        model: boardControl.devices

        delegate: ItemDelegate {
            id: entry
            required property var modelData

            Layout.fillWidth: true
            enabled: !boardControl.busy
            padding: 12
            leftPadding: 16
            Accessible.name: modelData.name
            onClicked: boardControl.selectDevice(modelData.key)

            background: Rectangle {
                radius: Theme.radius
                color: entry.modelData.selected ? Theme.honeyPale : (entry.down ? Theme.wing : Theme.paper)
                border.width: entry.modelData.selected || entry.visualFocus ? 2 : 1
                border.color: entry.modelData.selected || entry.visualFocus ? Theme.ink : Theme.rule
            }

            contentItem: Column {
                spacing: 3

                Text {
                    width: parent.width
                    text: entry.modelData.name
                          + (entry.modelData.deviceId ? "  (" + entry.modelData.deviceId + ")" : "")
                    color: Theme.ink
                    font.pixelSize: Theme.body + 1
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.boardSummary(entry.modelData)
                    color: Theme.inkSoft
                    font.pixelSize: Theme.small
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
