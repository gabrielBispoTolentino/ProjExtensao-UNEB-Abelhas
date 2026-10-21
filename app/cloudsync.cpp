// Sincronização com o Supabase (ver cloudsync.h).

#include "cloudsync.h"

#include "sensorreadingstore.h"
#include "supabaseconfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>
#include <QtNumeric>

namespace {

constexpr int pullIntervalMs   = 10000;   // quão atualizados ficam os cartões
constexpr int uploadDelayMs    = 2000;    // junta registros que chegam juntos
constexpr int retryBaseMs      = 30000;   // primeira nova tentativa após falha...
constexpr int retryMaxMs       = 600000;  // ...dobrando até 10 min
constexpr int pageSize         = 1000;    // linhas por download (máximo padrão do Supabase)
constexpr int uploadBatch      = 500;
constexpr int requestTimeoutMs = 20000;
// Linhas inseridas até este tempo antes da mais recente vista são buscadas de
// novo, caso tenham aparecido depois de linhas com id maior.
constexpr int recheckWindowSecs = 120;

// Linhas apagadas por chamada de delete_readings (cada uma precisa terminar
// nos poucos segundos que o Supabase permite à chave pública).
constexpr int deleteBatch      = 10000;

constexpr auto lastRemoteIdKey     = "supabase_last_id";
constexpr auto latestInsertedAtKey = "supabase_last_inserted_at";
constexpr auto recheckFloorKey     = "supabase_recheck_floor";

// Código HTTP de uma resposta; 0 quando o servidor nem foi alcançado.
int httpStatus(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

} // namespace

CloudSync::CloudSync(SensorReadingStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_batchLimit(uploadBatch)
    , m_pushRetryMs(retryBaseMs)
{
    m_pullTimer.setInterval(pullIntervalMs);
    connect(&m_pullTimer, &QTimer::timeout, this, [this]() { pull(); });

    m_pushTimer.setSingleShot(true);
    connect(&m_pushTimer, &QTimer::timeout, this, &CloudSync::push);
}

bool CloudSync::isConfigured()
{
    return !QString::fromLatin1(SupabaseConfig::url).contains(QLatin1String("YOUR-PROJECT"))
        && !QString::fromLatin1(SupabaseConfig::key).startsWith(QLatin1String("YOUR-"));
}

void CloudSync::start()
{
    if (!isConfigured()) {
        emit logMessage(QStringLiteral("Supabase: not configured (supabaseconfig.h)"));
        return;
    }

    m_lastRemoteId = m_store->syncValue(QString::fromLatin1(lastRemoteIdKey)).toLongLong();
    m_latestInsertedAt = QDateTime::fromString(m_store->syncValue(QString::fromLatin1(latestInsertedAtKey)),
                                               Qt::ISODateWithMs);
    m_recheckFloor = QDateTime::fromString(m_store->syncValue(QString::fromLatin1(recheckFloorKey)),
                                           Qt::ISODateWithMs);
    refreshCounts();
    m_pullTimer.start();
    pull();
    push();
}

void CloudSync::scheduleUpload()
{
    refreshCounts();
    // Não encurta a espera após uma falha; senão, envia logo.
    if (isConfigured() && !m_pushing && !m_pushTimer.isActive()) {
        m_pushTimer.start(uploadDelayMs);
    }
}

void CloudSync::localReadingsDeleted()
{
    // Tudo o que foi inserido até a linha mais recente vista já foi baixado,
    // então a conferência pode pular essas linhas. (Um envio que apareça fora
    // de ordem exatamente agora pode ficar de fora; ele continua no Supabase.)
    if (m_latestInsertedAt.isValid()) {
        m_recheckFloor = m_latestInsertedAt.addMSecs(1);
        QString errorMessage;
        if (!m_store->setSyncValue(QString::fromLatin1(recheckFloorKey),
                                   m_recheckFloor.toUTC().toString(Qt::ISODateWithMs), &errorMessage)) {
            emit logMessage(errorMessage);
        }
    }
    ++m_pullGeneration;
    refreshCounts();
}

QNetworkRequest CloudSync::request(const QString &pathAndQuery) const
{
    QNetworkRequest request(QUrl(QString::fromLatin1(SupabaseConfig::url) + pathAndQuery));
    const QByteArray key(SupabaseConfig::key);
    request.setRawHeader("apikey", key);
    // Chaves anon antigas são JWT e vão também em Authorization; as novas
    // sb_publishable_ só podem ir como apikey.
    if (!key.startsWith("sb_")) {
        request.setRawHeader("Authorization", "Bearer " + key);
    }
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(requestTimeoutMs);
    return request;
}

// ── Baixar: linhas novas no Supabase desde a última vista ──────────────────
void CloudSync::pull(bool continuation)
{
    if (m_pulling) {
        return;
    }
    m_pulling = true;

    // Linhas novas pelo id; na primeira página, confere também a janela recente.
    QString filter = QStringLiteral("id=gt.%1").arg(m_lastRemoteId);
    if (!continuation && m_latestInsertedAt.isValid()) {
        QDateTime sinceTime = m_latestInsertedAt.addSecs(-recheckWindowSecs);
        if (m_recheckFloor.isValid() && m_recheckFloor > sinceTime) {
            sinceTime = m_recheckFloor;
        }
        const QString since = sinceTime.toUTC().toString(Qt::ISODateWithMs);
        const QString orFilter = QStringLiteral("(id.gt.%1,inserted_at.gte.\"%2\")")
                                     .arg(m_lastRemoteId).arg(since);
        filter = QStringLiteral("or=") + QString::fromLatin1(QUrl::toPercentEncoding(orFilter));
    }

    const QString query =
        QStringLiteral("select=id,device_id,device_name,recorded_at,temperature,humidity,boot,seq,inserted_at")
        + QLatin1Char('&') + filter
        + QStringLiteral("&order=id.asc&limit=%1").arg(pageSize);

    QNetworkReply *reply = m_network.get(request(QStringLiteral("/rest/v1/readings?") + query));
    const int generation = m_pullGeneration;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        onPullFinished(reply, generation);
    });
}

