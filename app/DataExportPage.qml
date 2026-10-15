import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Salva leituras deste aparelho num arquivo CSV (planilha): de uma placa ou
// de todas, de um período. Abre direto no Excel, LibreOffice e Google
// Planilhas em português (";" entre colunas, vírgula decimal).
Item {
    id: root

    property string deviceId: ""       // "" = todas as placas
    property int rangeHours: 0         // 0 = tudo
    property int count: 0
    property var result: ({})          // {ok, message} da última exportação

    readonly property var periods: [
        { hours: 0,    label: "Todos os registros" },
        { hours: 24,   label: "Últimas 24 horas" },
        { hours: 168,  label: "Últimos 7 dias" },
        { hours: 720,  label: "Últimos 30 dias" },
        { hours: 2160, label: "Últimos 90 dias" }
    ]

    // "Todas as colmeias" + as placas conhecidas neste aparelho.
    property var boards: []

    // Mesma regra de DataCleanupPage: só troca a lista quando as placas mudam.
    function updateBoards() {
        const list = [{ deviceId: "", name: "Todas as colmeias" }].concat(
            sensorMonitor.devices.map(d => ({
                deviceId: d.deviceId,
                name: d.name === d.deviceId ? d.name : d.name + " (" + d.deviceId + ")"
            })))
        if (JSON.stringify(list) === JSON.stringify(boards))
            return
        boards = list
        if (!list.some(b => b.deviceId === deviceId))
            deviceId = ""
    }

    function refreshCount() {
        count = sensorMonitor.exportCount(deviceId, rangeHours)
    }

    // Ex.: "abelhas-colmeia-67-2026-10-15.csv".
    function suggestedName() {
        const device = sensorMonitor.devices.find(d => d.deviceId === deviceId)
        const board = deviceId === "" ? "todas" : (device ? device.name : deviceId)
        const slug = board.normalize("NFD").replace(/[̀-ͯ]/g, "")
                          .toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "")
        return "abelhas-" + (slug || "placa") + "-" + Qt.formatDate(new Date(), "yyyy-MM-dd") + ".csv"
    }

    Component.onCompleted: {
        updateBoards()
        refreshCount()
    }
    onDeviceIdChanged: refreshCount()
    onRangeHoursChanged: refreshCount()

    Connections {
        target: sensorMonitor
        function onDevicesChanged() { root.updateBoards() }
        function onDeviceReadingAdded(deviceId) {
            if (!countTimer.running)
                countTimer.start()
        }
        function onDataCleaned() { root.refreshCount() }
    }
    Timer {
        id: countTimer
        interval: 5000
        onTriggered: root.refreshCount()
    }

    FileDialog {
        id: saveDialog
        title: "Salvar planilha"
        fileMode: FileDialog.SaveFile
        nameFilters: ["Planilha CSV (*.csv)"]
        defaultSuffix: "csv"
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: root.result = sensorMonitor.exportCsv(selectedFile, root.deviceId, root.rangeHours)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        MessageBanner {
            text: root.result.message || ""
            failed: root.result.ok === false
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

                // ── O quê ──────────────────────────────────────────────────
                SectionHeading {
                    Layout.topMargin: 0
                    text: "1. Escolha os dados"
                }

                AppComboBox {
                    id: boardBox
                    Layout.fillWidth: true
                    model: root.boards
                    textRole: "name"
                    valueRole: "deviceId"
                    Accessible.name: "Colmeia"
                    onActivated: root.deviceId = currentValue

                    function sync() {
                        currentIndex = Math.max(0, indexOfValue(root.deviceId))
                    }
                    onModelChanged: sync()
                    Connections {
                        target: root
                        function onDeviceIdChanged() { boardBox.sync() }
                    }
                }

                AppComboBox {
                    Layout.fillWidth: true
                    model: root.periods
                    textRole: "label"
                    valueRole: "hours"
                    Accessible.name: "Período"
                    onActivated: root.rangeHours = currentValue
                }

                HelpText {
                    text: root.count === 0
                          ? "Nenhum registro desta seleção está guardado neste aparelho."
                          : root.count === 1
                            ? "1 registro será exportado."
                            : Number(root.count).toLocaleString(Qt.locale(), "f", 0) + " registros serão exportados."
                }

                // ── Arquivo ────────────────────────────────────────────────
                SectionHeading {
                    text: "2. Salve o arquivo"
                }

                HelpText {
                    text: "Uma linha por registro, com colmeia, data e hora, temperatura, umidade e a "
                          + "temperatura externa daquela hora"
                          + (outsideWeather.hasLocation
                             ? " em " + outsideWeather.locationName + "."
                             : " (vazia: defina o local do apiário no menu para preenchê-la).")
                }

                ActionButton {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    kind: "primary"
                    text: "Exportar CSV"
                    enabled: root.count > 0
                    onClicked: {
                        saveDialog.selectedFile = saveDialog.currentFolder + "/" + root.suggestedName()
                        saveDialog.open()
                    }
                }

                HelpText {
                    text: "Exporta o que está guardado neste aparelho, que já inclui o que veio da nuvem."
                }
            }
        }
    }
}
