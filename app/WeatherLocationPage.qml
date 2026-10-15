import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Cidade do apiário, de onde vem a temperatura externa (Open-Meteo) que o
// gráfico, as estatísticas e a exportação comparam com a da colmeia.
Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        MessageBanner {
            text: outsideWeather.statusText
            failed: outsideWeather.statusFailed
            busy: outsideWeather.updating
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

                // ── Local atual ────────────────────────────────────────────
                SectionHeading {
                    Layout.topMargin: 0
                    text: "Local atual"
                }

                Text {
                    Layout.fillWidth: true
                    text: outsideWeather.hasLocation ? outsideWeather.locationName : "Nenhum local definido."
                    color: outsideWeather.hasLocation ? Theme.ink : Theme.inkSoft
                    font.pixelSize: Theme.body + 1
                    font.bold: outsideWeather.hasLocation
                    wrapMode: Text.WordWrap
                }
                HelpText {
                    visible: outsideWeather.hasLocation
                    Layout.topMargin: -8
                    text: "Latitude " + Number(outsideWeather.latitude).toLocaleString(Qt.locale(), "f", 4)
                          + ", longitude " + Number(outsideWeather.longitude).toLocaleString(Qt.locale(), "f", 4)
                }

                Flow {
                    Layout.fillWidth: true
                    visible: outsideWeather.hasLocation
                    spacing: 8

                    ActionButton {
                        text: "Atualizar agora"
                        enabled: !outsideWeather.updating
                        onClicked: outsideWeather.refresh()
                    }
                    ActionButton {
                        text: "Remover local"
                        onClicked: outsideWeather.clearLocation()
                    }
                }

                // ── Busca ──────────────────────────────────────────────────
                SectionHeading {
                    text: outsideWeather.hasLocation ? "Trocar de cidade" : "Procure a cidade do apiário"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    TextField {
                        id: searchField
                        Layout.fillWidth: true
                        implicitHeight: Theme.touch
                        placeholderText: "Nome da cidade, ex.: Cruz das Almas"
                        inputMethodHints: Qt.ImhNoPredictiveText
                        onAccepted: outsideWeather.search(text)
                    }
                    ActionButton {
                        kind: "primary"
                        text: "Procurar"
                        enabled: searchField.text.trim().length >= 2 && !outsideWeather.searching
                        onClicked: outsideWeather.search(searchField.text)
                    }
                }

                HelpText {
                    visible: outsideWeather.searching || outsideWeather.searchMessage !== ""
                    text: outsideWeather.searching ? "Procurando..." : outsideWeather.searchMessage
                }

                Repeater {
                    model: outsideWeather.searchResults

                    delegate: ItemDelegate {
                        id: entry
                        required property var modelData

                        Layout.fillWidth: true
                        padding: 12
                        leftPadding: 16
                        Accessible.name: modelData.label
                        onClicked: {
                            outsideWeather.setLocation(modelData.label, modelData.latitude, modelData.longitude)
                            searchField.text = ""
                        }

                        background: Rectangle {
                            radius: Theme.radius
                            color: entry.down ? Theme.honeyPale : Theme.paper
                            border.width: entry.visualFocus ? 2 : 1
                            border.color: entry.visualFocus ? Theme.ink : Theme.rule
                        }

                        contentItem: Text {
                            text: entry.modelData.label
                            color: Theme.ink
                            font.pixelSize: Theme.body
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                HelpText {
                    Layout.topMargin: 8
                    text: "A temperatura externa vem do Open-Meteo (open-meteo.com): uma estimativa hora a "
                          + "hora, feita por modelos meteorológicos para a cidade, não uma medição no "
                          + "apiário. Escolha a cidade mais próxima das colmeias. As temperaturas ficam "
                          + "guardadas neste aparelho, e cada celular ou computador escolhe o próprio local."
                }
            }
        }
    }
}