void CloudSync::onPullFinished(QNetworkReply *reply, int generation)
{
    reply->deleteLater();
    m_pulling = false;

    if (generation != m_pullGeneration) {
        return;   // pedido antes de uma limpeza local: o próximo download pede de novo
    }

    if (reply->error() != QNetworkReply::NoError) {
        const int status = httpStatus(reply);
        const QString detail = QStringLiteral("%1 %2").arg(reply->errorString(),
                                                           QString::fromUtf8(reply->readAll()));
        emit logMessage(QStringLiteral("Supabase: pull failed - %1").arg(detail));
        if (status == 0) {
            setOnline(false);                       // sem internet / servidor inalcançável
        } else {
            setOnline(true);                        // alcançado, mas recusou (chave, tabela...)
            setPullError(QStringLiteral("HTTP %1").arg(status));
        }
        return;
    }
    setOnline(true);
    setPullError(QString());

    const QJsonArray rows = QJsonDocument::fromJson(reply->readAll()).array();
    if (rows.isEmpty()) {
        return;
    }

    QList<SensorPayload> readings;
    readings.reserve(rows.size());
    qint64 lastId = m_lastRemoteId;
    QDateTime latestInsertedAt = m_latestInsertedAt;
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        lastId = std::max(lastId, static_cast<qint64>(row.value(QStringLiteral("id")).toDouble()));
        const QDateTime insertedAt = QDateTime::fromString(row.value(QStringLiteral("inserted_at")).toString(),
                                                           Qt::ISODateWithMs);
        if (insertedAt.isValid() && (!latestInsertedAt.isValid() || insertedAt > latestInsertedAt)) {
            latestInsertedAt = insertedAt;
        }

        SensorPayload reading;
        reading.deviceId   = row.value(QStringLiteral("device_id")).toString();
        reading.name       = row.value(QStringLiteral("device_name")).toString();
        reading.tempDht    = row.value(QStringLiteral("temperature")).toDouble();
        const QJsonValue humidity = row.value(QStringLiteral("humidity"));
        reading.humidity   = humidity.isDouble() ? humidity.toDouble() : qQNaN();   // NULL: desconhecida
        reading.boot       = static_cast<qint64>(row.value(QStringLiteral("boot")).toDouble());
        reading.seq        = static_cast<qint64>(row.value(QStringLiteral("seq")).toDouble());
        reading.recordFlag = true;   // toda linha do Supabase é um registro
        // O Postgres envia, por exemplo, "2026-10-14T16:20:05+00:00".
        reading.timestamp  = QDateTime::fromString(row.value(QStringLiteral("recorded_at")).toString(),
                                                   Qt::ISODateWithMs).toSecsSinceEpoch();
        readings.append(reading);
    }

    emit readingsReceived(readings);   // guardadas (duplicatas ignoradas) antes de o cursor andar

    m_lastRemoteId = lastId;
    m_latestInsertedAt = latestInsertedAt;
    QString errorMessage;
    if (!m_store->setSyncValue(QString::fromLatin1(lastRemoteIdKey), QString::number(lastId), &errorMessage)
        || (latestInsertedAt.isValid()
            && !m_store->setSyncValue(QString::fromLatin1(latestInsertedAtKey),
                                      latestInsertedAt.toUTC().toString(Qt::ISODateWithMs), &errorMessage))) {
        emit logMessage(errorMessage);
    }
    refreshCounts();   // cópias da nuvem podem ter coberto registros pendentes

    if (rows.size() == pageSize) {
        pull(true);    // há mais (primeira sincronização, ou muito tempo offline)
    }
}

