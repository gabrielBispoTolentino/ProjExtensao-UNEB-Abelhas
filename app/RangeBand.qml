import QtQuick
import QtQuick.Layouts

// Onde a temperatura atual está dentro das últimas 24 h da própria colmeia:
//   24 h  28,9 ━━━━━━━●━━ 33,1 °C
// O sensor mede o ar que sobe do ninho para a melgueira, então a comparação
// útil é com o próprio dia da colônia, e não com uma temperatura fixa de cria.
RowLayout {
    id: root

    property real current
    property real low          // menor valor dos registros das últimas 24 h
    property real high
    property color color: Theme.propolis

    // Um valor ao vivo pode cair fora da faixa dos registros: a faixa se estende.
    readonly property real from: Math.min(low, current)
    readonly property real to: Math.max(high, current)
    readonly property real position: to - from < 0.05 ? 0.5 : (current - from) / (to - from)

    function number(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 1)
    }

    spacing: 8
    Accessible.name: "Últimas 24 horas: mínima " + number(from) + ", máxima " + number(to)
                     + " graus, agora " + number(current)

    Text {
        text: "24 h"
        color: Theme.inkSoft
        font.pixelSize: Theme.small
        font.bold: true
    }
    Text {
        text: root.number(root.from)
        color: Theme.inkSoft
        font.pixelSize: Theme.small
    }

    Item {
        Layout.fillWidth: true
        Layout.minimumWidth: 60
        implicitHeight: 18

        Rectangle {
            id: track
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Qt.tint(Theme.paper, Qt.rgba(root.color.r, root.color.g, root.color.b, 0.35))
        }

        // Agora.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            x: root.position * (parent.width - width)
            width: 16
            height: 16
            radius: 8
            color: root.color
            border.width: 3
            border.color: Theme.paper
        }
    }

    Text {
        text: root.number(root.to) + " °C"
        color: Theme.inkSoft
        font.pixelSize: Theme.small
    }
}
