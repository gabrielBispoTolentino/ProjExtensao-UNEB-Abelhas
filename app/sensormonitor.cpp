// Monitor das leituras (ver sensormonitor.h).

#include "sensormonitor.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QSet>
#include <QTimeZone>
#include <QVariantMap>

namespace {

// Guardado como texto ISO em UTC ("2026-10-14T16:20:05.000Z"); mostrado no
// fuso do aparelho como "14/10/2026 13:20:05".
QString formatRecordedAt(const QString &iso)
{
    const QDateTime dateTime = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return dateTime.isValid()
               ? dateTime.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss"))
               : iso;
}

const QString cloudSource = QStringLiteral("CLOUD");
const QString bleSource   = QStringLiteral("BLE");

// "1 registro" / "1.234 registros" (separador de milhar brasileiro).
QString recordsText(qint64 count)
{
    return count == 1 ? QStringLiteral("1 registro")
                      : QStringLiteral("%1 registros").arg(QLocale().toString(count));
}

// "1 registro apagado" / "1.234 registros apagados".
QString deletedText(qint64 count)
{
    return count == 1 ? QStringLiteral("1 registro apagado")
                      : QStringLiteral("%1 registros apagados").arg(QLocale().toString(count));
}

// Data de corte para "mais antigos que N dias"; inválida (= sem limite) para 0.
QDateTime cleanupCutoff(int olderThanDays)
{
    return olderThanDays > 0 ? QDateTime::currentDateTimeUtc().addDays(-olderThanDays) : QDateTime();
}

} // namespace

SensorMonitor::SensorMonitor(QObject *parent)
    : QObject(parent)
    , m_cloud(&m_store, this)
    , m_weather(&m_store, this)
#ifdef HAVE_QT_BLUETOOTH
    , m_bleClient(this)
#endif
{
    connect(&m_cloud, &CloudSync::readingsReceived, this, &SensorMonitor::handleCloudReadings);
    connect(&m_cloud, &CloudSync::stateChanged, this, &SensorMonitor::connectionChanged);
    connect(&m_cloud, &CloudSync::logMessage, this, &SensorMonitor::logMessage);
    connect(&m_cloud, &CloudSync::remoteDeleteFinished, this, &SensorMonitor::handleRemoteDeleteFinished);
#ifdef HAVE_QT_BLUETOOTH
    connect(&m_bleClient, &BleSensorClient::statusChanged, this, &SensorMonitor::logMessage);
    connect(&m_bleClient, &BleSensorClient::rawMessageReceived, this, &SensorMonitor::handleBleMessage);
    connect(&m_bleClient, &BleSensorClient::stateChanged, this, &SensorMonitor::connectionChanged);
#endif
}

void SensorMonitor::start(const QString &databasePath)
{
    QString errorMessage;
    if (!m_store.open(databasePath, &errorMessage)) {
        setStatus(errorMessage);
        return;
    }

    emit databasePathChanged();
    loadLatestReadings();
    m_cloud.start();
    m_weather.start();
#ifdef HAVE_QT_BLUETOOTH
    m_bleClient.start();
#endif
}

QString SensorMonitor::status() const
{
    return m_status;
}

bool SensorMonitor::cloudConnected() const
{
    return m_cloud.isOnline() && !m_cloud.hasServerError();
}

QString SensorMonitor::cloudStatusText() const
{
    if (!CloudSync::isConfigured()) {
        return QStringLiteral("Nuvem não configurada");
    }

    const int pending = m_cloud.pendingCount();
    const int rejected = m_cloud.rejectedCount();

    if (!m_cloud.isOnline()) {
        return pending > 0
                   ? QStringLiteral("Sem conexão com a nuvem - %1 guardado(s) para enviar").arg(recordsText(pending))
                   : QStringLiteral("Sem conexão com a nuvem (tentando reconectar...)");
    }
    // O servidor responde, mas recusa os pedidos (chave errada, tabela faltando...).
    if (m_cloud.hasServerError()) {
        return QStringLiteral("A nuvem recusou o acesso - verifique a configuração do Supabase");
    }

    QString text = pending > 0 ? QStringLiteral("Conectado à nuvem - enviando %1").arg(recordsText(pending))
                               : QStringLiteral("Conectado à nuvem");
    if (rejected > 0) {
        text += QStringLiteral(" (%1 recusado(s) pelo servidor)").arg(recordsText(rejected));
    }
    return text;
}