// ── Enviar: registros do Bluetooth que o Supabase ainda não tem ────────────
void CloudSync::push()
{
    if (m_pushing || !isConfigured()) {
        return;
    }

    QList<SensorReadingStore::PendingUpload> uploads;
    QString errorMessage;
    if (!m_store->pendingUploads(m_batchLimit, &uploads, &errorMessage)) {
        emit logMessage(errorMessage);
        return;
    }
    if (uploads.isEmpty()) {
        return;
    }

    QJsonArray rows;
    QList<qint64> localIds;
    for (const SensorReadingStore::PendingUpload &upload : uploads) {
        // Toda linha precisa ter as mesmas chaves (o PostgREST recusa lotes
        // misturados), então valores ausentes vão como null.
        QJsonObject row;
        row.insert(QStringLiteral("device_id"), upload.deviceId);
        row.insert(QStringLiteral("device_name"), upload.deviceName.isEmpty()
                                                      ? QJsonValue(QJsonValue::Null)
                                                      : QJsonValue(upload.deviceName));
        row.insert(QStringLiteral("recorded_at"), upload.recordedAt);
        row.insert(QStringLiteral("temperature"), upload.temperature);
        row.insert(QStringLiteral("humidity"), upload.humidity.isNull()
                                                   ? QJsonValue(QJsonValue::Null)
                                                   : QJsonValue(upload.humidity.toDouble()));
        row.insert(QStringLiteral("boot"), upload.boot);
        row.insert(QStringLiteral("seq"), upload.seq);
        row.insert(QStringLiteral("source"), QStringLiteral("app"));
        rows.append(row);
        localIds.append(upload.localId);
    }

    QNetworkRequest req = request(QStringLiteral("/rest/v1/readings?on_conflict=device_id,boot,seq"));
    // Registros que o Supabase já tem são pulados em vez de falhar o lote.
    req.setRawHeader("Prefer", "resolution=ignore-duplicates,return=minimal");

    m_pushing = true;
    QNetworkReply *reply = m_network.post(req, QJsonDocument(rows).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, localIds]() {
        onPushFinished(reply, localIds);
    });
}

void CloudSync::onPushFinished(QNetworkReply *reply, const QList<qint64> &localIds)
{
    reply->deleteLater();
    m_pushing = false;
    QString errorMessage;

    if (reply->error() == QNetworkReply::NoError) {
        setOnline(true);
        if (!m_store->markSynced(localIds, &errorMessage)) {
            emit logMessage(errorMessage);
            return;
        }
        emit logMessage(QStringLiteral("Supabase: uploaded %1 record(s)").arg(localIds.size()));
        m_batchLimit = std::min(uploadBatch, m_batchLimit * 2);
        m_pushRetryMs = retryBaseMs;
        refreshCounts();
        if (m_pendingCount > 0) {
            m_pushTimer.start(0);   // há mais
        }
        return;
    }

    const int status = httpStatus(reply);
    emit logMessage(QStringLiteral("Supabase: upload of %1 record(s) failed - %2 %3")
                        .arg(localIds.size())
                        .arg(reply->errorString(), QString::fromUtf8(reply->readAll())));

    if (status == 0) {
        // Sem internet: tenta de novo quando o servidor voltar (ou após a espera).
        setOnline(false);
        m_pushWaitsForNetwork = true;
        retryPushLater();
        return;
    }
    setOnline(true);

    const bool dataRefused = status >= 400 && status < 500 && status != 408 && status != 429;
    if (!dataRefused) {
        retryPushLater();   // servidor ocupado / erro temporário
        return;
    }

    if (localIds.size() > 1) {
        // Algum registro do lote foi recusado: divide o lote ao meio até isolá-lo.
        m_batchLimit = std::max(1, int(localIds.size()) / 2);
        m_pushTimer.start(0);
        return;
    }

    // Este registro sozinho foi recusado: separa-o para os outros subirem.
    emit logMessage(QStringLiteral("Supabase: record %1 refused, it will not be retried").arg(localIds.first()));
    if (!m_store->markRejected(localIds.first(), &errorMessage)) {
        emit logMessage(errorMessage);
        retryPushLater();
        return;
    }
    refreshCounts();
    m_pushTimer.start(0);
}

