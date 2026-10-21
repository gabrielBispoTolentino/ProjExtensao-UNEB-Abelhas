// O caminho do Bluetooth no SensorMonitor: o que chega da placa é guardado
// (registros) ou só mostrado (valores ao vivo), e cada lote do despejo do SD
// só é confirmado à placa depois de guardado. A confirmação é observada pela
// contagem de "registros guardados recebidos" na notificação do Android
// (backgroundStatusText), que só sobe quando o lote é confirmado.

#include "sensormonitor.h"
#include "testutil.h"

#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>

using namespace testutil;

namespace {

const QString key = QStringLiteral("AA:BB:CC:DD:EE:FF");

void bleLine(SensorMonitor &monitor, const QByteArray &line)
{
    QVERIFY(QMetaObject::invokeMethod(&monitor, "handleBleMessage", Q_ARG(QString, key), Q_ARG(QByteArray, line)));
}

void bleControl(SensorMonitor &monitor, const QJsonObject &message)
{
    QVERIFY(QMetaObject::invokeMethod(&monitor, "handleBleControl", Q_ARG(QString, key), Q_ARG(QJsonObject, message)));
}

QJsonObject batchEnd(int pos, int count)
{
    return {{"type", "dump"}, {"pos", pos}, {"count", count}, {"left", 0}};
}

int stored(SensorMonitor &monitor)
{
    return monitor.localDataCount(QString(), 0).value("count").toInt();
}

int pendingStored(SensorMonitor &monitor)
{
    return monitor.localDataCount(QString(), 0).value("pending").toInt();
}

} // namespace