bool SensorMonitor::bleConnected() const
{
#ifdef HAVE_QT_BLUETOOTH
    return m_bleClient.connectedCount() > 0;
#else
    return false;
#endif
}

QString SensorMonitor::bleStatusText() const
{
#ifdef HAVE_QT_BLUETOOTH
    const int count = m_bleClient.connectedCount();
    if (count > 0) {
        return count == 1 ? QStringLiteral("1 placa conectada por Bluetooth")
                          : QStringLiteral("%1 placas conectadas por Bluetooth").arg(count);
    }
    switch (m_bleClient.state()) {
    case BleSensorClient::State::Starting:
    case BleSensorClient::State::Scanning:
    case BleSensorClient::State::Connected:
        return QStringLiteral("Procurando placas por Bluetooth...");
    case BleSensorClient::State::NotFound:
        return QStringLiteral("Nenhuma placa por perto");
    case BleSensorClient::State::BluetoothOff:
        return QStringLiteral("Bluetooth desligado");
    case BleSensorClient::State::NoPermission:
        return QStringLiteral("Sem permissão para usar o Bluetooth");
    case BleSensorClient::State::LocationOff:
        return QStringLiteral("Ative a localização para encontrar as placas");
    case BleSensorClient::State::Error:
        break;
    }
    return QStringLiteral("Erro no Bluetooth");
#else
    return QStringLiteral("Bluetooth indisponível nesta versão");
#endif
}

QString SensorMonitor::databasePath() const
{
    return m_store.databasePath();
}

// Uma entrada por placa, com a leitura mais recente. Usada pela lista no QML.
QVariantList SensorMonitor::devices() const
{
    QVariantList list;
    for (const StoredSensorReading &r : m_devices) {
        QVariantMap entry;
        entry.insert(QStringLiteral("deviceId"),   r.deviceId);
        entry.insert(QStringLiteral("name"),       r.deviceName.isEmpty() ? r.deviceId : r.deviceName);
        entry.insert(QStringLiteral("recordedAt"), r.recordedAt);
        entry.insert(QStringLiteral("recordedAtText"), formatRecordedAt(r.recordedAt));
        entry.insert(QStringLiteral("tempDht"),    r.tempDht);
        entry.insert(QStringLiteral("humidity"),   r.humidity);
        entry.insert(QStringLiteral("source"),     m_deviceSources.value(r.deviceId));
        const auto range = m_ranges.constFind(r.deviceId);
        if (range != m_ranges.cend()) {
            entry.insert(QStringLiteral("tempMin24h"), range->min);
            entry.insert(QStringLiteral("tempMax24h"), range->max);
        }
        list.append(entry);
    }
    return list;
}

QVariantMap SensorMonitor::deviceStats(const QString &deviceId, int rangeHours)
{
    const QDateTime since = rangeHours > 0
                                ? QDateTime::currentDateTimeUtc().addSecs(-qint64(rangeHours) * 3600)
                                : QDateTime();
    QVariantMap stats;
    QString errorMessage;
    if (!m_store.readingStats(deviceId, since, &stats, &errorMessage)) {
        setStatus(errorMessage);
        return {};
    }
    return stats;
}

QVariantMap SensorMonitor::deviceSeries(const QString &deviceId, int rangeHours, int maxPoints)
{
    const QDateTime since = rangeHours > 0
                                ? QDateTime::currentDateTimeUtc().addSecs(-qint64(rangeHours) * 3600)
                                : QDateTime();
    QVariantMap series;
    QString errorMessage;
    if (!m_store.readingSeries(deviceId, since, maxPoints, &series, &errorMessage)) {
        setStatus(errorMessage);
        return {};
    }
    return series;
}

