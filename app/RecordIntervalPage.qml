import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Define de quanto em quanto tempo a placa grava um registro (a média das
// leituras a cada 2 s), que é o que vai para o Supabase. Intervalos maiores
// ocupam menos espaço no banco.
BoardPage {
    id: root

    readonly property var options: [1, 5, 10, 15, 30, 60]
    readonly property int currentInterval: board.interval || 0
    property int selected: 1

    // Ao escolher outra placa, começa pela configuração atual dela.
    onCurrentIntervalChanged: selected = currentInterval > 0 ? currentInterval : 1

    // Espaço aproximado no Supabase: ~190 bytes por linha com índices,
    // plano gratuito de 250 MB, uma placa.
    readonly property real bytesPerRow: 190
    readonly property real storageBytes: 250 * 1000 * 1000

    function minutesLabel(minutes) {
        return minutes === 1 ? "1 minuto"
             : minutes === 60 ? "1 hora"
             : minutes + " minutos"
    }

    function lastsLabel(minutes) {
        const days = storageBytes / (1440 / minutes * bytesPerRow)
        if (days < 60)
            return Math.round(days) + " dias"
        if (days < 730)
            return Math.round(days / 30) + " meses"
        const years = days / 365
        return (years < 10 ? years.toLocaleString(Qt.locale(), "f", 1) : Math.round(years)) + " anos"
    }

    BoardPicker {
        Layout.fillWidth: true
    }

    SectionHeading {
        text: "2. Escolha o intervalo"
        dimmed: !root.hasBoard
    }

    HelpText {
        visible: root.hasBoard
        text: root.currentInterval > 0
              ? "Agora: um registro a cada " + root.minutesLabel(root.currentInterval) + "."
              : "Esta placa não informou o intervalo. Atualize o firmware dela."
        color: Theme.ink
        font.pixelSize: Theme.body
    }

    ButtonGroup {
        id: intervalGroup
    }

    Repeater {
        model: root.options

        delegate: RadioButton {
            id: option
            required property int modelData

            Layout.fillWidth: true
            enabled: root.hasBoard && !root.busy
            checked: root.selected === modelData
            ButtonGroup.group: intervalGroup
            text: root.minutesLabel(modelData)   // para acessibilidade; quem mostra é o contentItem
            onClicked: root.selected = modelData

            contentItem: Item {
                implicitHeight: Math.max(optionRow.implicitHeight, 32)

                RowLayout {
                    id: optionRow
                    anchors.fill: parent
                    // Deixa espaço para o círculo de seleção desenhado pelo estilo.
                    anchors.leftMargin: option.indicator ? option.indicator.width + option.spacing : 0
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: option.text + (option.modelData === 1 ? " (padrão)" : "")
                        color: Theme.ink
                        font.pixelSize: Theme.body
                        font.bold: option.checked
                    }
                    Text {
                        text: "250 MB duram ~" + root.lastsLabel(option.modelData)
                        color: Theme.inkSoft
                        font.pixelSize: Theme.small
                    }
                }
            }
        }
    }

    ActionButton {
        Layout.fillWidth: true
        kind: "primary"
        text: boardControl.savingInterval ? "Salvando..." : "Salvar intervalo"
        // currentInterval primeiro: avaliá-lo pode redefinir `selected` (ver
        // onCurrentIntervalChanged), o que de outro jeito criaria um laço.
        enabled: root.hasBoard && !root.busy && root.currentInterval !== root.selected
        onClicked: boardControl.setRecordInterval(root.selected)
    }

    HelpText {
        text: "A placa continua medindo a cada 2 segundos para a tela ao vivo. O registro é a "
              + "média desse intervalo e é o que vai para a nuvem: intervalos maiores ocupam menos "
              + "espaço no Supabase. A duração é uma estimativa para uma placa no plano gratuito."
    }
}
