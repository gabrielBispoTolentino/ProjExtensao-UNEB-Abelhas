// Banco SQLite local (ver sensorreadingstore.h).

#include "sensorreadingstore.h"

#include <QDateTime>
#include <QDebug>
#include <QIODevice>
#include <QSqlError>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QUuid>
#include <QVariant>

SensorReadingStore::SensorReadingStore()
    : m_connectionName(QStringLiteral("sensor-store-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

SensorReadingStore::~SensorReadingStore()
{
    if (m_database.isValid()) {
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool SensorReadingStore::open(const QString &path, QString *errorMessage)
{
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(path);

    if (!m_database.open()) {
        setSqlError(QStringLiteral("Failed to open SQLite database"), m_database.lastError(), errorMessage);
        return false;
    }

    return createSchema(errorMessage);
}

bool SensorReadingStore::insertReading(const SensorPayload &reading, bool fromCloud, bool *inserted,
                                       QString *errorMessage)
{
    // Usa o relógio da placa quando ela enviou (registros offline chegam
    // bem depois de medidos); senão, a hora da chegada. Mesmo formato do
    // padrão da coluna, para as linhas ordenarem certo como texto.
    const QDateTime recordedAt = reading.timestamp > 0
                                     ? QDateTime::fromSecsSinceEpoch(reading.timestamp, QTimeZone::UTC)
                                     : QDateTime::currentDateTimeUtc();

    // OR IGNORE + índice único (device_id, boot, seq): um registro que já
    // chegou pelo outro caminho (Bluetooth / Supabase) é ignorado. Linhas
    // sem boot/seq (firmware antigo) ficam NULL e nunca colidem.
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
        INSERT OR IGNORE INTO sensor_readings
            (device_id, device_name, recorded_at, temp_dht, humidity, boot, seq, synced)
        VALUES (:device_id, :device_name, :recorded_at, :temp_dht, :humidity, :boot, :seq, :synced)
    )SQL"));
    query.bindValue(QStringLiteral(":device_id"), reading.deviceId);
    query.bindValue(QStringLiteral(":device_name"),
                    reading.name.isEmpty() ? QVariant() : QVariant(reading.name));
    query.bindValue(QStringLiteral(":recorded_at"), recordedAt.toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":temp_dht"), reading.tempDht);
    query.bindValue(QStringLiteral(":humidity"),
                    std::isnan(reading.humidity) ? QVariant() : QVariant(reading.humidity));
    const bool identified = reading.hasRecordId();
    query.bindValue(QStringLiteral(":boot"), identified ? QVariant(reading.boot) : QVariant());
    query.bindValue(QStringLiteral(":seq"),  identified ? QVariant(reading.seq) : QVariant());
    query.bindValue(QStringLiteral(":synced"), fromCloud ? 1 : 0);

    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to insert sensor reading"), query.lastError(), errorMessage);
        return false;
    }

    const bool wasInserted = query.numRowsAffected() > 0;
    if (inserted) {
        *inserted = wasInserted;
    }

    // Já tínhamos pelo Bluetooth e agora a nuvem também tem: nada a enviar.
    if (!wasInserted && fromCloud && identified) {
        QSqlQuery synced(m_database);
        synced.prepare(QStringLiteral(
            "UPDATE sensor_readings SET synced = 1 WHERE device_id = :device_id AND boot = :boot AND seq = :seq"));
        synced.bindValue(QStringLiteral(":device_id"), reading.deviceId);
        synced.bindValue(QStringLiteral(":boot"), reading.boot);
        synced.bindValue(QStringLiteral(":seq"), reading.seq);
        if (!synced.exec()) {
            setSqlError(QStringLiteral("Failed to mark reading as synced"), synced.lastError(), errorMessage);
            return false;
        }
    }
    return true;
}

bool SensorReadingStore::insertCloudReadings(const QList<SensorPayload> &readings, QString *errorMessage)
{
    if (!m_database.transaction()) {
        setSqlError(QStringLiteral("Failed to start transaction"), m_database.lastError(), errorMessage);
        return false;
    }
    for (const SensorPayload &reading : readings) {
        if (!insertReading(reading, true, nullptr, errorMessage)) {
            m_database.rollback();
            return false;
        }
    }
    if (!m_database.commit()) {
        setSqlError(QStringLiteral("Failed to save cloud readings"), m_database.lastError(), errorMessage);
        m_database.rollback();
        return false;
    }
    return true;
}

namespace {

// Valor de ":since" nas consultas "recorded_at >= :since". recorded_at é
// texto ISO em UTC, então comparar textos compara horários, e '' vem antes
// de qualquer data (= sem limite inferior). Tem que ser '' e não QString():
// o driver do SQLite grava uma QString nula como NULL, e nada é >= NULL.
QString sinceFilter(const QDateTime &since)
{
    return since.isValid() ? since.toUTC().toString(Qt::ISODateWithMs) : QStringLiteral("");
}

StoredSensorReading readStoredReading(const QSqlQuery &query)
{
    StoredSensorReading reading;
    reading.id = query.value(0).toLongLong();
    reading.deviceId = query.value(1).toString();
    reading.recordedAt = query.value(2).toString();
    reading.tempDht = query.value(3).toDouble();
    // humidity é NULL quando a umidade não é conhecida (Supabase, ou linhas
    // anteriores a essa coluna): NaN, e não 0, para a tela mostrar "–".
    reading.humidity = query.value(4).isNull() ? qQNaN() : query.value(4).toDouble();
    reading.deviceName = query.value(5).toString();
    return reading;
}

} // namespace

bool SensorReadingStore::latestReadings(QList<StoredSensorReading> *readings, QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"SQL(
        SELECT r.id, r.device_id, r.recorded_at, r.temp_dht, r.humidity, r.device_name
        FROM sensor_readings r
        JOIN (SELECT device_id, MAX(recorded_at) AS newest
              FROM sensor_readings GROUP BY device_id) latest
          ON r.device_id = latest.device_id AND r.recorded_at = latest.newest
    )SQL"))) {
        setSqlError(QStringLiteral("Failed to fetch latest readings"), query.lastError(), errorMessage);
        return false;
    }

    readings->clear();
    while (query.next()) {
        readings->append(readStoredReading(query));
    }
    return true;
}

