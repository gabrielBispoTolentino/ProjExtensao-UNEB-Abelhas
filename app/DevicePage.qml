import QtQuick
import QtQuick.Layouts

// Histórico de uma placa: estatísticas de temperatura e umidade no período
// escolhido e um gráfico das duas (ou de uma). Atualiza conforme chegam leituras.
Item {
    id: root

    property string deviceId

    property int rangeHours: 24        // 0 = todas as leituras
    property string chartMode: "both"  // "both" | "temp" | "hum"
    property bool collapseGaps: false  // juntar os períodos sem leituras no gráfico
    property bool showOutside: true    // temperatura externa no gráfico
    property var stats: ({})
    property var series: ({ points: [], bucketMs: 60000 })
    property var outside: []           // temperatura externa no período do gráfico

    // Pede ao Main para abrir outra tela (ex.: "weather", o local do apiário).
    signal openPageRequested(string page)

    readonly property var ranges: [
        { hours: 24,  label: "24 h" },
        { hours: 168, label: "7 dias" },
        { hours: 720, label: "30 dias" },
        { hours: 0,   label: "Tudo" }
    ]
    readonly property var chartModes: [
        { mode: "both", label: "Ambos" },
        { mode: "temp", label: "Temperatura" },
        { mode: "hum",  label: "Umidade" }
    ]

    function refresh() {
        stats = sensorMonitor.deviceStats(deviceId, rangeHours)
        series = sensorMonitor.deviceSeries(deviceId, rangeHours, 300)
        refreshOutside()
    }

    function refreshOutside() {
        outside = series.firstMs
                  ? outsideWeather.series(series.firstMs, series.lastMs, series.bucketMs || 60000)
                  : []
    }

    function fmt(value, decimals, unit) {
        if (value === undefined || value === null || isNaN(value))
            return "-"
        // pt-BR: vírgula decimal
        return Number(value).toLocaleString(Qt.locale(), "f", decimals) + (unit ? " " + unit : "")
    }

    function signed(value, decimals, unit) {
        if (value === undefined || value === null || isNaN(value))
            return "-"
        return (value > 0 ? "+" : "") + fmt(value, decimals, unit)
    }

    function when(ms) {
        return ms ? Qt.formatDateTime(new Date(ms), "dd/MM HH:mm") : ""
    }

    // Duração legível: "2 dias e 4 h", "5 h e 20 min", "40 min".
    function duration(ms) {
        const minutes = Math.round(ms / 60000)
        const days = Math.floor(minutes / 1440)
        const hours = Math.floor(minutes % 1440 / 60)
        const mins = minutes % 60
        if (days > 0)
            return days + (days === 1 ? " dia" : " dias") + (hours > 0 ? " e " + hours + " h" : "")
        if (hours > 0)
            return hours + " h" + (mins > 0 ? " e " + mins + " min" : "")
        return mins + " min"
    }

    function correlationText(r) {
        if (r === undefined || r === null)
            return "Pouca variação neste período para comparar temperatura e umidade."
        const a = Math.abs(r)
        if (a < 0.2)
            return "Sem relação clara: temperatura e umidade variam de forma independente."
        const strength = a < 0.4 ? "Correlação fraca"
                       : a < 0.7 ? "Correlação moderada"
                       : "Correlação forte"
        return strength + (r < 0
                           ? " negativa: quando a temperatura sobe, a umidade tende a cair."
                           : " positiva: temperatura e umidade tendem a subir e descer juntas.")
    }

    Component.onCompleted: refresh()
    onRangeHoursChanged: refresh()

    // Leituras novas chegam a cada ~2 s; redesenha no máximo a cada 5 s.
    Timer {
        id: refreshTimer
        interval: 5000
        onTriggered: root.refresh()
    }
    Connections {
        target: sensorMonitor
        function onDeviceReadingAdded(deviceId) {
            if (deviceId === root.deviceId && !refreshTimer.running)
                refreshTimer.start()
        }
    }
    // Horas externas novas (ou outro local): estatísticas também mudam.
    Connections {
        target: outsideWeather
        function onDataChanged() {
            if (!refreshTimer.running)
                refreshTimer.start()
        }
    }

    component StatLabel: Text {
        color: Theme.inkSoft
        font.pixelSize: Theme.small + 1
    }
    component StatValue: Text {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignRight
        color: Theme.ink
        font.pixelSize: Theme.small + 1
        elide: Text.ElideLeft
    }

    // Folha branca do gráfico e das estatísticas.
    component Sheet: Rectangle {
        radius: Theme.radius + 2
        color: Theme.paper
    }

    // Estatísticas de uma medida; a cor a liga à linha do gráfico.
    component StatsCard: Sheet {
        id: card
        property string title
        property color keyColor
        property var values: ({})
        property string unit
        // Linhas a mais no fim da tabela: [{label, value}].
        property var extraRows: []

        implicitHeight: cardColumn.implicitHeight + 32

        ColumnLayout {
            id: cardColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 4

            RowLayout {
                spacing: 8
                Rectangle {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 3
                    radius: 1.5
                    color: card.keyColor
                }
                Text {
                    text: card.title
                    color: Theme.ink
                    font.pixelSize: Theme.body + 1
                    font.bold: true
                }
            }

            // Destaque: o valor mais recente.
            Text {
                text: root.fmt(card.values.current, 1, card.unit)
                color: card.keyColor
                font.pixelSize: Theme.figure
                font.letterSpacing: -0.5
            }
            Text {
                visible: !!card.values.currentAt
                text: "Último registro: " + root.when(card.values.currentAt)
                color: Theme.inkSoft
                font.pixelSize: Theme.small
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                columns: 2
                columnSpacing: 12
                rowSpacing: 7

                StatLabel { text: "Média" }
                StatValue { text: root.fmt(card.values.average, 1, card.unit) }

                StatLabel { text: "Mínima" }
                StatValue {
                    text: root.fmt(card.values.min, 1, card.unit)
                          + (card.values.minAt ? "  (" + root.when(card.values.minAt) + ")" : "")
                }

                StatLabel { text: "Máxima" }
                StatValue {
                    text: root.fmt(card.values.max, 1, card.unit)
                          + (card.values.maxAt ? "  (" + root.when(card.values.maxAt) + ")" : "")
                }

                StatLabel { text: "Amplitude" }
                StatValue {
                    text: card.values.max === undefined ? "-"
                          : root.fmt(card.values.max - card.values.min, 1, card.unit)
                }

                StatLabel { text: "Desvio padrão" }
                StatValue { text: card.values.stdDev === undefined ? "-" : "± " + root.fmt(card.values.stdDev, 2, card.unit) }

                StatLabel { text: "Variação no período" }
                StatValue { text: root.signed(card.values.change, 1, card.unit) }

                // Uma célula por item (rótulo, valor, rótulo...): o GridLayout
                // de 2 colunas os põe lado a lado.
                Repeater {
                    model: card.extraRows.reduce((cells, row) => cells.concat(
                        [{ label: true, text: row.label }, { label: false, text: row.value }]), [])
                    delegate: Text {
                        required property var modelData
                        Layout.fillWidth: !modelData.label
                        horizontalAlignment: modelData.label ? Text.AlignLeft : Text.AlignRight
                        text: modelData.text
                        color: modelData.label ? Theme.inkSoft : Theme.ink
                        font.pixelSize: Theme.small + 1
                        elide: modelData.label ? Text.ElideNone : Text.ElideLeft
                    }
                }
            }
        }
    }

    // Escolha entre poucas opções (período, modo do gráfico).
    component Choices: Flow {
        id: choices
        property var options: []
        property string valueKey
        property var current
        signal picked(var value)
        spacing: 6

        Repeater {
            model: choices.options

            ChoiceChip {
                required property var modelData
                text: modelData.label
                checked: modelData[choices.valueKey] === choices.current
                onClicked: choices.picked(modelData[choices.valueKey])
            }
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight + 24
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: content
            width: flick.width
            spacing: 12

            // Filtros acima do gráfico.
            Flow {
                Layout.fillWidth: true
                spacing: 16

                Choices {
                    options: root.ranges
                    valueKey: "hours"
                    current: root.rangeHours
                    onPicked: (value) => root.rangeHours = value
                }
                Choices {
                    options: root.chartModes
                    valueKey: "mode"
                    current: root.chartMode
                    onPicked: (value) => root.chartMode = value
                }
                ChoiceChip {
                    text: "Ocultar períodos sem dados"
                    checked: root.collapseGaps
                    onToggled: root.collapseGaps = checked
                }
                ChoiceChip {
                    text: "Temperatura externa"
                    checked: root.showOutside
                    onToggled: root.showOutside = checked
                }
            }

            Text {
                Layout.fillWidth: true
                text: (root.stats.count || 0) === 0
                      ? "Nenhuma leitura desta placa neste período."
                      : root.stats.count.toLocaleString(Qt.locale(), "f", 0) + " registros, de "
                        + Qt.formatDateTime(new Date(root.stats.firstAt), "dd/MM/yyyy HH:mm") + " a "
                        + Qt.formatDateTime(new Date(root.stats.lastAt), "dd/MM/yyyy HH:mm")
                color: Theme.inkSoft
                font.pixelSize: Theme.small
                wrapMode: Text.WordWrap
            }

            Sheet {
                Layout.fillWidth: true
                implicitHeight: root.chartMode === "both" ? 380 : 260

                SensorChart {
                    id: chart
                    anchors.fill: parent
                    anchors.margins: 12
                    points: root.series.points || []
                    bucketMs: root.series.bucketMs || 60000
                    mode: root.chartMode
                    collapseGaps: root.collapseGaps
                    outsidePoints: root.outside
                    showOutside: root.showOutside
                    // Da primeira à última leitura de fato no período, para a
                    // linha ocupar o gráfico (no mínimo 1 min de largura).
                    startMs: root.series.firstMs || 0
                    endMs: Math.max(root.series.lastMs || 0, startMs + 60000)
                }
            }

            // Temperatura externa ligada, mas sem cidade escolhida: leva à tela do local.
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.showOutside && !outsideWeather.hasLocation && root.chartMode !== "hum"
                spacing: 8

                HelpText {
                    text: "Para comparar com a temperatura lá fora, escolha a cidade do apiário."
                }
                ActionButton {
                    text: "Definir local do apiário"
                    onClicked: root.openPageRequested("weather")
                }
            }

            // Com a opção ligada, diz o que foi ocultado (ou que não havia o
            // que ocultar, para o toque na opção não parecer sem efeito).
            HelpText {
                visible: root.collapseGaps && chart.points.length > 0
                text: chart.gapCount === 0
                      ? "Nenhum período sem leituras neste intervalo: a placa registrou sem interrupções."
                      : (chart.gapCount === 1 ? "1 período sem leituras foi ocultado"
                                              : chart.gapCount + " períodos sem leituras foram ocultados")
                        + " (" + root.duration(chart.hiddenMs) + " no total). As faixas claras"
                        + " com zigue-zague marcam onde a placa ficou desligada ou fora de alcance."
            }

            GridLayout {
                Layout.fillWidth: true
                columns: root.width >= 560 ? 2 : 1
                columnSpacing: 12
                rowSpacing: 12

                StatsCard {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    title: "Temperatura"
                    keyColor: Theme.propolis
                    unit: "°C"
                    values: root.stats.temperature || {}
                    extraRows: values.outsideAverage === undefined ? [] : [
                        { label: "Externa (média)", value: root.fmt(values.outsideAverage, 1, "°C") },
                        { label: "Externa mín. / máx.",
                          value: root.fmt(values.outsideMin, 1, "") + " / " + root.fmt(values.outsideMax, 1, "°C") },
                        { label: "Colmeia − externa (média)", value: root.signed(values.outsideDifference, 1, "°C") }
                    ]
                }
                StatsCard {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    title: "Umidade"
                    keyColor: Theme.sky
                    unit: "%"
                    values: root.stats.humidity || {}
                }
            }

            Sheet {
                Layout.fillWidth: true
                implicitHeight: correlationColumn.implicitHeight + 32
                visible: (root.stats.count || 0) > 0

                ColumnLayout {
                    id: correlationColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 16
                    spacing: 6

                    Text {
                        text: "Temperatura x umidade"
                        color: Theme.ink
                        font.pixelSize: Theme.body + 1
                        font.bold: true
                    }
                    Text {
                        visible: root.stats.correlation !== undefined
                        text: "Correlação (r de Pearson): " + root.fmt(root.stats.correlation, 2, "")
                        color: Theme.ink
                        font.pixelSize: Theme.body
                    }
                    HelpText {
                        text: root.correlationText(root.stats.correlation)
                    }
                }
            }
        }
    }
}
