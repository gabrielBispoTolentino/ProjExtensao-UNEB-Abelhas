#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

// Atualização do app pelas versões publicadas no GitHub (repositório
// ProjExtensao-UNEB-Abelhas, aba Releases). Ao abrir, pergunta ao GitHub
// qual é a versão mais recente; se for mais nova, o QML oferece atualizar.
//
// Cada versão publicada precisa ter a tag vX.Y.Z e os arquivos
// BeeDuino-X.Y.Z-windows.zip e BeeDuino-X.Y.Z-android.apk (só o fim do nome
// importa: -windows.zip / -android.apk).
// O primeiro parágrafo das notas da versão aparece no aviso do app.
//
//  * Windows: baixa o .zip e chama updater/install-windows.ps1, que espera o
//    app fechar, copia os arquivos novos por cima dos antigos (voltando os
//    antigos se algo falhar) e abre o app de novo. Só funciona na pasta do
//    .zip (BeeDuino.exe numa pasta onde dá para escrever); nas
//    versões de desenvolvimento o botão abre a página de download.
//    Até a 1.4.0 o app se chamava Abelhas Petterson, e o atualizador dessas
//    versões procura AbelhasPetterson.exe no .zip e abre esse arquivo: por
//    isso o .zip leva também uma cópia com o nome antigo, que o script novo
//    não instala e apaga da pasta do app.
//  * Android: baixa o .apk e abre o instalador do sistema. O Android não
//    deixa um app se atualizar sozinho: a pessoa toca em "Atualizar" e, na
//    primeira vez, permite "instalar apps desconhecidos" para este app. O
//    APK novo precisa ter a mesma assinatura do instalado.
//
// O arquivo baixado é conferido pelo tamanho e pelo SHA-256 informados pelo
// GitHub antes de ser instalado.
class AppUpdater : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    // "idle", "checking", "available", "downloading", "installing" ou "failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged)
    // false: o botão de atualizar só abre a página de download.
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY stateChanged)
    // Erro (estado "failed") ou instrução (estado "installing").
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(qint64 bytesReceived READ bytesReceived NOTIFY progressChanged)
    Q_PROPERTY(qint64 bytesTotal READ bytesTotal NOTIFY stateChanged)

public:
    explicit AppUpdater(QObject *parent = nullptr);

    // Ao abrir o app: limpa downloads antigos e procura uma versão nova.
    void start();

    Q_INVOKABLE void check();
    // Baixa e instala a versão nova (ou abre a página, sem canInstall).
    Q_INVOKABLE void install();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openReleasePage();

    QString currentVersion() const;
    QString state() const { return m_state; }
    QString latestVersion() const { return m_latestVersion; }
    QString releaseNotes() const { return m_releaseNotes; }
    bool canInstall() const { return m_canInstall; }
    QString message() const { return m_message; }
    qint64 bytesReceived() const { return m_bytesReceived; }
    qint64 bytesTotal() const { return m_assetSize; }

signals:
    void stateChanged();
    void progressChanged();

private:
    void onCheckFinished(QNetworkReply *reply);
    void onDownloadReadyRead(QNetworkReply *reply);
    void onDownloadFinished(QNetworkReply *reply);
    void launchInstaller();
    void setState(const QString &state, const QString &message = QString());
    void fail(const QString &message);
    QString workDir() const;

    QNetworkAccessManager   m_network;
    QPointer<QNetworkReply> m_reply;

    QString                 m_state = QStringLiteral("idle");
    QString                 m_message;
    QString                 m_latestVersion;
    QString                 m_releaseNotes;
    QUrl                    m_releaseUrl;
    QUrl                    m_assetUrl;
    QString                 m_assetName;
    qint64                  m_assetSize = 0;
    QByteArray              m_assetSha256;   // vazio se o GitHub não informou
    bool                    m_canInstall = false;

    QFile                   m_file;
    QCryptographicHash      m_hash { QCryptographicHash::Sha256 };
    qint64                  m_bytesReceived = 0;
    bool                    m_downloaded = false;   // m_file completo e conferido
};