bool SensorReadingStore::latestReading(const QString &deviceId, StoredSensorReading *reading, bool *found,
                                       QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
        SELECT id, device_id, recorded_at, temp_dht, humidity, device_name
        FROM sensor_readings
        WHERE device_id = :device_id
        ORDER BY recorded_at DESC
        LIMIT 1
    )SQL"));
    query.bindValue(QStringLiteral(":device_id"), deviceId);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to fetch latest reading"), query.lastError(), errorMessage);
        return false;
    }

    *found = query.next();
    if (*found) {
        *reading = readStoredReading(query);
    }
    return true;
}

bool SensorReadingStore::pendingUploads(int limit, QList<PendingUpload> *uploads, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
        SELECT id, device_id, device_name, recorded_at, temp_dht, humidity, boot, seq
        FROM sensor_readings
        WHERE synced = 0 AND boot IS NOT NULL AND seq IS NOT NULL
        ORDER BY id
        LIMIT :limit
    )SQL"));
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to list readings to upload"), query.lastError(), errorMessage);
        return false;
    }

    uploads->clear();
    while (query.next()) {
        PendingUpload upload;
        upload.localId     = query.value(0).toLongLong();
        upload.deviceId    = query.value(1).toString();
        upload.deviceName  = query.value(2).toString();
        upload.recordedAt  = query.value(3).toString();
        upload.temperature = query.value(4).toDouble();
        upload.humidity    = query.value(5);
        upload.boot        = query.value(6).toLongLong();
        upload.seq         = query.value(7).toLongLong();
        uploads->append(upload);
    }
    return true;
}

bool SensorReadingStore::pendingUploadCount(int *count, QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sensor_readings WHERE synced = 0 AND boot IS NOT NULL AND seq IS NOT NULL"))
        || !query.next()) {
        setSqlError(QStringLiteral("Failed to count readings to upload"), query.lastError(), errorMessage);
        return false;
    }
    *count = query.value(0).toInt();
    return true;
}

bool SensorReadingStore::markSynced(const QList<qint64> &localIds, QString *errorMessage)
{
    if (!m_database.transaction()) {
        setSqlError(QStringLiteral("Failed to start transaction"), m_database.lastError(), errorMessage);
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE sensor_readings SET synced = 1 WHERE id = :id"));
    for (qint64 id : localIds) {
        query.bindValue(QStringLiteral(":id"), id);
        if (!query.exec()) {
            setSqlError(QStringLiteral("Failed to mark readings as synced"), query.lastError(), errorMessage);
            m_database.rollback();
            return false;
        }
    }
    if (!m_database.commit()) {
        setSqlError(QStringLiteral("Failed to mark readings as synced"), m_database.lastError(), errorMessage);
        m_database.rollback();
        return false;
    }
    return true;
}

bool SensorReadingStore::markRejected(qint64 localId, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE sensor_readings SET synced = 2 WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), localId);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to mark reading as rejected"), query.lastError(), errorMessage);
        return false;
    }
    return true;
}

bool SensorReadingStore::rejectedCount(int *count, QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM sensor_readings WHERE synced = 2")) || !query.next()) {
        setSqlError(QStringLiteral("Failed to count rejected readings"), query.lastError(), errorMessage);
        return false;
    }
    *count = query.value(0).toInt();
    return true;
}

namespace {

// Cláusula WHERE de countReadings/deleteReadings; os valores entram por
// bindCleanupFilter. recorded_at é texto ISO em UTC, então comparar textos
// compara horários.
QString cleanupFilter(const QString &deviceId, const QDateTime &before)
{
    QStringList conditions{QStringLiteral("1 = 1")};
    if (!deviceId.isEmpty()) {
        conditions.append(QStringLiteral("device_id = :device_id"));
    }
    if (before.isValid()) {
        conditions.append(QStringLiteral("recorded_at < :before"));
    }
    return conditions.join(QStringLiteral(" AND "));
}

void bindCleanupFilter(QSqlQuery *query, const QString &deviceId, const QDateTime &before)
{
    if (!deviceId.isEmpty()) {
        query->bindValue(QStringLiteral(":device_id"), deviceId);
    }
    if (before.isValid()) {
        query->bindValue(QStringLiteral(":before"), before.toUTC().toString(Qt::ISODateWithMs));
    }
}

} // namespace

