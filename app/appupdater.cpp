// Atualização do app pelo GitHub Releases (ver appupdater.h).

#include "appupdater.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QVersionNumber>

namespace {

// Repositório definitivo (público: a consulta não precisa de chave).
constexpr auto latestReleaseApi =
    "https://api.github.com/repos/gabrielBispoTolentino/ProjExtensao-UNEB-Abelhas/releases/latest";
constexpr auto releasesPage =
    "https://github.com/gabrielBispoTolentino/ProjExtensao-UNEB-Abelhas/releases/latest";

constexpr int checkTimeoutMs    = 30000;
// Tempo sem receber nenhum byte, não a duração do download inteiro.
constexpr int downloadTimeoutMs = 60000;

// Passado pelo install-windows.ps1 ao abrir de novo a versão antiga.
constexpr auto updateFailedArgument = "--update-failed";

// Arquivo da versão para esta plataforma, pelo fim do nome.
QString assetSuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("-windows.zip");
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("-android.apk");
#else
    return QString();
#endif
}

QNetworkRequest githubRequest(const QUrl &url, int timeoutMs)
{
    QNetworkRequest request(url);
    // O GitHub recusa pedidos sem User-Agent.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("BeeDuino/%1").arg(QStringLiteral(APP_VERSION)));
    request.setTransferTimeout(timeoutMs);
    return request;
}

// Primeiro parágrafo das notas, até o primeiro título "## ..." (o resto é a
// tabela de downloads e instruções de instalação manual).
QString releaseSummary(QString body)
{
    body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    static const QRegularExpression heading(QStringLiteral("^#"), QRegularExpression::MultilineOption);
    const auto match = heading.match(body);
    if (match.hasMatch()) {
        body.truncate(match.capturedStart());
    }
    return body.trimmed();
}

// Windows: só a pasta do .zip pode ser atualizada no lugar. Um executável
// com outro nome é uma versão de desenvolvimento (pasta de build).
// AbelhasPetterson.exe: o atualizador da 1.4.0 ou anterior instalou esta
// versão e a abriu pelo nome antigo (ver appupdater.h).
bool appFolderInstallable()
{
#if defined(Q_OS_WIN)
    const QFileInfo exe(QCoreApplication::applicationFilePath());
    if (exe.fileName().compare(QStringLiteral("BeeDuino.exe"), Qt::CaseInsensitive) != 0
        && exe.fileName().compare(QStringLiteral("AbelhasPetterson.exe"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    // Testa criando um arquivo: as permissões do NTFS não aparecem no QFileInfo.
    QTemporaryFile probe(QDir(exe.absolutePath()).filePath(QStringLiteral("gravavel-XXXXXX")));
    return probe.open();
#elif defined(Q_OS_ANDROID)
    return true;
#else
    return false;
#endif
}

} // namespace

AppUpdater::AppUpdater(QObject *parent)
    : QObject(parent)
{
}

QString AppUpdater::currentVersion() const
{
    return QStringLiteral(APP_VERSION);
}

void AppUpdater::start()
{
    if (QCoreApplication::arguments().contains(QString::fromLatin1(updateFailedArgument))) {
        fail(QStringLiteral("A atualização não pôde ser instalada e o app continua na versão %1. "
                            "Detalhes em %2.")
                 .arg(currentVersion(),
                      QDir::toNativeSeparators(QDir(workDir()).filePath(QStringLiteral("atualizacao.log")))));
        return;
    }

    // Pacotes de uma atualização já instalada ou interrompida. O log e o
    // script ficam (são pequenos e ajudam a entender uma falha).
    QDir dir(workDir());
    const QStringList packages = dir.entryList({ QStringLiteral("*.zip"), QStringLiteral("*.apk") }, QDir::Files);
    for (const QString &name : packages) {
        dir.remove(name);
    }
    check();
}

// ── Consulta ao GitHub ─────────────────────────────────────────────────────
void AppUpdater::check()
{
    if (m_reply || m_state == QStringLiteral("installing")) {
        return;
    }
    // Endereço trocável só para testes, com um servidor local no lugar do GitHub.
    const QString override = qEnvironmentVariable("BEEDUINO_UPDATE_API_URL");
    const QUrl url(override.isEmpty() ? QString::fromLatin1(latestReleaseApi) : override);

    QNetworkRequest request = githubRequest(url, checkTimeoutMs);
    request.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onCheckFinished(reply); });
    setState(QStringLiteral("checking"));
}

