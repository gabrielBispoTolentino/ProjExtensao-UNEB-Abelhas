import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Apaga leituras: de uma placa ou de todas, só as antigas ou tudo, neste
// aparelho (SQLite local) e/ou no Supabase (exige a senha de administrador).
Item {
    id: root

    property bool local: true
    property bool cloud: false
    property string deviceId: ""       // "" = todas as placas
    property int olderThanDays: 0      // 0 = tudo
    property var localCount: ({ count: 0, pending: 0 })

    readonly property var periods: [
        { days: 0,   label: "Todos os registros" },
        { days: 7,   label: "Mais antigos que 7 dias" },
        { days: 30,  label: "Mais antigos que 30 dias" },
        { days: 90,  label: "Mais antigos que 90 dias" },
        { days: 365, label: "Mais antigos que 1 ano" }
    ]

    // "Todas as colmeias" + as placas conhecidas neste aparelho (ver updateBoards).
    property var boards: []

    function records(count) {
        return count === 1 ? "1 registro"
                           : Number(count).toLocaleString(Qt.locale(), "f", 0) + " registros"
    }

    // Aviso dos registros recebidos por Bluetooth que nunca chegaram ao Supabase.
    function pendingText(count) {
        return count === 1 ? "1 registro ainda não foi enviado à nuvem e será perdido."
                           : records(count) + " ainda não foram enviados à nuvem e serão perdidos."
    }

    // sensorMonitor.devices muda a cada valor ao vivo (2 s): a lista só é
    // trocada quando as placas mudam, para não fechar se estiver aberta.
    function updateBoards() {
        const list = [{ deviceId: "", name: "Todas as colmeias" }].concat(
            sensorMonitor.devices.map(d => ({
                deviceId: d.deviceId,
                name: d.name === d.deviceId ? d.name : d.name + " (" + d.deviceId + ")"
            })))
        if (JSON.stringify(list) === JSON.stringify(boards))
            return
        boards = list
        // Uma placa com todos os dados apagados sai da lista: volta para "todas".
        if (!list.some(b => b.deviceId === deviceId))
            deviceId = ""
    }

    function refreshCount() {
        localCount = sensorMonitor.localDataCount(deviceId, olderThanDays)
    }

    // Ex.: "os registros mais antigos que 30 dias de "Colmeia 1"".
    function selectionText() {
        const what = olderThanDays === 0
                     ? "todos os registros"
                     : "os registros " + periods.find(p => p.days === olderThanDays).label.toLowerCase()
        const board = boards.find(b => b.deviceId === deviceId)
        const of = deviceId === "" ? " de todas as colmeias" : " de \"" + (board ? board.name : deviceId) + "\""
        const where = local && cloud ? " neste aparelho e na nuvem"
                    : cloud ? " na nuvem" : " neste aparelho"
        return what + of + where
    }

    Component.onCompleted: {
        sensorMonitor.clearCleanupMessage()
        updateBoards()
        refreshCount()
    }
    onDeviceIdChanged: refreshCount()
    onOlderThanDaysChanged: refreshCount()

    Connections {
        target: sensorMonitor
        function onDevicesChanged() {
            root.updateBoards()
        }
        function onDataCleaned() {
            root.refreshCount()
            if (!sensorMonitor.cleanupFailed)
                passwordField.text = ""
        }
        // Registros novos chegam a cada minuto ou mais; mantém a contagem em dia.
        function onDeviceReadingAdded(deviceId) {
            if (!countTimer.running)
                countTimer.start()
        }
    }
    Timer {
        id: countTimer
        interval: 5000
        onTriggered: root.refreshCount()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        // Resultado / andamento no topo, como nas ferramentas da placa.
        MessageBanner {
            text: sensorMonitor.cleanupMessage
            failed: sensorMonitor.cleanupFailed
            busy: sensorMonitor.cleaning
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
                    enabled: !sensorMonitor.cleaning
                    Accessible.name: "Colmeia"
                    onActivated: root.deviceId = currentValue

                    // Continua mostrando a placa escolhida quando a lista muda.
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
                    valueRole: "days"
                    enabled: !sensorMonitor.cleaning
                    Accessible.name: "Período"
                    onActivated: root.olderThanDays = currentValue
                }

                // ── Onde ───────────────────────────────────────────────────
                SectionHeading {
                    text: "2. Escolha onde apagar"
                }

                CheckBox {
                    Layout.fillWidth: true
                    text: "Neste aparelho"
                    checked: root.local
                    enabled: !sensorMonitor.cleaning
                    onToggled: root.local = checked
                }
                HelpText {
                    Layout.leftMargin: 34
                    Layout.topMargin: -8
                    text: root.localCount.count === 0
                          ? "Nenhum registro desta seleção está guardado aqui."
                          : (root.localCount.count === 1
                             ? "1 registro desta seleção está guardado aqui."
                             : root.records(root.localCount.count) + " desta seleção estão guardados aqui.")
                            + (root.localCount.pending > 0 ? " " + root.pendingText(root.localCount.pending) : "")
                    color: root.local && root.localCount.pending > 0 ? Theme.sting : Theme.inkSoft
                }

                CheckBox {
                    Layout.fillWidth: true
                    text: "Na nuvem (Supabase)"
                    checked: root.cloud
                    enabled: sensorMonitor.cloudConfigured && !sensorMonitor.cleaning
                    onToggled: root.cloud = checked
                }
                HelpText {
                    Layout.leftMargin: 34
                    Layout.topMargin: -8
                    text: sensorMonitor.cloudConfigured
                          ? "Some para todos os celulares e computadores que ainda não baixaram esses "
                            + "registros. Quem já baixou mantém a cópia até limpar o próprio aparelho."
                          : "Nuvem não configurada."
                }

                // ── Senha (só para a nuvem) ────────────────────────────────
                SectionHeading {
                    visible: root.cloud
                    text: "3. Digite a senha de administrador"
                }
                TextField {
                    id: passwordField
                    Layout.fillWidth: true
                    implicitHeight: Theme.touch
                    visible: root.cloud
                    enabled: !sensorMonitor.cleaning
                    echoMode: TextInput.Password
                    placeholderText: "Senha definida no Supabase"
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                }

                ActionButton {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    kind: "danger"
                    text: sensorMonitor.cleaning ? "Apagando..." : "Apagar dados"
                    enabled: (root.local || root.cloud)
                             && (!root.cloud || passwordField.text !== "")
                             && !sensorMonitor.cleaning
                    onClicked: confirmDialog.open()
                }

                HelpText {
                    text: "Apagar não pode ser desfeito. As placas não reenviam o que já mandaram para "
                          + "a nuvem, e este aparelho não baixa de novo o que foi apagado daqui."
                }
            }
        }
    }

    ConfirmDialog {
        id: confirmDialog
        title: "Apagar dados?"
        danger: true
        acceptText: "Apagar"
        message: "Serão apagados " + root.selectionText() + ". Isso não pode ser desfeito."
                 + (root.local && root.localCount.pending > 0
                    ? "\n\nAtenção: " + root.pendingText(root.localCount.pending)
                    : "")
        onAccepted: sensorMonitor.cleanData(root.deviceId, root.olderThanDays, root.local, root.cloud,
                                            root.cloud ? passwordField.text : "")
    }
}