// ── Exportação ("Exportar dados") ──────────────────────────────────────────
QVariantMap SensorMonitor::exportCsv(const QUrl &fileUrl, const QString &deviceId, int rangeHours)
{
    const auto result = [](bool ok, const QString &message) {
        return QVariantMap{{QStringLiteral("ok"), ok}, {QStringLiteral("message"), message}};
    };
    const QDateTime since = rangeHours > 0
                                ? QDateTime::currentDateTimeUtc().addSecs(-qint64(rangeHours) * 3600)
                                : QDateTime();

    // No Android o seletor devolve content://...; o QFile do Qt abre essas
    // URLs pelo texto completo.
    QFile file(fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return result(false, QStringLiteral("Não foi possível criar o arquivo: %1").arg(file.errorString()));
    }
    int count = 0;
    QString errorMessage;
    const bool ok = m_store.exportCsv(&file, deviceId, since, &count, &errorMessage);
    file.close();
    if (!ok) {
        return result(false, QStringLiteral("A exportação falhou: %1").arg(errorMessage));
    }

    QString message = count == 0 ? QStringLiteral("Nenhum registro nessa seleção: o arquivo só tem o cabeçalho.")
                    : count == 1 ? QStringLiteral("1 registro exportado.")
                                 : QStringLiteral("%1 registros exportados.").arg(QLocale().toString(count));
    if (fileUrl.isLocalFile()) {
        message += QStringLiteral(" Arquivo: %1").arg(QDir::toNativeSeparators(fileUrl.toLocalFile()));
    }
    return result(true, message);
}

int SensorMonitor::exportCount(const QString &deviceId, int rangeHours)
{
    const QDateTime since = rangeHours > 0
                                ? QDateTime::currentDateTimeUtc().addSecs(-qint64(rangeHours) * 3600)
                                : QDateTime();
    int count = 0;
    QString errorMessage;
    if (!m_store.countExportRows(deviceId, since, &count, &errorMessage)) {
        setStatus(errorMessage);
    }
    return count;
}

// ── Limpeza ("Limpar dados") ───────────────────────────────────────────────
bool SensorMonitor::cloudConfigured() const
{
    return CloudSync::isConfigured();
}

QVariantMap SensorMonitor::localDataCount(const QString &deviceId, int olderThanDays)
{
    int count = 0;
    int pending = 0;
    QString errorMessage;
    if (!m_store.countReadings(deviceId, cleanupCutoff(olderThanDays), &count, &pending, &errorMessage)) {
        setStatus(errorMessage);
        return {};
    }
    return {{QStringLiteral("count"), count}, {QStringLiteral("pending"), pending}};
}

void SensorMonitor::cleanData(const QString &deviceId, int olderThanDays, bool local, bool cloud,
                              const QString &adminPassword)
{
    if (m_cleaning || (!local && !cloud)) {
        return;
    }
    m_cleaning = true;
    m_cleanupLocal = local;
    m_cleanupDeviceId = deviceId;
    m_cleanupBefore = cleanupCutoff(olderThanDays);   // mesma data de corte nos dois lugares
    m_cleanupMessage = cloud ? QStringLiteral("Apagando na nuvem...") : QStringLiteral("Apagando...");
    m_cleanupFailed = false;
    emit cleanupChanged();

    // Nuvem primeiro: se falhar (senha errada, offline), nada se perde aqui.
    if (cloud) {
        m_cloud.deleteRemote(adminPassword, deviceId, m_cleanupBefore);
        return;
    }

    int deleted = 0;
    QString errorMessage;
    if (!deleteLocalReadings(&deleted, &errorMessage)) {
        finishCleanup(errorMessage, true);
        return;
    }
    finishCleanup(deleted == 0 ? QStringLiteral("Nenhum registro para apagar neste aparelho.")
                               : deletedText(deleted) + QStringLiteral(" deste aparelho."),
                  false);
}

