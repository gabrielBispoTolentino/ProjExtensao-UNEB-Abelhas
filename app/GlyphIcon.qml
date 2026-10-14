import QtQuick
import QtQuick.Shapes

// Ícone de linha pequeno (grade de 24x24), desenhado em vetor para ficar
// nítido em qualquer tamanho e aceitar cor: "bluetooth", "wifi", "back" ou "chevron".
Item {
    id: root

    property string glyph: "wifi"
    property color color: Theme.ink
    property real size: 16

    implicitWidth: size
    implicitHeight: size

    readonly property var paths: ({
        // A runa do Bluetooth.
        "bluetooth": "M7,7 L17,17 L12,22 L12,2 L17,7 L7,17",
        // Setas de "voltar" (barra do topo) e "abrir" (linhas das colmeias).
        "back": "M15,5 L8,12 L15,19",
        "chevron": "M9,5 L16,12 L9,19",
        // Três arcos e um ponto.
        "wifi": "M2.5,9.5 A14,14 0 0 1 21.5,9.5 "
              + "M5.5,13 A9.5,9.5 0 0 1 18.5,13 "
              + "M8.7,16.5 A4.8,4.8 0 0 1 15.3,16.5 "
              + "M11.1,19.8 A0.9,0.9 0 1 0 12.9,19.8 A0.9,0.9 0 1 0 11.1,19.8"
    })

    Shape {
        width: 24
        height: 24
        scale: root.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: root.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathSvg { path: root.paths[root.glyph] || "" }
        }
    }
}