bool SensorReadingStore::countReadings(const QString &deviceId, const QDateTime &before, int *count,
                                       int *pending, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*), COALESCE(SUM(synced = 0 AND boot IS NOT NULL AND seq IS NOT NULL), 0) "
        "FROM sensor_readings WHERE %1").arg(cleanupFilter(deviceId, before)));
    bindCleanupFilter(&query, deviceId, before);
    if (!query.exec() || !query.next()) {
        setSqlError(QStringLiteral("Failed to count readings"), query.lastError(), errorMessage);
        return false;
    }
    *count = query.value(0).toInt();
    *pending = query.value(1).toInt();
    return true;
}

bool SensorReadingStore::deleteReadings(const QString &deviceId, const QDateTime &before, int *deleted,
                                        QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM sensor_readings WHERE %1").arg(cleanupFilter(deviceId, before)));
    bindCleanupFilter(&query, deviceId, before);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to delete readings"), query.lastError(), errorMessage);
        return false;
    }
    *deleted = query.numRowsAffected();
    query.finish();

    // O SQLite mantém as páginas liberadas dentro do arquivo até reconstruí-lo.
    // Uma falha aqui não é grave (as linhas já foram apagadas).
    if (*deleted > 0) {
        QSqlQuery vacuum(m_database);
        if (!vacuum.exec(QStringLiteral("VACUUM"))) {
            qWarning().noquote() << QStringLiteral("SQLite VACUUM failed: %1").arg(vacuum.lastError().text());
        }
    }
    return true;
}

QString SensorReadingStore::syncValue(const QString &key) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM sync_state WHERE key = :key"));
    query.bindValue(QStringLiteral(":key"), key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool SensorReadingStore::setSyncValue(const QString &key, const QString &value, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO sync_state (key, value) VALUES (:key, :value) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.bindValue(QStringLiteral(":key"), key);
    query.bindValue(QStringLiteral(":value"), value);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to save sync state"), query.lastError(), errorMessage);
        return false;
    }
    return true;
}