void SensorMonitor::handleRemoteDeleteFinished(CloudSync::DeleteResult result, qint64 deleted, int httpStatus)
{
    if (!m_cleaning) {
        return;
    }

    if (result != CloudSync::DeleteResult::Done) {
        QString reason;
        switch (result) {
        case CloudSync::DeleteResult::Offline:
            reason = QStringLiteral("Sem conexão com a nuvem.");
            break;
        case CloudSync::DeleteResult::WrongPassword:
            reason = QStringLiteral("Senha de administrador incorreta.");
            break;
        case CloudSync::DeleteResult::PasswordNotSet:
            reason = QStringLiteral("A senha de administrador ainda não foi definida no Supabase "
                                    "(veja supabase/schema.sql).");
            break;
        case CloudSync::DeleteResult::NotInstalled:
            reason = QStringLiteral("A limpeza ainda não foi instalada no Supabase: rode de novo o "
                                    "supabase/schema.sql no SQL Editor.");
            break;
        case CloudSync::DeleteResult::Done:
        case CloudSync::DeleteResult::ServerError:
            reason = QStringLiteral("A nuvem recusou o pedido (HTTP %1).").arg(httpStatus);
            break;
        }
        // Lotes já apagados continuam apagados; o texto avisa.
        const QString done = deleted > 0
                                 ? QStringLiteral(" Só %1 da nuvem; nada foi apagado deste aparelho.")
                                       .arg(deletedText(deleted))
                                 : QStringLiteral(" Nada foi apagado.");
        finishCleanup(reason + done, true);
        return;
    }

    QString message = deleted == 0 ? QStringLiteral("Nenhum registro para apagar na nuvem")
                                   : deletedText(deleted) + QStringLiteral(" da nuvem");
    if (m_cleanupLocal) {
        int localDeleted = 0;
        QString errorMessage;
        if (!deleteLocalReadings(&localDeleted, &errorMessage)) {
            finishCleanup(message + QStringLiteral(", mas falhou neste aparelho: ") + errorMessage, true);
            return;
        }
        message += QStringLiteral(" e %1 deste aparelho").arg(recordsText(localDeleted));
    }
    finishCleanup(message + QLatin1Char('.'), false);
}

bool SensorMonitor::deleteLocalReadings(int *deleted, QString *errorMessage)
{
    if (!m_store.deleteReadings(m_cleanupDeviceId, m_cleanupBefore, deleted, errorMessage)) {
        return false;
    }
    m_cloud.localReadingsDeleted();

    // Cartões: mostra o que sobrou (uma placa sem dados some até enviar de novo).
    m_devices.clear();
    m_ranges.clear();
    loadLatestReadings();
    emit devicesChanged();
    return true;
}

void SensorMonitor::finishCleanup(const QString &message, bool failed)
{
    m_cleaning = false;
    m_cleanupMessage = message;
    m_cleanupFailed = failed;
    logMessage(QStringLiteral("Cleanup: %1").arg(message));
    emit cleanupChanged();
    emit dataCleaned();
}

void SensorMonitor::clearCleanupMessage()
{
    if (m_cleaning || m_cleanupMessage.isEmpty()) {
        return;
    }
    m_cleanupMessage.clear();
    m_cleanupFailed = false;
    emit cleanupChanged();
}

void SensorMonitor::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }

    m_status = status;
    qInfo().noquote() << status;
    emit statusChanged();
}

// Detalhes técnicos ficam no log; a interface mostra os textos de estado.
void SensorMonitor::logMessage(const QString &message)
{
    qInfo().noquote() << message;
}