void CloudSync::retryPushLater()
{
    m_pushTimer.start(m_pushRetryMs);
    m_pushRetryMs = std::min(m_pushRetryMs * 2, retryMaxMs);
}

// ── Apagar: leituras removidas do Supabase (senha de administrador) ────────
void CloudSync::deleteRemote(const QString &adminPassword, const QString &deviceId, const QDateTime &before)
{
    if (m_deleting) {
        return;
    }
    m_deleting = true;
    m_deletePassword = adminPassword;
    m_deleteDeviceId = deviceId;
    m_deleteBefore = before;
    m_deletedCount = 0;
    deleteRemoteBatch();
}

void CloudSync::deleteRemoteBatch()
{
    QJsonObject args;
    args.insert(QStringLiteral("admin_password"), m_deletePassword);
    args.insert(QStringLiteral("target_device_id"), m_deleteDeviceId.isEmpty()
                                                        ? QJsonValue(QJsonValue::Null)
                                                        : QJsonValue(m_deleteDeviceId));
    args.insert(QStringLiteral("recorded_before"), m_deleteBefore.isValid()
                                                       ? QJsonValue(m_deleteBefore.toUTC().toString(Qt::ISODateWithMs))
                                                       : QJsonValue(QJsonValue::Null));
    args.insert(QStringLiteral("max_rows"), deleteBatch);

    QNetworkReply *reply = m_network.post(request(QStringLiteral("/rest/v1/rpc/delete_readings")),
                                          QJsonDocument(args).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onDeleteBatchFinished(reply); });
}

void CloudSync::onDeleteBatchFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    const QByteArray body = reply->readAll();

    const auto finish = [this](DeleteResult result, int status = 0) {
        m_deleting = false;
        m_deletePassword.clear();
        emit remoteDeleteFinished(result, m_deletedCount, status);
    };

    if (reply->error() == QNetworkReply::NoError) {
        setOnline(true);
        // A função devolve só um número, ex.: 10000.
        bool ok = false;
        const qint64 deleted = body.trimmed().toLongLong(&ok);
        if (!ok) {
            emit logMessage(QStringLiteral("Supabase: unexpected delete reply - %1").arg(QString::fromUtf8(body)));
            finish(DeleteResult::ServerError, httpStatus(reply));
            return;
        }
        m_deletedCount += deleted;
        if (deleted >= deleteBatch) {
            deleteRemoteBatch();   // ainda há mais
        } else {
            emit logMessage(QStringLiteral("Supabase: deleted %1 reading(s)").arg(m_deletedCount));
            finish(DeleteResult::Done);
        }
        return;
    }

    const int status = httpStatus(reply);
    // Corpo de erro do PostgREST: {"code": "28P01", "message": ...}
    const QString code = QJsonDocument::fromJson(body).object().value(QStringLiteral("code")).toString();
    emit logMessage(QStringLiteral("Supabase: delete failed - %1 %2").arg(reply->errorString(),
                                                                         QString::fromUtf8(body)));
    if (status == 0) {
        setOnline(false);
        finish(DeleteResult::Offline);
    } else if (code == QLatin1String("28P01")) {
        finish(DeleteResult::WrongPassword);
    } else if (code == QLatin1String("28000")) {
        finish(DeleteResult::PasswordNotSet);
    } else if (code == QLatin1String("PGRST202")) {   // função não encontrada
        finish(DeleteResult::NotInstalled);
    } else {
        finish(DeleteResult::ServerError, status);
    }
}

// ── Auxiliares ─────────────────────────────────────────────────────────────
void CloudSync::setOnline(bool online)
{
    if (m_online == online) {
        return;
    }
    m_online = online;
    emit stateChanged();

    // De volta online após falha de rede: envia o que acumulou.
    if (online && m_pushWaitsForNetwork && m_pendingCount > 0 && !m_pushing) {
        m_pushWaitsForNetwork = false;
        m_pushRetryMs = retryBaseMs;
        m_pushTimer.start(uploadDelayMs);
    }
}

void CloudSync::setPullError(const QString &error)
{
    if (m_pullError == error) {
        return;
    }
    m_pullError = error;
    emit stateChanged();
}

void CloudSync::refreshCounts()
{
    int pending = 0;
    int rejected = 0;
    QString errorMessage;
    if (!m_store->pendingUploadCount(&pending, &errorMessage)
        || !m_store->rejectedCount(&rejected, &errorMessage)) {
        emit logMessage(errorMessage);
        return;
    }
    if (pending != m_pendingCount || rejected != m_rejectedCount) {
        m_pendingCount = pending;
        m_rejectedCount = rejected;
        emit stateChanged();
    }
}