bool SensorReadingStore::readingStats(const QString &deviceId, const QDateTime &since,
                                      QVariantMap *stats, QString *errorMessage)
{
    const QString sinceText = sinceFilter(since);

    QSqlQuery totals(m_database);
    totals.prepare(QStringLiteral(R"SQL(
        SELECT COUNT(*), MIN(recorded_at), MAX(recorded_at),
               AVG(temp_dht), MIN(temp_dht), MAX(temp_dht), AVG(temp_dht * temp_dht),
               COUNT(humidity), AVG(humidity), MIN(humidity), MAX(humidity), AVG(humidity * humidity),
               AVG(CASE WHEN humidity IS NOT NULL THEN temp_dht END),
               AVG(CASE WHEN humidity IS NOT NULL THEN temp_dht * temp_dht END),
               AVG(temp_dht * humidity)
        FROM sensor_readings
        WHERE device_id = :device_id AND recorded_at >= :since
    )SQL"));
    totals.bindValue(QStringLiteral(":device_id"), deviceId);
    totals.bindValue(QStringLiteral(":since"), sinceText);
    if (!totals.exec() || !totals.next()) {
        setSqlError(QStringLiteral("Failed to compute statistics"), totals.lastError(), errorMessage);
        return false;
    }

    const auto msecs = [](const QVariant &iso) {
        return QDateTime::fromString(iso.toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
    };
    // Desvio padrão populacional a partir de E[x] e E[x²].
    const auto stdDev = [](double mean, double meanOfSquares) {
        return std::sqrt(std::max(0.0, meanOfSquares - mean * mean));
    };

    QVariantMap result;
    const qint64 count = totals.value(0).toLongLong();
    result.insert(QStringLiteral("count"), count);
    if (count == 0) {
        *stats = result;
        return true;
    }
    result.insert(QStringLiteral("firstAt"), msecs(totals.value(1)));
    result.insert(QStringLiteral("lastAt"),  msecs(totals.value(2)));

    QVariantMap temperature;
    const double tempMean = totals.value(3).toDouble();
    temperature.insert(QStringLiteral("count"),   count);
    temperature.insert(QStringLiteral("average"), tempMean);
    temperature.insert(QStringLiteral("min"),     totals.value(4).toDouble());
    temperature.insert(QStringLiteral("max"),     totals.value(5).toDouble());
    temperature.insert(QStringLiteral("stdDev"),  stdDev(tempMean, totals.value(6).toDouble()));

    QVariantMap humidity;
    const qint64 humidityCount = totals.value(7).toLongLong();
    const double humMean = totals.value(8).toDouble();
    humidity.insert(QStringLiteral("count"), humidityCount);
    if (humidityCount > 0) {
        humidity.insert(QStringLiteral("average"), humMean);
        humidity.insert(QStringLiteral("min"),     totals.value(9).toDouble());
        humidity.insert(QStringLiteral("max"),     totals.value(10).toDouble());
        humidity.insert(QStringLiteral("stdDev"),  stdDev(humMean, totals.value(11).toDouble()));
    }

    // Correlação de Pearson entre temperatura e umidade (linhas com as duas).
    if (humidityCount >= 3) {
        const double tMean = totals.value(12).toDouble();
        const double tVar  = totals.value(13).toDouble() - tMean * tMean;
        const double hVar  = totals.value(11).toDouble() - humMean * humMean;
        const double cov   = totals.value(14).toDouble() - tMean * humMean;
        if (tVar > 1e-9 && hVar > 1e-9) {
            result.insert(QStringLiteral("correlation"),
                          std::clamp(cov / std::sqrt(tVar * hVar), -1.0, 1.0));
        }
    }

    // Uma linha do período escolhida pelo ORDER BY: quando/quanto para os
    // valores atual, inicial, mínimo e máximo.
    const auto fetchRow = [&](const QString &column, const QString &order,
                              QVariant *at, QVariant *value) -> bool {
        QSqlQuery row(m_database);
        row.prepare(QStringLiteral(R"SQL(
            SELECT recorded_at, %1
            FROM sensor_readings
            WHERE device_id = :device_id AND recorded_at >= :since AND %1 IS NOT NULL
            ORDER BY %2
            LIMIT 1
        )SQL").arg(column, order));
        row.bindValue(QStringLiteral(":device_id"), deviceId);
        row.bindValue(QStringLiteral(":since"), sinceText);
        if (!row.exec()) {
            setSqlError(QStringLiteral("Failed to compute statistics"), row.lastError(), errorMessage);
            return false;
        }
        if (row.next()) {
            *at = msecs(row.value(0));
            *value = row.value(1).toDouble();
        }
        return true;
    };

    const auto addExtremes = [&](const QString &column, QVariantMap *measure) -> bool {
        QVariant at, value, firstValue;
        QVariant ignored;
        if (!fetchRow(column, QStringLiteral("recorded_at DESC"), &at, &value)) return false;
        measure->insert(QStringLiteral("current"), value);
        measure->insert(QStringLiteral("currentAt"), at);
        if (!fetchRow(column, QStringLiteral("recorded_at ASC"), &ignored, &firstValue)) return false;
        if (value.isValid() && firstValue.isValid()) {
            measure->insert(QStringLiteral("change"), value.toDouble() - firstValue.toDouble());
        }
        if (!fetchRow(column, QStringLiteral("%1 ASC, recorded_at DESC").arg(column), &at, &value)) return false;
        measure->insert(QStringLiteral("minAt"), at);
        if (!fetchRow(column, QStringLiteral("%1 DESC, recorded_at DESC").arg(column), &at, &value)) return false;
        measure->insert(QStringLiteral("maxAt"), at);
        return true;
    };

    if (!addExtremes(QStringLiteral("temp_dht"), &temperature)) {
        return false;
    }
    if (humidityCount > 0 && !addExtremes(QStringLiteral("humidity"), &humidity)) {
        return false;
    }

    // Temperatura externa na hora de cada leitura: a diferença mostra quanto
    // a colônia aquece (ou resfria) a colmeia em relação ao tempo lá fora.
    QSqlQuery outside(m_database);
    outside.prepare(QStringLiteral(R"SQL(
        SELECT COUNT(o.temp), AVG(o.temp), MIN(o.temp), MAX(o.temp), AVG(r.temp_dht - o.temp)
        FROM sensor_readings r
        JOIN outside_temperature o
          ON o.hour = (CAST(strftime('%s', r.recorded_at) AS INTEGER) + 1800) / 3600 * 3600
        WHERE r.device_id = :device_id AND r.recorded_at >= :since
    )SQL"));
    outside.bindValue(QStringLiteral(":device_id"), deviceId);
    outside.bindValue(QStringLiteral(":since"), sinceText);
    if (!outside.exec() || !outside.next()) {
        setSqlError(QStringLiteral("Failed to compute statistics"), outside.lastError(), errorMessage);
        return false;
    }
    if (outside.value(0).toLongLong() > 0) {
        temperature.insert(QStringLiteral("outsideAverage"),    outside.value(1).toDouble());
        temperature.insert(QStringLiteral("outsideMin"),        outside.value(2).toDouble());
        temperature.insert(QStringLiteral("outsideMax"),        outside.value(3).toDouble());
        temperature.insert(QStringLiteral("outsideDifference"), outside.value(4).toDouble());
    }

    result.insert(QStringLiteral("temperature"), temperature);
    result.insert(QStringLiteral("humidity"), humidity);
    *stats = result;
    return true;
}

bool SensorReadingStore::temperatureRange(const QString &deviceId, const QDateTime &since,
                                          double *min, double *max, int *count, QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
        SELECT COUNT(*), MIN(temp_dht), MAX(temp_dht)
        FROM sensor_readings
        WHERE device_id = :device_id AND recorded_at >= :since
    )SQL"));
    query.bindValue(QStringLiteral(":device_id"), deviceId);
    query.bindValue(QStringLiteral(":since"), sinceFilter(since));
    if (!query.exec() || !query.next()) {
        setSqlError(QStringLiteral("Failed to compute temperature range"), query.lastError(), errorMessage);
        return false;
    }
    *count = query.value(0).toInt();
    *min = query.value(1).toDouble();
    *max = query.value(2).toDouble();
    return true;
}

bool SensorReadingStore::readingSeries(const QString &deviceId, const QDateTime &since, int maxPoints,
                                       QVariantMap *series, QString *errorMessage)
{
    const QString sinceText = sinceFilter(since);

    // Intervalo realmente coberto pelas leituras (primeira -> última), para
    // que uma placa com 1 h de dados na visão de 24 h ainda tenha pontos
    // detalhados e o gráfico comece na primeira leitura.
    QSqlQuery bounds(m_database);
    bounds.prepare(QStringLiteral(R"SQL(
        SELECT MIN(recorded_at), MAX(recorded_at)
        FROM sensor_readings
        WHERE device_id = :device_id AND recorded_at >= :since
    )SQL"));
    bounds.bindValue(QStringLiteral(":device_id"), deviceId);
    bounds.bindValue(QStringLiteral(":since"), sinceText);
    if (!bounds.exec()) {
        setSqlError(QStringLiteral("Failed to load chart data"), bounds.lastError(), errorMessage);
        return false;
    }

    QVariantMap result;
    if (!bounds.next() || bounds.value(0).isNull()) {
        result.insert(QStringLiteral("bucketMs"), 60000);
        result.insert(QStringLiteral("points"), QVariantList());
        *series = result;
        return true;
    }
    const qint64 firstMs = QDateTime::fromString(bounds.value(0).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
    const qint64 lastMs  = QDateTime::fromString(bounds.value(1).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
    const qint64 spanSecs = std::max<qint64>(1, (lastMs - firstMs) / 1000);
    const qint64 bucketSecs = std::max<qint64>(1, (spanSecs + maxPoints - 1) / std::max(1, maxPoints));

    // Média das leituras que caem no mesmo intervalo de bucketSecs.
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
        SELECT CAST(strftime('%s', recorded_at) AS INTEGER) / :bucket AS bucket,
               AVG(temp_dht), AVG(humidity),
               AVG((julianday(recorded_at) - 2440587.5) * 86400000.0)
        FROM sensor_readings
        WHERE device_id = :device_id AND recorded_at >= :since
        GROUP BY bucket
        ORDER BY bucket
    )SQL"));
    query.bindValue(QStringLiteral(":bucket"), bucketSecs);
    query.bindValue(QStringLiteral(":device_id"), deviceId);
    query.bindValue(QStringLiteral(":since"), sinceText);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to load chart data"), query.lastError(), errorMessage);
        return false;
    }

    QVariantList points;
    while (query.next()) {
        QVariantMap point;
        // Hora média das leituras do intervalo (ms desde a época, para datas
        // do JavaScript): sempre entre a primeira e a última leitura.
        point.insert(QStringLiteral("t"), qRound64(query.value(3).toDouble()));
        point.insert(QStringLiteral("temp"), query.value(1).toDouble());
        point.insert(QStringLiteral("hum"), query.value(2).isNull() ? QVariant() : QVariant(query.value(2).toDouble()));
        points.append(point);
    }

    result.insert(QStringLiteral("bucketMs"), bucketSecs * 1000);
    result.insert(QStringLiteral("firstMs"), firstMs);
    result.insert(QStringLiteral("lastMs"), lastMs);
    result.insert(QStringLiteral("points"), points);
    *series = result;
    return true;
}

// ── Temperatura externa ────────────────────────────────────────────────────
bool SensorReadingStore::saveOutsideTemperatures(const QList<QPair<qint64, double>> &hours,
                                                 QString *errorMessage)
{
    if (!m_database.transaction()) {
        setSqlError(QStringLiteral("Failed to start transaction"), m_database.lastError(), errorMessage);
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO outside_temperature (hour, temp) VALUES (:hour, :temp) "
        "ON CONFLICT(hour) DO UPDATE SET temp = excluded.temp"));
    for (const auto &hour : hours) {
        query.bindValue(QStringLiteral(":hour"), hour.first);
        query.bindValue(QStringLiteral(":temp"), hour.second);
        if (!query.exec()) {
            setSqlError(QStringLiteral("Failed to save outside temperature"), query.lastError(), errorMessage);
            m_database.rollback();
            return false;
        }
    }
    if (!m_database.commit()) {
        setSqlError(QStringLiteral("Failed to save outside temperature"), m_database.lastError(), errorMessage);
        m_database.rollback();
        return false;
    }
    return true;
}

bool SensorReadingStore::clearOutsideTemperatures(QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("DELETE FROM outside_temperature"))) {
        setSqlError(QStringLiteral("Failed to clear outside temperature"), query.lastError(), errorMessage);
        return false;
    }
    return true;
}

bool SensorReadingStore::outsideTemperatureBounds(qint64 *firstSecs, qint64 *lastSecs, QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT COALESCE(MIN(hour), 0), COALESCE(MAX(hour), 0) FROM outside_temperature"))
        || !query.next()) {
        setSqlError(QStringLiteral("Failed to read outside temperature"), query.lastError(), errorMessage);
        return false;
    }
    *firstSecs = query.value(0).toLongLong();
    *lastSecs = query.value(1).toLongLong();
    return true;
}

