import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Renomeia uma placa. O nome fica salvo na própria placa, vai junto com cada
// leitura (todo app vê) e é o nome mostrado na busca de Bluetooth.
BoardPage {
    id: root

    readonly property string currentName: board.name || ""

    // Ao escolher outra placa, começa pelo nome atual dela.
    onCurrentNameChanged: nameField.text = currentName

    BoardPicker {
        Layout.fillWidth: true
    }

    SectionHeading {
        text: "2. Escreva o novo nome"
        dimmed: !root.hasBoard
    }

    TextField {
        id: nameField
        Layout.fillWidth: true
        implicitHeight: Theme.touch
        enabled: root.hasBoard && !root.busy
        placeholderText: "ex.: Colmeia 3 - jardim"
        maximumLength: 24
        onAccepted: if (saveButton.enabled) saveButton.clicked()
    }

    ActionButton {
        id: saveButton
        Layout.fillWidth: true
        kind: "primary"
        text: boardControl.renaming ? "Salvando..." : "Salvar nome"
        enabled: root.hasBoard && !root.busy
                 && nameField.text.trim() !== "" && nameField.text.trim() !== root.currentName
        onClicked: boardControl.renameBoard(nameField.text)
    }

    HelpText {
        text: "Até 24 caracteres. As leituras passam a usar o nome novo na hora; a busca "
              + "de Bluetooth mostra o nome novo depois que a placa reinicia ou o celular desconecta."
    }
}
