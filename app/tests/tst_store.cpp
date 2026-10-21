// Banco local (SensorReadingStore): o que fica pendente de envio, repetidos,
// estatísticas, limpeza, exportação CSV e temperatura externa.

#include "testutil.h"

#include <QBuffer>
#include <QTest>
#include <QTimeZone>

using namespace testutil;

class TestStore : public QObject
{
    Q_OBJECT

private slots:
    // ── Gravação e envio ───────────────────────────────────────────────────
    void bluetoothRecordIsPending()
    {
        TempStore db;
        bool inserted = false;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 32.5, 50.7, secsAgo(60)), false, &inserted, &error));
        QVERIFY(inserted);
        QCOMPARE(db.pending(), 1);

        QList<SensorReadingStore::PendingUpload> uploads;
        QVERIFY(db.store.pendingUploads(10, &uploads, &error));
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads[0].boot, 20);
        QCOMPARE(uploads[0].seq, 1);
        QCOMPARE(uploads[0].temperature, 32.5);
        QCOMPARE(uploads[0].humidity.toDouble(), 50.7);
        QVERIFY(uploads[0].recordedAt.endsWith(QLatin1Char('Z')));
    }

    void sameRecordTwiceIsStoredOnce()
    {
        // O despejo reenvia um lote sem confirmação; o registro ao vivo
        // também chega pelo SD depois: tudo isso não pode duplicar.
        TempStore db;
        bool inserted = true;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 32.5, 50.7, secsAgo(60)), false, &inserted, &error));
        QVERIFY(db.store.insertReading(record(20, 1, 32.5, 50.7, secsAgo(60)), false, &inserted, &error));
        QVERIFY(!inserted);
        QCOMPARE(db.total(), 1);
        QCOMPARE(db.pending(), 1);
    }

    void differentBootsAndBoardsDoNotCollide()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(60)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(21, 1, 30, 50, secsAgo(60)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(60), QStringLiteral("AABBCCDDEEFF")), false, nullptr, &error));
        QCOMPARE(db.total(), 3);
    }

    void cloudCopyMarksBluetoothRecordSynced()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 32.5, 50.7, secsAgo(60)), false, nullptr, &error));
        QCOMPARE(db.pending(), 1);
        QVERIFY(db.store.insertCloudReadings({record(20, 1, 32.5, 50.7, secsAgo(60))}, &error));
        QCOMPARE(db.pending(), 0);
        QCOMPARE(db.total(), 1);
    }

    void cloudReadingsAreNeverUploaded()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertCloudReadings({record(9, 1, 30, 50, secsAgo(120)), record(9, 2, 31, 51, secsAgo(60))}, &error));
        QCOMPARE(db.total(), 2);
        QCOMPARE(db.pending(), 0);
    }

    void unidentifiedReadingIsKeptButNotUploaded()
    {
        // Firmware antigo, sem boot/seq: o Supabase exige os dois.
        TempStore db;
        QString error;
        SensorPayload old = record(0, 0, 30, 50, secsAgo(60));
        QVERIFY(db.store.insertReading(old, false, nullptr, &error));
        QVERIFY(db.store.insertReading(old, false, nullptr, &error));   // sem id, não colide
        QCOMPARE(db.total(), 2);
        QCOMPARE(db.pending(), 0);
    }

    void markSyncedAndRejected()
    {
        TempStore db;
        QString error;
        for (int seq = 1; seq <= 3; ++seq) {
            QVERIFY(db.store.insertReading(record(20, seq, 30, 50, secsAgo(60 * seq)), false, nullptr, &error));
        }
        QList<SensorReadingStore::PendingUpload> uploads;
        QVERIFY(db.store.pendingUploads(10, &uploads, &error));
        QCOMPARE(uploads.size(), 3);
        QVERIFY(db.store.markSynced({uploads[0].localId, uploads[1].localId}, &error));
        QVERIFY(db.store.markRejected(uploads[2].localId, &error));
        QCOMPARE(db.pending(), 0);
        QCOMPARE(db.rejected(), 1);
    }

    void pendingUploadsRespectsLimitAndOrder()
    {
        TempStore db;
        QString error;
        for (int seq = 1; seq <= 5; ++seq) {
            QVERIFY(db.store.insertReading(record(20, seq, 30, 50, secsAgo(600 - seq)), false, nullptr, &error));
        }
        QList<SensorReadingStore::PendingUpload> uploads;
        QVERIFY(db.store.pendingUploads(2, &uploads, &error));
        QCOMPARE(uploads.size(), 2);
        QCOMPARE(uploads[0].seq, 1);
        QCOMPARE(uploads[1].seq, 2);
    }

    void missingTimestampUsesArrivalTime()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, 0), false, nullptr, &error));
        StoredSensorReading latest;
        bool found = false;
        QVERIFY(db.store.latestReading(deviceId, &latest, &found, &error));
        QVERIFY(found);
        const QDateTime at = QDateTime::fromString(latest.recordedAt, Qt::ISODateWithMs);
        QVERIFY(qAbs(at.secsTo(QDateTime::currentDateTimeUtc())) < 10);
    }

    void dataSurvivesReopening()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sensor_readings.db"));
        QString error;
        {
            SensorReadingStore store;
            QVERIFY(store.open(path, &error));
            QVERIFY(store.insertReading(record(20, 1, 30, 50, secsAgo(60)), false, nullptr, &error));
            QVERIFY(store.setSyncValue(QStringLiteral("supabase_last_id"), QStringLiteral("42"), &error));
        }
        SensorReadingStore store;
        QVERIFY(store.open(path, &error));
        int pending = 0;
        QVERIFY(store.pendingUploadCount(&pending, &error));
        QCOMPARE(pending, 1);
        QCOMPARE(store.syncValue(QStringLiteral("supabase_last_id")), QStringLiteral("42"));
    }

    // ── Estatísticas ───────────────────────────────────────────────────────
    void statsOfAPeriod()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30.0, 40.0, secsAgo(3 * 3600)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 2, 34.0, 50.0, secsAgo(2 * 3600)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 3, 32.0, 60.0, secsAgo(1 * 3600)), false, nullptr, &error));

        QVariantMap stats;
        QVERIFY(db.store.readingStats(deviceId, QDateTime(), &stats, &error));
        QCOMPARE(stats.value("count").toInt(), 3);
        const QVariantMap t = stats.value("temperature").toMap();
        QCOMPARE(t.value("average").toDouble(), 32.0);
        QCOMPARE(t.value("min").toDouble(), 30.0);
        QCOMPARE(t.value("max").toDouble(), 34.0);
        QCOMPARE(t.value("current").toDouble(), 32.0);
        QCOMPARE(t.value("change").toDouble(), 2.0);
        QVERIFY(qAbs(t.value("stdDev").toDouble() - 1.63299) < 1e-4);
        const QVariantMap h = stats.value("humidity").toMap();
        QCOMPARE(h.value("average").toDouble(), 50.0);
        QVERIFY(stats.contains("correlation"));
    }

    void allTimeRangeIncludesEverything()
    {
        // Regressão da v1.0.0: "Tudo" (sem limite inferior) não mostrava nada.
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(400 * 86400)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 2, 31, 51, secsAgo(60)), false, nullptr, &error));
        QVariantMap stats;
        QVERIFY(db.store.readingStats(deviceId, QDateTime(), &stats, &error));
        QCOMPARE(stats.value("count").toInt(), 2);
        QVariantMap series;
        QVERIFY(db.store.readingSeries(deviceId, QDateTime(), 300, &series, &error));
        QCOMPARE(series.value("points").toList().size(), 2);
        int rows = 0;
        QVERIFY(db.store.countExportRows(QString(), QDateTime(), &rows, &error));
        QCOMPARE(rows, 2);
    }

    void periodFilterExcludesOlderReadings()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(2 * 86400)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 2, 31, 51, secsAgo(3600)), false, nullptr, &error));
        QVariantMap stats;
        QVERIFY(db.store.readingStats(deviceId, QDateTime::currentDateTimeUtc().addSecs(-86400), &stats, &error));
        QCOMPARE(stats.value("count").toInt(), 1);
    }

    void temperatureRangeOfLastDay()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 28.9, 50, secsAgo(5 * 3600)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 2, 33.1, 50, secsAgo(3600)), false, nullptr, &error));
        double min = 0, max = 0;
        int count = 0;
        QVERIFY(db.store.temperatureRange(deviceId, QDateTime::currentDateTimeUtc().addSecs(-86400), &min, &max, &count, &error));
        QCOMPARE(count, 2);
        QCOMPARE(min, 28.9);
        QCOMPARE(max, 33.1);
    }

    void temperatureRangeWithoutLowerLimit()
    {
        // Mesmo problema da v1.0.0, que ficou neste lugar: "sem limite" tem
        // de virar '' (sinceFilter), não NULL (nada é >= NULL).
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(3600)), false, nullptr, &error));
        double min = 0, max = 0;
        int count = 0;
        QVERIFY(db.store.temperatureRange(deviceId, QDateTime(), &min, &max, &count, &error));
        QCOMPARE(count, 1);
    }

    void seriesBucketsAveragePoints()
    {
        TempStore db;
        QString error;
        for (int i = 0; i < 100; ++i) {
            QVERIFY(db.store.insertReading(record(20, i + 1, 30 + i % 2, 50, secsAgo(6000 - 60 * i)), false, nullptr, &error));
        }
        QVariantMap series;
        QVERIFY(db.store.readingSeries(deviceId, QDateTime(), 10, &series, &error));
        const QVariantList points = series.value("points").toList();
        QVERIFY(points.size() <= 11);
        QVERIFY(points.size() >= 9);
        for (const QVariant &point : points) {
            const double temp = point.toMap().value("temp").toDouble();
            QVERIFY(temp >= 30.0 && temp <= 31.0);
        }
    }

    void unknownHumidityStaysUnknown()
    {
        // Umidade NULL (do Supabase) é guardada como NULL, não como 0: não
        // entra nas médias, sobe como null e sai vazia na planilha.
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 60, secsAgo(120)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 2, 32, qQNaN(), secsAgo(60)), false, nullptr, &error));

        QVariantMap stats;
        QVERIFY(db.store.readingStats(deviceId, QDateTime(), &stats, &error));
        QCOMPARE(stats.value("humidity").toMap().value("count").toInt(), 1);
        QCOMPARE(stats.value("humidity").toMap().value("average").toDouble(), 60.0);

        StoredSensorReading latest;
        bool found = false;
        QVERIFY(db.store.latestReading(deviceId, &latest, &found, &error));
        QVERIFY(qIsNaN(latest.humidity));

        QList<SensorReadingStore::PendingUpload> uploads;
        QVERIFY(db.store.pendingUploads(10, &uploads, &error));
        QVERIFY(uploads[1].humidity.isNull());

        QBuffer out;
        out.open(QIODevice::WriteOnly);
        int count = 0;
        QVERIFY(db.store.exportCsv(&out, QString(), QDateTime(), &count, &error));
        const QList<QByteArray> lines = out.data().mid(3).split('\n');
        QCOMPARE(lines[2].trimmed().split(';').value(5), QByteArray());
    }

    // ── Temperatura externa ────────────────────────────────────────────────
    void outsideTemperatureJoinsNearestHour()
    {
        TempStore db;
        QString error;
        // Horas cheias: 10:00 = 20 °C, 11:00 = 24 °C (UTC, um dia qualquer).
        const qint64 ten = QDateTime(QDate(2026, 10, 20), QTime(10, 0), QTimeZone::UTC).toSecsSinceEpoch();
        QVERIFY(db.store.saveOutsideTemperatures({{ten, 20.0}, {ten + 3600, 24.0}}, &error));
        // 10:29 → hora das 10; 10:31 → hora das 11.
        QVERIFY(db.store.insertReading(record(20, 1, 30.0, 50, ten + 29 * 60), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 2, 30.0, 50, ten + 31 * 60), false, nullptr, &error));
        QVariantMap stats;
        QVERIFY(db.store.readingStats(deviceId, QDateTime(), &stats, &error));
        const QVariantMap t = stats.value("temperature").toMap();
        QCOMPARE(t.value("outsideAverage").toDouble(), 22.0);
        QCOMPARE(t.value("outsideMin").toDouble(), 20.0);
        QCOMPARE(t.value("outsideMax").toDouble(), 24.0);
        QCOMPARE(t.value("outsideDifference").toDouble(), 8.0);
    }

    // ── Limpeza ────────────────────────────────────────────────────────────
    void deleteOlderThanForOneBoard()
    {
        TempStore db;
        QString error;
        const QString other = QStringLiteral("AABBCCDDEEFF");
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(10 * 86400)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(20, 2, 30, 50, secsAgo(3600)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(5, 1, 30, 50, secsAgo(10 * 86400), other), false, nullptr, &error));

        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-7);
        int count = 0, pending = 0;
        QVERIFY(db.store.countReadings(deviceId, cutoff, &count, &pending, &error));
        QCOMPARE(count, 1);
        QCOMPARE(pending, 1);   // a tela avisa: nunca foi enviado
        int deleted = 0;
        QVERIFY(db.store.deleteReadings(deviceId, cutoff, &deleted, &error));
        QCOMPARE(deleted, 1);
        QCOMPARE(db.total(), 2);   // a recente e a da outra placa ficam
    }

    void deleteEverything()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(60)), false, nullptr, &error));
        QVERIFY(db.store.insertReading(record(5, 1, 30, 50, secsAgo(60), QStringLiteral("AABBCCDDEEFF")), false, nullptr, &error));
        int deleted = 0;
        QVERIFY(db.store.deleteReadings(QString(), QDateTime(), &deleted, &error));
        QCOMPARE(deleted, 2);
        QCOMPARE(db.total(), 0);
    }

    // ── Exportação ─────────────────────────────────────────────────────────
    void csvForBrazilianExcel()
    {
        TempStore db;
        QString error;
        const qint64 at = QDateTime(QDate(2026, 10, 20), QTime(13, 5, 9), QTimeZone::UTC).toSecsSinceEpoch();
        QVERIFY(db.store.saveOutsideTemperatures({{at - 5 * 60 - 9, 21.5}}, &error));   // hora das 13:00
        QVERIFY(db.store.insertReading(record(20, 1, 32.5, 50.25, at), false, nullptr, &error));

        QBuffer out;
        out.open(QIODevice::WriteOnly);
        int count = 0;
        QVERIFY(db.store.exportCsv(&out, QString(), QDateTime(), &count, &error));
        QCOMPARE(count, 1);
        const QByteArray csv = out.data();
        QVERIFY2(csv.startsWith("\xEF\xBB\xBF"), "UTF-8 BOM so Excel reads accents");
        const QList<QByteArray> lines = csv.mid(3).split('\n');
        QVERIFY(lines[0].endsWith('\r'));
        QCOMPARE(lines[0].trimmed(), QByteArray("colmeia;id_placa;data_hora;data_hora_utc;temperatura_c;umidade_pct;temperatura_externa_c"));
        const QList<QByteArray> fields = lines[1].trimmed().split(';');
        QCOMPARE(fields.size(), 7);
        QCOMPARE(fields[0], QByteArray("Colmeia Teste"));
        QCOMPARE(fields[1], deviceId.toUtf8());
        QCOMPARE(fields[3], QByteArray("2026-10-20T13:05:09Z"));
        QCOMPARE(fields[4], QByteArray("32,50"));   // vírgula decimal
        QCOMPARE(fields[5], QByteArray("50,25"));
        QCOMPARE(fields[6], QByteArray("21,50"));
        const QDateTime local = QDateTime::fromSecsSinceEpoch(at).toLocalTime();
        QCOMPARE(fields[2], local.toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")).toUtf8());
    }

    void csvUsesCurrentBoardName()
    {
        TempStore db;
        QString error;
        SensorPayload first = record(20, 1, 30, 50, secsAgo(7200));
        first.name = QStringLiteral("Nome antigo");
        QVERIFY(db.store.insertReading(first, false, nullptr, &error));
        SensorPayload renamed = record(20, 2, 30, 50, secsAgo(60));
        renamed.name = QStringLiteral("Colmeia; nova \"A\"");
        QVERIFY(db.store.insertReading(renamed, false, nullptr, &error));

        QBuffer out;
        out.open(QIODevice::WriteOnly);
        int count = 0;
        QVERIFY(db.store.exportCsv(&out, QString(), QDateTime(), &count, &error));
        const QList<QByteArray> lines = out.data().mid(3).split('\n');
        // Nome com ";" e aspas precisa vir entre aspas, as duas linhas com o nome atual.
        QVERIFY2(lines[1].startsWith("\"Colmeia; nova \"\"A\"\"\";"), lines[1].constData());
        QVERIFY2(lines[2].startsWith("\"Colmeia; nova \"\"A\"\"\";"), lines[2].constData());
    }
};

QTEST_GUILESS_MAIN(TestStore)
#include "tst_store.moc"