void AppUpdater::onCheckFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    m_reply = nullptr;

    // Sem internet ou GitHub fora do ar: tenta de novo na próxima abertura,
    // sem incomodar ninguém.
    if (reply->error() != QNetworkReply::NoError) {
        qWarning().noquote() << QStringLiteral("Update check failed: %1").arg(reply->errorString());
        setState(QStringLiteral("idle"));
        return;
    }
    const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
    QString tag = release.value(QStringLiteral("tag_name")).toString();
    if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        tag.remove(0, 1);
    }
    const QVersionNumber latest = QVersionNumber::fromString(tag);
    if (latest.isNull() || release.value(QStringLiteral("draft")).toBool()
        || release.value(QStringLiteral("prerelease")).toBool()
        || latest <= QVersionNumber::fromString(currentVersion())) {
        setState(QStringLiteral("idle"));
        return;
    }

    m_latestVersion = latest.toString();
    m_releaseNotes = releaseSummary(release.value(QStringLiteral("body")).toString());
    m_releaseUrl = QUrl(release.value(QStringLiteral("html_url")).toString());
    m_assetUrl.clear();
    m_assetName.clear();
    m_assetSize = 0;
    m_assetSha256.clear();
    m_downloaded = false;

    const QString suffix = assetSuffix();
    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (suffix.isEmpty() || !name.endsWith(suffix, Qt::CaseInsensitive)) {
            continue;
        }
        m_assetName = QFileInfo(name).fileName();   // sem pastas vindas do servidor
        m_assetUrl = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
        m_assetSize = asset.value(QStringLiteral("size")).toInteger();
        // "sha256:<hex>" (versões publicadas antes de 2025 não têm).
        const QString digest = asset.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(QStringLiteral("sha256:"))) {
            m_assetSha256 = QByteArray::fromHex(digest.mid(7).toLatin1());
        }
        break;
    }
    m_canInstall = m_assetUrl.isValid() && !m_assetName.isEmpty() && appFolderInstallable();
    setState(QStringLiteral("available"));
}

// ── Download ───────────────────────────────────────────────────────────────
void AppUpdater::install()
{
    if (!m_canInstall) {
        openReleasePage();
        return;
    }
    if (m_reply) {
        return;
    }
    // Android: o instalador foi fechado sem instalar; abre de novo.
    if (m_downloaded && QFile::exists(m_file.fileName())) {
        launchInstaller();
        return;
    }

    QDir().mkpath(workDir());
    m_file.setFileName(QDir(workDir()).filePath(m_assetName));
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Não foi possível gravar o download: %1").arg(m_file.errorString()));
        return;
    }
    m_hash.reset();
    m_bytesReceived = 0;
    m_downloaded = false;

    QNetworkReply *reply = m_network.get(githubRequest(m_assetUrl, downloadTimeoutMs));
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { onDownloadReadyRead(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onDownloadFinished(reply); });
    setState(QStringLiteral("downloading"));
    emit progressChanged();
}

void AppUpdater::cancel()
{
    if (m_state != QStringLiteral("downloading") || !m_reply) {
        return;
    }
    // m_reply limpo antes do abort(): a resposta cancelada é ignorada.
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->abort();
    reply->deleteLater();
    m_file.close();
    m_file.remove();
    setState(QStringLiteral("available"));
}

void AppUpdater::onDownloadReadyRead(QNetworkReply *reply)
{
    if (reply != m_reply) {
        return;
    }
    const QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        return;
    }
    if (m_file.write(data) != data.size()) {
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
        const QString error = m_file.errorString();
        m_file.close();
        m_file.remove();
        fail(QStringLiteral("Não foi possível gravar o download (falta espaço?): %1").arg(error));
        return;
    }
    m_hash.addData(data);
    m_bytesReceived += data.size();
    emit progressChanged();
}

