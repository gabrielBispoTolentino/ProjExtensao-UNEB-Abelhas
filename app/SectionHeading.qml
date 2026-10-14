import QtQuick
import QtQuick.Layouts

// Título de um passo ou grupo numa tela de ferramenta, ex.: "1. Escolha a placa".
// dimmed: o passo ainda não pode ser usado (ex.: nenhuma placa escolhida).
Text {
    property bool dimmed: false

    Layout.fillWidth: true
    Layout.topMargin: 8
    color: dimmed ? Theme.inkFaint : Theme.ink
    font.pixelSize: Theme.lead
    font.bold: true
    wrapMode: Text.WordWrap
}
