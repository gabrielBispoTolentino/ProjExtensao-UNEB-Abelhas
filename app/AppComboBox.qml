import QtQuick
import QtQuick.Controls

// ComboBox do estilo Basic com contorno visível (o original não tem).
ComboBox {
    id: control

    implicitHeight: Theme.touch
    font.pixelSize: Theme.body

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Theme.honeyPale : Theme.paper
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Theme.ink : Theme.inkFaint
        opacity: control.enabled ? 1 : 0.45
    }
}