void AppUpdater::onDownloadFinished(QNetworkReply *reply)
{
    if (reply != m_reply) {
        return;   // cancelado
    }
    onDownloadReadyRead(reply);
    if (reply != m_reply) {
        return;   // falhou ao gravar o resto
    }
    m_reply = nullptr;
    reply->deleteLater();
    m_file.close();

    QString problem;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        qWarning().noquote() << QStringLiteral("Update download failed (HTTP %1): %2")
                                    .arg(status).arg(reply->errorString());
        problem = QStringLiteral("Não foi possível baixar a versão %1. Confira a internet e tente de novo.")
                      .arg(m_latestVersion);
    } else if (m_assetSize > 0 && m_bytesReceived != m_assetSize) {
        problem = QStringLiteral("O download veio incompleto. Tente de novo.");
    } else if (!m_assetSha256.isEmpty() && m_hash.result() != m_assetSha256) {
        problem = QStringLiteral("O arquivo baixado não confere com o publicado (SHA-256). Tente de novo.");
    }
    if (!problem.isEmpty()) {
        m_file.remove();
        fail(problem);
        return;
    }
    m_downloaded = true;
    launchInstaller();
}

// ── Instalação ─────────────────────────────────────────────────────────────
void AppUpdater::launchInstaller()
{
    const QString package = m_file.fileName();
#if defined(Q_OS_WIN)
    // O script sai do app (recurso) para a pasta de trabalho: o PowerShell
    // precisa de um arquivo .ps1 de verdade.
    const QString script = QDir(workDir()).filePath(QStringLiteral("install-windows.ps1"));
    QFile::remove(script);
    if (!QFile::copy(QStringLiteral(":/qt/qml/BeeDuino/updater/install-windows.ps1"), script)) {
        fail(QStringLiteral("Não foi possível preparar o instalador."));
        return;
    }
    // A cópia de um recurso é somente leitura; sem isto não daria para apagá-la depois.
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner);

    // Caminho completo: não depende do PATH. Sem janela de console (o Qt
    // inicia processos de um app sem console com CREATE_NO_WINDOW).
    const QString powershell = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")))
                                   .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    const QStringList arguments = {
        QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
        QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
        QStringLiteral("-File"), QDir::toNativeSeparators(script),
        QStringLiteral("-ProcessId"), QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("-Package"), QDir::toNativeSeparators(package),
        QStringLiteral("-AppDir"), QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
        QStringLiteral("-WorkDir"), QDir::toNativeSeparators(workDir()),
    };
    if (!QProcess::startDetached(QDir::toNativeSeparators(powershell), arguments)) {
        fail(QStringLiteral("Não foi possível iniciar o instalador (PowerShell)."));
        return;
    }
    setState(QStringLiteral("installing"),
             QStringLiteral("O app vai fechar e abrir de novo já na versão %1.").arg(m_latestVersion));
    // Um instante para a mensagem aparecer; o script espera o app fechar.
    QTimer::singleShot(1500, qApp, &QCoreApplication::quit);
#elif defined(Q_OS_ANDROID)
    // O Qt entrega arquivos locais por um FileProvider (pasta files/ do app,
    // onde workDir() fica), e o tipo .apk abre o instalador do Android.
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(package))) {
        fail(QStringLiteral("Não foi possível abrir o instalador do Android."));
        return;
    }
    setState(QStringLiteral("installing"),
             QStringLiteral("Toque em \"Atualizar\" na tela do Android. Na primeira vez, o Android "
                            "pede para permitir a instalação de apps vindos deste app."));
#else
    Q_UNUSED(package);
#endif
}

void AppUpdater::openReleasePage()
{
    QDesktopServices::openUrl(m_releaseUrl.isValid() ? m_releaseUrl : QUrl(QString::fromLatin1(releasesPage)));
}

void AppUpdater::setState(const QString &state, const QString &message)
{
    if (state == m_state && message == m_message) {
        return;
    }
    m_state = state;
    m_message = message;
    emit stateChanged();
}

void AppUpdater::fail(const QString &message)
{
    qWarning().noquote() << QStringLiteral("Update failed: %1").arg(message);
    setState(QStringLiteral("failed"), message);
}

QString AppUpdater::workDir() const
{
#if defined(Q_OS_ANDROID)
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("update"));
#else
    // Caminho curto: o .zip tem caminhos de até ~140 caracteres e o
    // Expand-Archive não passa de 260 no total.
    return QDir(QDir::tempPath()).filePath(QStringLiteral("BeeDuinoUpdate"));
#endif
}