class TestMonitor : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void liveValueUpdatesCardOnly()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("live.db"));
        bleLine(monitor, liveLine(32.5, 50.7, secsAgo(1)));
        QCOMPARE(stored(monitor), 0);
        const QVariantList devices = monitor.devices();
        QCOMPARE(devices.size(), 1);
        QCOMPARE(devices[0].toMap().value("tempDht").toDouble(), 32.5);
        QCOMPARE(devices[0].toMap().value("source").toString(), QStringLiteral("BLE"));
    }

    void recordIsStoredForUpload()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("record.db"));
        bleLine(monitor, recordLine(20, 1, 32.5, 50.7, secsAgo(60)));
        QCOMPARE(stored(monitor), 1);
        QCOMPARE(pendingStored(monitor), 1);
        bleLine(monitor, recordLine(20, 1, 32.5, 50.7, secsAgo(60)));   // repetido
        QCOMPARE(stored(monitor), 1);
    }

    void brokenLineIsIgnored()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("broken.db"));
        bleLine(monitor, "{\"deviceId\":\"X\",\"tempD");
        QCOMPARE(stored(monitor), 0);
        QVERIFY(monitor.devices().isEmpty());
    }

    void olderRecordDoesNotReplaceNewerCard()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("order.db"));
        bleLine(monitor, recordLine(20, 5, 33.0, 50, secsAgo(60)));
        bleLine(monitor, recordLine(20, 1, 29.0, 50, secsAgo(3600)));   // do despejo, mais antigo
        QCOMPARE(stored(monitor), 2);
        QCOMPARE(monitor.devices()[0].toMap().value("tempDht").toDouble(), 33.0);
    }

    void dumpBatchIsAcknowledgedAfterStoring()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("dump.db"));
        for (int seq = 1; seq <= 3; ++seq) {
            bleLine(monitor, sdLine(20, seq, 32, 50, secsAgo(600 - seq)));
        }
        bleControl(monitor, batchEnd(300, 3));
        QCOMPARE(stored(monitor), 3);
        QVERIFY2(monitor.backgroundStatusText().contains("registros guardados na placa (3)"),
                 qPrintable(monitor.backgroundStatusText()));

        for (int seq = 4; seq <= 5; ++seq) {
            bleLine(monitor, sdLine(20, seq, 32, 50, secsAgo(600 - seq)));
        }
        bleControl(monitor, batchEnd(500, 2));
        QVERIFY(monitor.backgroundStatusText().contains("(5)"));

        bleControl(monitor, {{"type", "dump"}, {"done", true}});
        QVERIFY2(!monitor.backgroundStatusText().contains("guardados na placa"), qPrintable(monitor.backgroundStatusText()));
        QCOMPARE(stored(monitor), 5);
        QCOMPARE(pendingStored(monitor), 5);
    }

    void resentBatchDoesNotDuplicate()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("resend.db"));
        for (int round = 0; round < 2; ++round) {
            for (int seq = 1; seq <= 3; ++seq) {
                bleLine(monitor, sdLine(20, seq, 32, 50, secsAgo(600 - seq)));
            }
            bleControl(monitor, batchEnd(300, 3));
        }
        QCOMPARE(stored(monitor), 3);
    }

    void batchIsNotAcknowledgedWhenStoringFails()
    {
        SensorMonitor monitor;
        const QString path = dir.filePath("fail.db");
        monitor.start(path);
        // Banco quebrado por fora (ex.: disco cheio): os INSERTs falham.
        {
            QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("breaker"));
            other.setDatabaseName(path);
            QVERIFY(other.open());
            QVERIFY(QSqlQuery(other).exec(QStringLiteral("DROP TABLE sensor_readings")));
            other.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("breaker"));

        bleLine(monitor, sdLine(20, 1, 32, 50, secsAgo(60)));
        bleControl(monitor, batchEnd(100, 1));
        QVERIFY2(!monitor.backgroundStatusText().contains("guardados na placa"),
                 "a batch that was not stored must not be acknowledged");
    }

    void wifiAnswerIsNotABatch()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("wifi.db"));
        bleControl(monitor, {{"type", "dump"}, {"state", "wifi"}});
        QVERIFY(!monitor.backgroundStatusText().contains("guardados na placa"));
    }

    void batchWithAMissingRecord()
    {
        // Uma linha se perdeu no caminho (pedaço corrompido): o lote diz
        // count = 3 mas só chegaram 2. Sem confirmação, a placa reenvia.
        SensorMonitor monitor;
        monitor.start(dir.filePath("missing.db"));
        bleLine(monitor, sdLine(20, 1, 32, 50, secsAgo(300)));
        bleLine(monitor, "{\"deviceId\":\"3076F590C1D0\",\"tem");   // registro 2 corrompido
        bleLine(monitor, sdLine(20, 3, 32, 50, secsAgo(100)));
        bleControl(monitor, batchEnd(300, 3));
        QCOMPARE(stored(monitor), 2);
        QVERIFY2(!monitor.backgroundStatusText().contains("guardados na placa"), "incomplete batch acknowledged");

        // O reenvio chega completo: confirmado.
        for (int seq = 1; seq <= 3; ++seq) {
            bleLine(monitor, sdLine(20, seq, 32, 50, secsAgo(400 - 100 * seq)));
        }
        bleControl(monitor, batchEnd(300, 3));
        QCOMPARE(stored(monitor), 3);
        QVERIFY(monitor.backgroundStatusText().contains("guardados na placa (3)"));
    }

    void liveRecordDoesNotHideAMissingOne()
    {
        // Um registro novo (ao vivo) chega no meio do lote: não pode contar
        // no lugar do registro do SD que se perdeu.
        SensorMonitor monitor;
        monitor.start(dir.filePath("mixed.db"));
        bleLine(monitor, sdLine(20, 1, 32, 50, secsAgo(300)));
        bleLine(monitor, recordLine(20, 9, 32, 50, secsAgo(1)));        // ao vivo, sem "sd"
        bleLine(monitor, sdLine(20, 3, 32, 50, secsAgo(100)));          // o 2 se perdeu
        bleControl(monitor, batchEnd(300, 3));
        QVERIFY2(!monitor.backgroundStatusText().contains("guardados na placa"), "live record counted as a dump record");
    }

    void oldFirmwareWithoutSdFlag()
    {
        // Firmware 1.4.0 e anteriores não marcam os registros do despejo:
        // contam todos os registros do lote.
        SensorMonitor monitor;
        monitor.start(dir.filePath("oldfw.db"));
        for (int seq = 1; seq <= 3; ++seq) {
            bleLine(monitor, recordLine(20, seq, 32, 50, secsAgo(600 - seq)));
        }
        bleControl(monitor, batchEnd(300, 3));
        QVERIFY(monitor.backgroundStatusText().contains("guardados na placa (3)"));
    }

    void statusTextForTheNotification()
    {
        SensorMonitor monitor;
        monitor.start(dir.filePath("status.db"));
        // Supabase dos testes não está rodando: tudo "para enviar".
        bleLine(monitor, recordLine(20, 1, 32, 50, secsAgo(60)));
        bleLine(monitor, recordLine(20, 2, 32, 50, secsAgo(30)));
        QTRY_VERIFY_WITH_TIMEOUT(monitor.backgroundStatusText().contains("2 registros"), 5000);
    }
};

QTEST_MAIN(TestMonitor)
#include "tst_monitor.moc"