bool SensorReadingStore::oldestReadingSecs(qint64 *secs, QString *errorMessage)
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT COALESCE(CAST(strftime('%s', MIN(recorded_at)) AS INTEGER), 0) FROM sensor_readings"))
        || !query.next()) {
        setSqlError(QStringLiteral("Failed to read readings"), query.lastError(), errorMessage);
        return false;
    }
    *secs = query.value(0).toLongLong();
    return true;
}

bool SensorReadingStore::outsideSeries(qint64 fromMs, qint64 toMs, qint64 bucketMs, QVariantList *points,
                                       QString *errorMessage)
{
    // Uma hora antes e depois: a linha chega até as bordas do gráfico.
    const qint64 bucketSecs = std::max<qint64>(3600, bucketMs / 1000);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
        SELECT hour / :bucket AS bucket, AVG(hour), AVG(temp)
        FROM outside_temperature
        WHERE hour >= :from AND hour <= :to
        GROUP BY bucket
        ORDER BY bucket
    )SQL"));
    query.bindValue(QStringLiteral(":bucket"), bucketSecs);
    query.bindValue(QStringLiteral(":from"), fromMs / 1000 - 3600);
    query.bindValue(QStringLiteral(":to"), toMs / 1000 + 3600);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to load outside temperature"), query.lastError(), errorMessage);
        return false;
    }
    points->clear();
    while (query.next()) {
        points->append(QVariantMap{
            {QStringLiteral("t"), qRound64(query.value(1).toDouble() * 1000.0)},
            {QStringLiteral("temp"), query.value(2).toDouble()},
        });
    }
    return true;
}

