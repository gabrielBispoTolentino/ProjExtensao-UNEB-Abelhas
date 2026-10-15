import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Janela principal: faixa do topo, lista de colmeias, menu lateral e as
// outras telas (carregadas sob demanda no Loader).
Window {
    id: window
    width: 720
    height: 760
    visible: true
    title: "Abelhas Petterson"
    color: Theme.wing

    // Cores dos controles Basic (caixas de seleção, campos de texto, listas...).
    palette {
        window: Theme.wing
        windowText: Theme.ink
        base: Theme.paper
        alternateBase: Theme.wing
        text: Theme.ink
        button: Theme.paper
        buttonText: Theme.ink
        brightText: Theme.paper
        highlight: Theme.ink
        highlightedText: Theme.paper
        light: Theme.honeyPale
        midlight: Theme.rule
        mid: Theme.inkFaint
        dark: Theme.ink
        shadow: Theme.ink
        placeholderText: Theme.inkFaint
        link: Theme.sky
    }

    // Janela do tamanho de celular: margens menores.
    readonly property bool compact: width < 600
    readonly property real gutter: compact ? 16 : 24
    // Texto e leituras continuam legíveis em janelas largas: uma coluna
    // centralizada, compartilhada pela faixa do topo e pelo conteúdo.
    readonly property real contentMaxWidth: 760
    readonly property real columnX: page.safeLeft
        + Math.max(gutter, (width - page.safeLeft - page.safeRight - contentMaxWidth) / 2)
    readonly property real columnWidth: Math.min(width - page.safeLeft - page.safeRight - 2 * gutter,
                                                 contentMaxWidth)

    // No celular o menu abre deslizando da direita; no computador, pelo botão ☰.
    readonly property bool isMobile: Qt.platform.os === "android" || Qt.platform.os === "ios"

    // Ferramentas da placa exigem Bluetooth (boardControl é null sem o Qt Bluetooth).
    readonly property bool hasBoardTools: boardControl !== null

    // Ferramentas do menu lateral; "" é a própria lista de colmeias.
    readonly property var tools: [
        { page: "wifi",     title: "Configurar Wi-Fi",      detail: "Conectar uma placa à internet" },
        { page: "name",     title: "Nome da placa",         detail: "Renomear uma placa" },
        { page: "interval", title: "Intervalo de registro", detail: "Com que frequência a placa grava" },
        { page: "restart",  title: "Reiniciar placa",       detail: "Reiniciar uma placa à distância" }
    ]
    // Ferramentas de dados: funcionam sem Bluetooth.
    readonly property var dataTools: [
        { page: "export",  title: "Exportar dados",    detail: "Salvar as leituras numa planilha (CSV)" },
        { page: "weather", title: "Local do apiário",  detail: "Cidade da temperatura externa" },
        { page: "cleanup", title: "Limpar dados",      detail: "Apagar leituras do aparelho ou da nuvem" }
    ]
    property string currentPage: ""

    // Placa cuja tela de estatísticas/gráfico (página "device") está aberta.
    property string selectedDeviceId: ""
    property string selectedDeviceName: ""

    // Uma placa grava a cada 1 a 60 minutos: só depois disso (mais o tempo
    // de envio) vale avisar que ela ficou em silêncio.
    readonly property real staleMs: 65 * 60 * 1000
    // Relógio dos textos "há 5 min".
    property real now: Date.now()
    Timer {
        interval: 15000
        running: true
        repeat: true
        onTriggered: window.now = Date.now()
    }

    function openPage(pageName) {
        currentPage = pageName
        sideMenu.close()
    }

    function openDevice(deviceId, name) {
        selectedDeviceId = deviceId
        selectedDeviceName = name
        currentPage = "device"
    }

    function pageTitle(pageName) {
        if (pageName === "device")
            return selectedDeviceName
        for (const tool of tools.concat(dataTools)) {
            if (tool.page === pageName)
                return tool.title
        }
        return "Abelhas Petterson"
    }

    // Números em pt-BR (vírgula decimal), ex.: "24,5".
    function number(value, decimals) {
        return Number(value).toLocaleString(Qt.locale(), "f", decimals)
    }

    // "agora", "há 40 s", "há 5 min", "há 3 h", "há 2 dias".
    function ago(ms) {
        const s = Math.max(0, Math.round(ms / 1000))
        if (s < 10)
            return "agora"
        if (s < 60)
            return "há " + s + " s"
        const m = Math.round(s / 60)
        if (m < 60)
            return "há " + m + " min"
        const h = Math.round(m / 60)
        if (h < 48)
            return "há " + h + " h"
        return "há " + Math.round(h / 24) + " dias"
    }

    function capitalized(text) {
        return text.charAt(0).toUpperCase() + text.slice(1)
    }

    // Botão voltar do Android: fecha o menu, depois sai da tela, depois fecha o app.
    onClosing: (close) => {
        if (sideMenu.opened) {
            close.accepted = false
            sideMenu.close()
        } else if (currentPage !== "") {
            close.accepted = false
            currentPage = ""
        }
    }

    Item {
        id: page
        anchors.fill: parent

        // Espaço das barras de status/navegação do Android (0 no computador).
        readonly property real safeTop: SafeArea.margins.top
        readonly property real safeBottom: SafeArea.margins.bottom
        readonly property real safeLeft: SafeArea.margins.left
        readonly property real safeRight: SafeArea.margins.right
    }

    Component { id: wifiPage;     WifiSetupPage {} }
    Component { id: namePage;     BoardNamePage {} }
    Component { id: restartPage;  RestartBoardPage {} }
    Component { id: intervalPage; RecordIntervalPage {} }
    Component {
        id: devicePage
        DevicePage {
            deviceId: window.selectedDeviceId
            onOpenPageRequested: (pageName) => window.openPage(pageName)
        }
    }
    Component { id: cleanupPage;  DataCleanupPage {} }
    Component { id: exportPage;   DataExportPage {} }
    Component { id: weatherPage;  WeatherLocationPage {} }

    // Estado da nuvem / Bluetooth na faixa do topo: ícone cheio quando conectado.
    component StatusChip: RowLayout {
        property string glyph
        property bool active
        property string text

        spacing: 6

        GlyphIcon {
            glyph: parent.glyph
            size: 16
            color: Theme.ink
            opacity: parent.active ? 1 : 0.4
        }
        Text {
            Layout.fillWidth: true
            text: parent.text
            color: Theme.ink
            font.pixelSize: Theme.small
            wrapMode: Text.WordWrap
        }
    }

    // Um valor ao vivo com a unidade em tamanho menor: "31,4 °C".
    component Reading: Row {
        property real value
        property string unit
        property color tint
        property bool dimmed

        spacing: 4

        Text {
            id: figure
            text: window.number(parent.value, 1)
            color: parent.dimmed ? Theme.inkFaint : parent.tint
            font.pixelSize: Theme.reading
            font.letterSpacing: -1
        }
        Text {
            anchors.baseline: figure.baseline
            text: parent.unit
            color: parent.dimmed ? Theme.inkFaint : Theme.inkSoft
            font.pixelSize: Theme.lead
        }
    }

    // Item do menu lateral para `tools` / `dataTools`: título + linha de detalhe.
    component MenuEntry: ItemDelegate {
        id: entry
        required property var modelData

        Layout.fillWidth: true
        highlighted: window.currentPage === modelData.page
        onClicked: window.openPage(modelData.page)
        topPadding: 10
        bottomPadding: 10
        leftPadding: 20

        background: Rectangle {
            color: entry.highlighted ? Theme.honeyPale : (entry.down ? Theme.wing : "transparent")
            border.width: entry.visualFocus ? 2 : 0
            border.color: Theme.ink

            Rectangle {
                visible: entry.highlighted
                width: 4
                height: parent.height
                color: Theme.honey
            }
        }

        contentItem: Column {
            spacing: 2

            Text {
                text: modelData.title
                color: Theme.ink
                font.pixelSize: Theme.body + 1
                font.bold: entry.highlighted
            }
            Text {
                text: modelData.detail
                color: Theme.inkSoft
                font.pixelSize: Theme.small
            }
        }
    }

    component MenuGroup: Text {
        Layout.leftMargin: 20
        Layout.topMargin: 16
        Layout.bottomMargin: 4
        color: Theme.inkSoft
        font.pixelSize: Theme.small
        font.bold: true
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.bottomMargin: page.safeBottom
        spacing: 0

        // ── Faixa do topo (cor de mel, como a abelha) ──────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: band.implicitHeight + page.safeTop + 12 + 16
            color: Theme.honey

            ColumnLayout {
                id: band
                x: window.columnX
                y: page.safeTop + 12
                width: window.columnWidth
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    ToolButton {
                        id: backButton
                        visible: window.currentPage !== ""
                        Accessible.name: "Voltar"
                        implicitWidth: 40
                        implicitHeight: Theme.touch
                        Layout.leftMargin: -10
                        onClicked: window.currentPage = ""
                        background: Rectangle {
                            radius: Theme.radius
                            color: backButton.down ? Theme.honeyDeep : "transparent"
                            border.width: backButton.visualFocus ? 2 : 0
                            border.color: Theme.ink
                        }
                        contentItem: Item {
                            GlyphIcon {
                                anchors.centerIn: parent
                                glyph: "back"
                                size: 24
                                color: Theme.ink
                            }
                        }
                    }

                    Image {
                        visible: window.currentPage === ""
                        source: "icons/bee.svg"
                        sourceSize.width: 40
                        sourceSize.height: 40
                        Layout.preferredWidth: 40
                        Layout.preferredHeight: 40
                    }

                    Text {
                        Layout.fillWidth: true
                        text: window.pageTitle(window.currentPage)
                        color: Theme.ink
                        font.pixelSize: Theme.title
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    // ☰ desenhado com três barras para não depender de um caractere da fonte.
                    ToolButton {
                        id: menuButton
                        visible: !window.isMobile
                        Accessible.name: "Menu"
                        implicitWidth: Theme.touch
                        implicitHeight: Theme.touch
                        Layout.rightMargin: -10
                        onClicked: sideMenu.open()
                        background: Rectangle {
                            radius: Theme.radius
                            color: menuButton.down ? Theme.honeyDeep : "transparent"
                            border.width: menuButton.visualFocus ? 2 : 0
                            border.color: Theme.ink
                        }
                        contentItem: Item {
                            Column {
                                anchors.centerIn: parent
                                spacing: 4

                                Repeater {
                                    model: 3
                                    Rectangle {
                                        width: 20
                                        height: 2
                                        radius: 1
                                        color: Theme.ink
                                    }
                                }
                            }
                        }
                    }
                }

                // Como as leituras estão chegando ao app (só na lista de colmeias).
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: window.currentPage === ""
                    spacing: 4

                    StatusChip {
                        Layout.fillWidth: true
                        glyph: "wifi"
                        active: sensorMonitor.cloudConnected
                        text: sensorMonitor.cloudStatusText
                    }
                    StatusChip {
                        Layout.fillWidth: true
                        glyph: "bluetooth"
                        active: sensorMonitor.bleConnected
                        text: sensorMonitor.bleStatusText
                    }
                }
            }
        }

        // ── Conteúdo ───────────────────────────────────────────────────────
        Item {
            id: content
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Deslizar da direita para a esquerda em qualquer lugar abre o menu (o
            // arrasto pela borda do Drawer falha em celulares com navegação por
            // gestos). Desligado nas estatísticas: lá o arrasto move a mira do gráfico.
            DragHandler {
                enabled: window.isMobile && !sideMenu.opened && window.currentPage !== "device"
                target: null
                yAxis.enabled: false
                onActiveTranslationChanged: {
                    if (active && activeTranslation.x < -60)
                        sideMenu.open()
                }
            }

            Item {
                id: column
                x: window.columnX
                y: window.gutter
                width: window.columnWidth
                height: parent.height - y

                // Outras telas (criadas sob demanda, para senhas digitadas não ficarem na memória).
                Loader {
                    anchors.fill: parent
                    visible: active
                    active: window.currentPage === "device"
                            || window.dataTools.some(t => t.page === window.currentPage)
                            || (window.hasBoardTools && window.currentPage !== "")
                    sourceComponent: window.currentPage === "device" ? devicePage
                                   : window.currentPage === "cleanup" ? cleanupPage
                                   : window.currentPage === "export" ? exportPage
                                   : window.currentPage === "weather" ? weatherPage
                                   : window.currentPage === "wifi" ? wifiPage
                                   : window.currentPage === "name" ? namePage
                                   : window.currentPage === "restart" ? restartPage
                                   : window.currentPage === "interval" ? intervalPage
                                   : null
                }

                // ── Lista de colmeias: uma linha por placa ─────────────────
                ListView {
                    id: deviceList
                    anchors.fill: parent
                    anchors.topMargin: -window.gutter / 2
                    visible: window.currentPage === ""
                    clip: true
                    model: sensorMonitor.devices
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: ItemDelegate {
                        id: row
                        required property var modelData
                        required property int index

                        readonly property real ageMs: window.now - Date.parse(modelData.recordedAt)
                        readonly property bool stale: ageMs > window.staleMs
                        readonly property bool hasRange: modelData.tempMin24h !== undefined

                        width: deviceList.width
                        padding: 0
                        topPadding: 16
                        bottomPadding: 18
                        Accessible.name: modelData.name + ", " + window.number(modelData.tempDht, 1)
                                         + " graus, umidade " + window.number(modelData.humidity, 0)
                                         + " por cento. Abrir estatísticas"
                        onClicked: window.openDevice(modelData.deviceId, modelData.name)

                        background: Rectangle {
                            color: row.down ? Theme.honeyPale : "transparent"
                            border.width: row.visualFocus ? 2 : 0
                            border.color: Theme.ink
                            radius: row.visualFocus ? Theme.radius : 0

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.rule
                            }
                        }

                        contentItem: ColumnLayout {
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    text: row.modelData.name
                                    color: Theme.ink
                                    font.pixelSize: Theme.lead
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                GlyphIcon {
                                    glyph: "chevron"
                                    size: 20
                                    color: Theme.inkSoft
                                }
                            }

                            // Quando e como chegou o último valor; vermelho se a placa silenciou.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                GlyphIcon {
                                    visible: !!row.modelData.source && !row.stale
                                    glyph: row.modelData.source === "BLE" ? "bluetooth" : "wifi"
                                    size: 14
                                    color: Theme.inkSoft
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: row.stale
                                          ? "Sem leituras " + window.ago(row.ageMs)
                                          : window.capitalized(window.ago(row.ageMs))
                                            + (row.modelData.source === "BLE" ? ", por Bluetooth"
                                               : row.modelData.source === "CLOUD" ? ", pela internet" : "")
                                    color: row.stale ? Theme.sting : Theme.inkSoft
                                    font.pixelSize: Theme.small
                                    font.bold: row.stale
                                    elide: Text.ElideRight
                                }
                            }

                            Flow {
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                                spacing: 28

                                Reading {
                                    value: row.modelData.tempDht
                                    unit: "°C"
                                    tint: Theme.propolis
                                    dimmed: row.stale
                                }
                                Reading {
                                    value: row.modelData.humidity
                                    unit: "%"
                                    tint: Theme.sky
                                    dimmed: row.stale
                                }
                            }

                            RangeBand {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 460
                                visible: row.hasRange && !row.stale
                                current: row.modelData.tempDht
                                low: row.hasRange ? row.modelData.tempMin24h : 0
                                high: row.hasRange ? row.modelData.tempMax24h : 0
                            }
                        }
                    }

                    // Nada recebido ainda: explica de onde vêm as leituras.
                    ColumnLayout {
                        width: deviceList.width
                        y: window.gutter / 2
                        visible: deviceList.count === 0
                        spacing: 6

                        Text {
                            Layout.fillWidth: true
                            text: "Nenhuma colmeia ainda"
                            color: Theme.ink
                            font.pixelSize: Theme.lead
                            font.bold: true
                        }
                        HelpText {
                            text: "As colmeias aparecem aqui assim que uma placa enviar leituras pela "
                                  + "internet ou se conectar a este aparelho por Bluetooth."
                        }
                    }
                }
            }
        }
    }

    // ── Menu lateral (abre pela direita) ───────────────────────────────────
    Drawer {
        id: sideMenu
        edge: Qt.RightEdge
        width: Math.min(320, window.width * 0.85)
        height: window.height
        // Computador: só o botão ☰ abre (sem arrastos acidentais do mouse).
        dragMargin: window.isMobile ? 32 : 0

        background: Rectangle {
            color: Theme.paper
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: page.safeTop + 20
            anchors.bottomMargin: page.safeBottom + 12
            anchors.rightMargin: page.safeRight
            spacing: 0

            Text {
                Layout.leftMargin: 20
                Layout.bottomMargin: 8
                text: "Menu"
                color: Theme.ink
                font.pixelSize: Theme.title
                font.bold: true
            }

            MenuEntry {
                modelData: ({ page: "", title: "Colmeias", detail: "Leituras de cada placa agora" })
            }

            MenuGroup {
                visible: window.hasBoardTools
                text: "Placa, por Bluetooth"
            }

            Repeater {
                model: window.hasBoardTools ? window.tools : []

                delegate: MenuEntry {}
            }

            MenuGroup {
                text: "Dados"
            }

            Repeater {
                model: window.dataTools

                delegate: MenuEntry {}
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
