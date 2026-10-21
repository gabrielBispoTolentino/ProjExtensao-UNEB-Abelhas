import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Versão nova do app (appUpdater, ver appupdater.h): pergunta antes de
// baixar, mostra o andamento e o que acontece depois. Fechar o aviso não
// cancela nada; o botão "Atualizar" do menu lateral o abre de novo.
Dialog {
    id: root

    readonly property string phase: appUpdater.state
    readonly property bool windows: Qt.platform.os === "windows"
    // Windows: o app fecha sozinho logo depois; não há mais o que fazer aqui.
    readonly property bool closing: phase === "installing" && windows

    function megabytes(bytes) {
        return Number(bytes / (1024 * 1024)).toLocaleString(Qt.locale(), "f", 1)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(440, parent ? parent.width - 32 : 440)
    modal: true
    padding: 20
    closePolicy: phase === "downloading" || closing ? Popup.NoAutoClose : Popup.CloseOnEscape
    // Título no conteúdo, como no ConfirmDialog (um Text com quebra de linha
    // no header cria um laço na altura do diálogo).
    header: null
    contentWidth: availableWidth
    contentHeight: body.implicitHeight

    background: Rectangle {
        radius: Theme.radius + 4
        color: Theme.paper
    }

    Overlay.modal: Rectangle {
        color: Qt.rgba(0.17, 0.13, 0.09, 0.45)   // tinta, translúcida
    }

    ColumnLayout {
        id: body
        width: root.availableWidth
        spacing: 10

        Text {
            Layout.fillWidth: true
            text: root.phase === "failed" ? "Não foi possível atualizar"
                : root.phase === "downloading" ? "Baixando a versão " + appUpdater.latestVersion
                : root.phase === "installing" ? "Instalando a versão " + appUpdater.latestVersion
                : "Nova versão disponível"
            color: Theme.ink
            font.pixelSize: Theme.lead + 2
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: root.phase === "failed" || root.phase === "installing" ? appUpdater.message
                : root.phase === "downloading" ? ""
                : "A versão " + appUpdater.latestVersion + " está pronta (você usa a "
                  + appUpdater.currentVersion + ")."
                  + (appUpdater.canInstall ? "" : " Baixe-a na página de downloads.")
            color: root.phase === "failed" ? Theme.sting : Theme.ink
            font.pixelSize: Theme.body
            lineHeight: 1.2
            wrapMode: Text.WordWrap
        }

        // Resumo das notas da versão, escrito na publicação (Markdown).
        Text {
            Layout.fillWidth: true
            visible: root.phase === "available" && appUpdater.releaseNotes !== ""
            text: appUpdater.releaseNotes
            textFormat: Text.MarkdownText
            color: Theme.inkSoft
            font.pixelSize: Theme.body
            wrapMode: Text.WordWrap
        }

        ProgressBar {
            id: bar
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: root.phase === "downloading"
            value: appUpdater.bytesTotal > 0 ? appUpdater.bytesReceived / appUpdater.bytesTotal : 0
            Accessible.name: "Andamento do download"

            background: Rectangle {
                implicitHeight: 10
                radius: 5
                color: Theme.honeyPale
            }
            contentItem: Item {
                implicitHeight: 10

                Rectangle {
                    width: bar.visualPosition * parent.width
                    height: parent.height
                    radius: 5
                    color: Theme.honey
                }
            }
        }

        HelpText {
            visible: root.phase === "downloading"
            text: root.megabytes(appUpdater.bytesReceived) + " de "
                  + root.megabytes(appUpdater.bytesTotal) + " MB"
        }

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            visible: root.closing
            running: visible
        }
    }

    // Botões próprios, como no ConfirmDialog: o de dispensar primeiro, a
    // ação depois.
    footer: Item {
        implicitHeight: buttons.visible ? buttons.implicitHeight + 24 : 12

        RowLayout {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.top: parent.top
            anchors.topMargin: 4
            spacing: 10
            visible: !root.closing

            ActionButton {
                text: root.phase === "available" ? "Agora não"
                    : root.phase === "downloading" ? "Cancelar"
                    : "Fechar"
                onClicked: {
                    if (root.phase === "downloading")
                        appUpdater.cancel()
                    else
                        root.close()
                }
            }
            ActionButton {
                visible: root.phase !== "downloading"
                kind: "primary"
                text: root.phase === "installing" ? "Abrir o instalador"
                    : !appUpdater.canInstall ? (root.phase === "failed" ? "Baixar manualmente"
                                                                       : "Abrir página de download")
                    : root.phase === "failed" ? "Tentar de novo"
                    : "Atualizar"
                onClicked: {
                    // Sem instalação no lugar o botão só abre o navegador.
                    if (!appUpdater.canInstall)
                        root.close()
                    appUpdater.install()
                }
            }
        }
    }
}