// ── Exportação ─────────────────────────────────────────────────────────────
namespace {

// Campo de texto do CSV: entre aspas quando tem ";", aspas ou quebra de linha.
QString csvText(const QString &text)
{
    if (!text.contains(QLatin1Char(';')) && !text.contains(QLatin1Char('"'))
        && !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r'))) {
        return text;
    }
    QString quoted = text;
    quoted.replace(QLatin1String("\""), QLatin1String("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

// Número com vírgula decimal ("31,25"); vazio quando falta o valor.
QString csvNumber(const QVariant &value)
{
    if (value.isNull()) {
        return QString();
    }
    return QString::number(value.toDouble(), 'f', 2).replace(QLatin1Char('.'), QLatin1Char(','));
}

} // namespace

bool SensorReadingStore::exportCsv(QIODevice *device, const QString &deviceId, const QDateTime &since,
                                   int *count, QString *errorMessage)
{
    // Nome atual de cada placa (o do registro mais recente que tem nome),
    // para todas as linhas de uma placa terem o mesmo nome mesmo depois de
    // renomeada. MAX() faz o SQLite pegar device_name da linha mais recente.
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
        WITH names AS (
            SELECT device_id, device_name, MAX(recorded_at)
            FROM sensor_readings
            WHERE device_name IS NOT NULL AND device_name <> ''
            GROUP BY device_id
        )
        SELECT COALESCE(n.device_name, r.device_id), r.device_id, r.recorded_at,
               r.temp_dht, r.humidity, o.temp
        FROM sensor_readings r
        LEFT JOIN names n ON n.device_id = r.device_id
        LEFT JOIN outside_temperature o
          ON o.hour = (CAST(strftime('%s', r.recorded_at) AS INTEGER) + 1800) / 3600 * 3600
        WHERE %1 AND r.recorded_at >= :since
        ORDER BY r.device_id, r.recorded_at
    )SQL").arg(deviceId.isEmpty() ? QStringLiteral("1 = 1") : QStringLiteral("r.device_id = :device_id")));
    if (!deviceId.isEmpty()) {
        query.bindValue(QStringLiteral(":device_id"), deviceId);
    }
    query.bindValue(QStringLiteral(":since"),
                    sinceFilter(since));
    query.setForwardOnly(true);
    if (!query.exec()) {
        setSqlError(QStringLiteral("Failed to export readings"), query.lastError(), errorMessage);
        return false;
    }

    // BOM: sem ele o Excel lê o arquivo como ANSI e estraga os acentos.
    // Linhas terminadas em CRLF, como o Excel espera.
    QByteArray buffer("\xEF\xBB\xBF");
    buffer += "colmeia;id_placa;data_hora;data_hora_utc;temperatura_c;umidade_pct;temperatura_externa_c\r\n";
    int rows = 0;
    while (query.next()) {
        const QDateTime at = QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs);
        const QStringList fields{
            csvText(query.value(0).toString()),
            csvText(query.value(1).toString()),
            at.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")),
            at.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ")),
            csvNumber(query.value(3)),
            csvNumber(query.value(4)),
            csvNumber(query.value(5)),
        };
        buffer += fields.join(QLatin1Char(';')).toUtf8();
        buffer += "\r\n";
        ++rows;
        // Escreve aos poucos: um histórico grande não fica todo na memória.
        if (buffer.size() > (1 << 16)) {
            if (device->write(buffer) != buffer.size()) {
                if (errorMessage) *errorMessage = device->errorString();
                return false;
            }
            buffer.clear();
        }
    }
    if (!buffer.isEmpty() && device->write(buffer) != buffer.size()) {
        if (errorMessage) *errorMessage = device->errorString();
        return false;
    }
    *count = rows;
    return true;
}

bool SensorReadingStore::countExportRows(const QString &deviceId, const QDateTime &since, int *count,
                                         QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM sensor_readings WHERE %1 AND recorded_at >= :since")
                      .arg(deviceId.isEmpty() ? QStringLiteral("1 = 1") : QStringLiteral("device_id = :device_id")));
    if (!deviceId.isEmpty()) {
        query.bindValue(QStringLiteral(":device_id"), deviceId);
    }
    query.bindValue(QStringLiteral(":since"),
                    sinceFilter(since));
    if (!query.exec() || !query.next()) {
        setSqlError(QStringLiteral("Failed to count readings"), query.lastError(), errorMessage);
        return false;
    }
    *count = query.value(0).toInt();
    return true;
}