// ── Bluetooth: valores ao vivo (a cada 2 s) e registros ────────────────────
void SensorMonitor::handleBleMessage(const QString &topic, const QByteArray &payload)
{
    Q_UNUSED(topic)

    SensorPayload reading;
    QString errorMessage;
    if (!SensorPayload::fromJson(payload, &reading, &errorMessage)) {
        setStatus(QStringLiteral("JSON parse error: %1").arg(errorMessage));
        qWarning().noquote() << QStringLiteral("Raw payload: %1").arg(QString::fromUtf8(payload));
        return;
    }
    if (reading.deviceId.isEmpty()) {
        reading.deviceId = QStringLiteral("unknown");
    }

    if (!reading.recordFlag || !reading.hasRecordId()) {
        // Valor ao vivo (ou leitura de firmware antigo): só atualiza o
        // cartão, não é guardado nem enviado.
        StoredSensorReading live;
        live.deviceId   = reading.deviceId;
        live.deviceName = reading.name;
        live.tempDht    = reading.tempDht;
        live.humidity   = reading.humidity;
        const QDateTime at = reading.timestamp > 0
                                 ? QDateTime::fromSecsSinceEpoch(reading.timestamp, QTimeZone::UTC)
                                 : QDateTime::currentDateTimeUtc();
        live.recordedAt = at.toString(Qt::ISODateWithMs);
        if (live.deviceName.isEmpty()) {
            live.deviceName = m_devices.value(reading.deviceId).deviceName;
        }
        applyReading(live, bleSource);
        return;
    }

    // Registro: guardado localmente e enviado ao Supabase (agora ou quando houver internet).
    bool inserted = false;
    if (!m_store.insertReading(reading, false, &inserted, &errorMessage)) {
        setStatus(errorMessage);
        return;
    }
    if (!inserted) {
        return;   // já existia (ex.: veio antes pelo Supabase)
    }
    m_cloud.scheduleUpload();
    emit deviceReadingAdded(reading.deviceId);
    showLatestOf(reading.deviceId, bleSource);
}

// ── Supabase: linhas novas desde a última sincronização ────────────────────
void SensorMonitor::handleCloudReadings(const QList<SensorPayload> &readings)
{
    QString errorMessage;
    if (!m_store.insertCloudReadings(readings, &errorMessage)) {
        setStatus(errorMessage);
        return;
    }

    QSet<QString> deviceIds;
    for (const SensorPayload &reading : readings) {
        deviceIds.insert(reading.deviceId);
    }
    for (const QString &deviceId : std::as_const(deviceIds)) {
        emit deviceReadingAdded(deviceId);
        showLatestOf(deviceId, cloudSource);
    }
}

// ── Cartões ────────────────────────────────────────────────────────────────
void SensorMonitor::loadLatestReadings()
{
    QList<StoredSensorReading> readings;
    QString errorMessage;
    if (!m_store.latestReadings(&readings, &errorMessage)) {
        setStatus(errorMessage);
        return;
    }
    for (const StoredSensorReading &reading : std::as_const(readings)) {
        refreshRange(reading.deviceId);
        applyReading(reading, QString());
    }
}

void SensorMonitor::showLatestOf(const QString &deviceId, const QString &source)
{
    StoredSensorReading reading;
    bool found = false;
    QString errorMessage;
    if (!m_store.latestReading(deviceId, &reading, &found, &errorMessage)) {
        setStatus(errorMessage);
        return;
    }
    if (found) {
        refreshRange(deviceId);
        applyReading(reading, source);
    }
}

void SensorMonitor::refreshRange(const QString &deviceId)
{
    double min = 0.0;
    double max = 0.0;
    int count = 0;
    QString errorMessage;
    if (!m_store.temperatureRange(deviceId, QDateTime::currentDateTimeUtc().addDays(-1),
                                  &min, &max, &count, &errorMessage)) {
        setStatus(errorMessage);
        return;
    }
    if (count >= 2) {
        m_ranges.insert(deviceId, {min, max});
    } else {
        m_ranges.remove(deviceId);   // histórico curto demais para uma faixa
    }
}

void SensorMonitor::applyReading(const StoredSensorReading &reading, const QString &source)
{
    // Dados mais antigos (registros enviados com atraso, um lote da nuvem
    // atrás do valor ao vivo do Bluetooth) são guardados, mas o cartão mantém
    // a leitura mais recente.
    const auto current = m_devices.constFind(reading.deviceId);
    if (current != m_devices.cend() && current->recordedAt > reading.recordedAt) {
        return;
    }

    StoredSensorReading shown = reading;
    if (shown.deviceName.isEmpty() && current != m_devices.cend()) {
        shown.deviceName = current->deviceName;   // linhas antigas podem não ter o nome
    }
    m_devices.insert(reading.deviceId, shown);
    if (!source.isEmpty()) {
        m_deviceSources.insert(reading.deviceId, source);
    }
    emit devicesChanged();
}