bool SensorReadingStore::createSchema(QString *errorMessage)
{
    // ── Migração de versões antigas ────────────────────────────────────────
    // O formato desta tabela mudou ao longo das versões do app:
    //   1. Original: humidity REAL NOT NULL           -> inserções falhavam
    //   2. Correção anterior: sem coluna humidity     -> umidade sempre 0
    //   3. humidity REAL, aceita NULL, lida e gravada
    //   4. Atual: igual à 3, mais device_id (uma linha por leitura de ESP32)
    //
    // Qualquer um desses formatos pode estar no aparelho do usuário. O
    // CREATE TABLE IF NOT EXISTS abaixo não faz nada se a tabela já existe,
    // então os formatos antigos são detectados e corrigidos aqui antes.
    {
        QSqlQuery tableCheck(m_database);
        if (!tableCheck.exec(QStringLiteral(
                "SELECT name FROM sqlite_master WHERE type='table' AND name='sensor_readings'"))) {
            setSqlError(QStringLiteral("Failed to inspect SQLite schema"), tableCheck.lastError(), errorMessage);
            return false;
        }

        const bool tableExists = tableCheck.next();
        // Libera a consulta agora - senão ela pode manter uma trava em
        // sqlite_master/sensor_readings e os DROP/ALTER seguintes falham com
        // "database table is locked".
        tableCheck.finish();

        if (tableExists) {
            QSqlQuery columnInfo(m_database);
            if (!columnInfo.exec(QStringLiteral("PRAGMA table_info(sensor_readings)"))) {
                setSqlError(QStringLiteral("Failed to inspect sensor_readings columns"), columnInfo.lastError(), errorMessage);
                return false;
            }

            bool hasHumidityColumn = false;
            bool humidityIsNotNull = false;
            bool hasDeviceIdColumn = false;
            while (columnInfo.next()) {
                const QString columnName = columnInfo.value(QStringLiteral("name")).toString();
                if (columnName.compare(QStringLiteral("humidity"), Qt::CaseInsensitive) == 0) {
                    hasHumidityColumn = true;
                    humidityIsNotNull = columnInfo.value(QStringLiteral("notnull")).toInt() != 0;
                } else if (columnName.compare(QStringLiteral("device_id"), Qt::CaseInsensitive) == 0) {
                    hasDeviceIdColumn = true;
                }
            }
            // Mesmo motivo de antes: finish() antes do ALTER/DROP na tabela
            // que esta consulta estava lendo.
            columnInfo.finish();

            if (!hasHumidityColumn) {
                // Caso 2: a coluna tinha sido removida. Adicionar uma coluna
                // que aceita NULL e sem padrão é sempre seguro no SQLite e
                // preserva todas as linhas.
                QSqlQuery addColumn(m_database);
                if (!addColumn.exec(QStringLiteral(
                        "ALTER TABLE sensor_readings ADD COLUMN humidity REAL"))) {
                    setSqlError(QStringLiteral("Failed to add humidity column"), addColumn.lastError(), errorMessage);
                    return false;
                }
            } else if (humidityIsNotNull) {
                // Caso 1: restrição NOT NULL antiga. O SQLite não remove um
                // NOT NULL no lugar, então a tabela é reconstruída (já com
                // device_id).
                if (!m_database.transaction()) {
                    setSqlError(QStringLiteral("Failed to start schema migration transaction"), m_database.lastError(), errorMessage);
                    return false;
                }

                QSqlQuery migrate(m_database);

                // A tabela antiga pode ou não ter device_id.
                const QString deviceIdSelect = hasDeviceIdColumn
                                                   ? QStringLiteral("device_id")
                                                   : QStringLiteral("'legacy'");

                const bool migrationOk =
                    migrate.exec(QStringLiteral(R"SQL(
                        CREATE TABLE sensor_readings_migrated (
                            id INTEGER PRIMARY KEY AUTOINCREMENT,
                            device_id TEXT NOT NULL DEFAULT 'legacy',
                            recorded_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                            temp_dht REAL NOT NULL,
                            humidity REAL
                        )
                    )SQL"))
                    && migrate.exec(QStringLiteral(R"SQL(
                        INSERT INTO sensor_readings_migrated (id, device_id, recorded_at, temp_dht, humidity)
                        SELECT id, %1, recorded_at, temp_dht, humidity
                        FROM sensor_readings
                        WHERE temp_dht IS NOT NULL
                    )SQL").arg(deviceIdSelect))
                    && migrate.exec(QStringLiteral("DROP TABLE sensor_readings"))
                    && migrate.exec(QStringLiteral(
                        "ALTER TABLE sensor_readings_migrated RENAME TO sensor_readings"));

                if (!migrationOk) {
                    setSqlError(QStringLiteral("Failed to migrate sensor_readings schema"), migrate.lastError(), errorMessage);
                    m_database.rollback();
                    return false;
                }

                if (!m_database.commit()) {
                    setSqlError(QStringLiteral("Failed to commit schema migration"), m_database.lastError(), errorMessage);
                    m_database.rollback();
                    return false;
                }

                hasDeviceIdColumn = true; // a tabela reconstruída já tem
            }

            // Caso 3 -> 4: tabela certa, mas ainda sem device_id. As linhas
            // existentes recebem 'legacy'.
            if (!hasDeviceIdColumn) {
                QSqlQuery addDeviceId(m_database);
                if (!addDeviceId.exec(QStringLiteral(
                        "ALTER TABLE sensor_readings ADD COLUMN device_id TEXT NOT NULL DEFAULT 'legacy'"))) {
                    setSqlError(QStringLiteral("Failed to add device_id column"), addDeviceId.lastError(), errorMessage);
                    return false;
                }
            }
        }
    }
    // ── Fim da migração ────────────────────────────────────────────────────

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(R"SQL(
        CREATE TABLE IF NOT EXISTS sensor_readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL DEFAULT 'legacy',
            recorded_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
            temp_dht REAL NOT NULL,
            humidity REAL,
            boot INTEGER,
            seq INTEGER,
            device_name TEXT,
            synced INTEGER NOT NULL DEFAULT 0
        )
    )SQL"))) {
        setSqlError(QStringLiteral("Failed to update SQLite schema"), query.lastError(), errorMessage);
        return false;
    }

    // Colunas adicionadas depois da primeira versão (instalações novas já as têm).
    // synced: 0 = registro recebido por Bluetooth, ainda não está no Supabase;
    //         1 = o Supabase já tem (ou nunca deve ser enviado);
    //         2 = o Supabase recusou (dado inválido) - não é reenviado.
    bool addedSynced = false;
    if (!ensureColumn(QStringLiteral("boot"), QStringLiteral("INTEGER"), errorMessage)
        || !ensureColumn(QStringLiteral("seq"), QStringLiteral("INTEGER"), errorMessage)
        || !ensureColumn(QStringLiteral("device_name"), QStringLiteral("TEXT"), errorMessage)
        || !ensureColumn(QStringLiteral("synced"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"), errorMessage,
                         &addedSynced)) {
        return false;
    }
    if (addedSynced) {
        // Tudo o que foi guardado antes da nuvem existir - inclusive as
        // leituras a cada 2 s de firmwares antigos, que tinham boot/seq mas
        // não são registros - fica só local: só os registros novos sobem.
        QSqlQuery markExisting(m_database);
        if (!markExisting.exec(QStringLiteral("UPDATE sensor_readings SET synced = 1"))) {
            setSqlError(QStringLiteral("Failed to update SQLite schema"), markExisting.lastError(), errorMessage);
            return false;
        }
    }

    QSqlQuery syncStateQuery(m_database);
    if (!syncStateQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sync_state (key TEXT PRIMARY KEY, value TEXT)"))) {
        setSqlError(QStringLiteral("Failed to update SQLite schema"), syncStateQuery.lastError(), errorMessage);
        return false;
    }

    // Uma linha por registro, mesmo chegando por Bluetooth e pelo Supabase.
    QSqlQuery uniqueIndexQuery(m_database);
    if (!uniqueIndexQuery.exec(QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS idx_sensor_readings_unique_reading
            ON sensor_readings (device_id, boot, seq)
    )SQL"))) {
        setSqlError(QStringLiteral("Failed to update SQLite schema"), uniqueIndexQuery.lastError(), errorMessage);
        return false;
    }

    QSqlQuery indexQuery(m_database);
    if (!indexQuery.exec(QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS idx_sensor_readings_recorded_at
            ON sensor_readings (recorded_at)
    )SQL"))) {
        setSqlError(QStringLiteral("Failed to update SQLite schema"), indexQuery.lastError(), errorMessage);
        return false;
    }

    QSqlQuery deviceIndexQuery(m_database);
    if (!deviceIndexQuery.exec(QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS idx_sensor_readings_device
            ON sensor_readings (device_id, id)
    )SQL"))) {
        setSqlError(QStringLiteral("Failed to update SQLite schema"), deviceIndexQuery.lastError(), errorMessage);
        return false;
    }

    // Estatísticas e gráfico filtram uma placa num período.
    QSqlQuery deviceTimeIndexQuery(m_database);
    if (!deviceTimeIndexQuery.exec(QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS idx_sensor_readings_device_time
            ON sensor_readings (device_id, recorded_at)
    )SQL"))) {
        setSqlError(QStringLiteral("Failed to update SQLite schema"), deviceTimeIndexQuery.lastError(), errorMessage);
        return false;
    }

    // Temperatura externa por hora cheia (ver OutsideWeather).
    QSqlQuery outsideQuery(m_database);
    if (!outsideQuery.exec(QStringLiteral(R"SQL(
        CREATE TABLE IF NOT EXISTS outside_temperature (
            hour INTEGER PRIMARY KEY,
            temp REAL NOT NULL
        )
    )SQL"))) {
        setSqlError(QStringLiteral("Failed to update SQLite schema"), outsideQuery.lastError(), errorMessage);
        return false;
    }

    return true;
}

bool SensorReadingStore::ensureColumn(const QString &name, const QString &definition, QString *errorMessage,
                                      bool *added)
{
    if (added) {
        *added = false;
    }

    QSqlQuery columnInfo(m_database);
    if (!columnInfo.exec(QStringLiteral("PRAGMA table_info(sensor_readings)"))) {
        setSqlError(QStringLiteral("Failed to inspect sensor_readings columns"), columnInfo.lastError(), errorMessage);
        return false;
    }

    bool exists = false;
    while (columnInfo.next()) {
        if (columnInfo.value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0) {
            exists = true;
        }
    }
    columnInfo.finish(); // libera a trava antes do ALTER TABLE

    if (exists) {
        return true;
    }

    QSqlQuery addColumn(m_database);
    if (!addColumn.exec(QStringLiteral("ALTER TABLE sensor_readings ADD COLUMN %1 %2").arg(name, definition))) {
        setSqlError(QStringLiteral("Failed to add %1 column").arg(name), addColumn.lastError(), errorMessage);
        return false;
    }
    if (added) {
        *added = true;
    }
    return true;
}

void SensorReadingStore::setSqlError(const QString &context, const QSqlError &error, QString *errorMessage) const
{
    if (errorMessage) {
        *errorMessage = QStringLiteral("%1: %2").arg(context, error.text());
    }
}