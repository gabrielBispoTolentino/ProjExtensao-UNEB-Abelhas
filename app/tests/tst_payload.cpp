// Leitura das linhas JSON que chegam da placa por Bluetooth (SensorPayload):
// os formatos exatos do firmware e o que deve ser recusado.

#include "testutil.h"

#include <QTest>

using namespace testutil;

class TestPayload : public QObject
{
    Q_OBJECT

private slots:
    void recordFromFirmware()
    {
        // Igual a sendRecordBLE() no firmware.
        SensorPayload p;
        QString error;
        QVERIFY2(SensorPayload::fromJson(recordLine(20, 7, 32.51, 50.88, 1792590000), &p, &error), qPrintable(error));
        QCOMPARE(p.deviceId, deviceId);
        QCOMPARE(p.name, QStringLiteral("Colmeia Teste"));
        QCOMPARE(p.tempDht, 32.51);
        QCOMPARE(p.humidity, 50.88);
        QCOMPARE(p.boot, 20);
        QCOMPARE(p.seq, 7);
        QCOMPARE(p.timestamp, 1792590000);
        QVERIFY(p.recordFlag);
        QVERIFY(p.hasRecordId());
    }

    void recordWithoutClock()
    {
        // Placa ainda sem hora: o firmware omite "ts".
        SensorPayload p;
        QVERIFY(SensorPayload::fromJson(recordLine(20, 1, 32.5, 50.7, 0), &p, nullptr));
        QCOMPARE(p.timestamp, 0);
        QVERIFY(p.recordFlag);
    }

    void liveValueIsNotARecord()
    {
        SensorPayload p;
        QVERIFY(SensorPayload::fromJson(liveLine(32.5, 50.7, 1792590000), &p, nullptr));
        QVERIFY(!p.recordFlag);
        QVERIFY(!p.hasRecordId());
    }

    void oldFirmwareTwoSecondReadings()
    {
        // Firmwares antigos mandavam boot/seq a cada 2 s, sem "rec": não são registros.
        SensorPayload p;
        QVERIFY(SensorPayload::fromJson(R"({"deviceId":"X","tempDHT":30,"umidade":60,"boot":3,"seq":9})", &p, nullptr));
        QVERIFY(p.hasRecordId());
        QVERIFY(!p.recordFlag);
    }

    void integerValues()
    {
        SensorPayload p;
        QVERIFY(SensorPayload::fromJson(R"({"deviceId":"X","tempDHT":30,"umidade":60})", &p, nullptr));
        QCOMPARE(p.tempDht, 30.0);
    }

    void refusesBrokenLines_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::newRow("not json") << QByteArray("{\"deviceId\":\"X\",\"tempDHT\":3");
        QTest::newRow("merged lines") << QByteArray(R"({"tempDHT":1,"umidade":2}{"tempDHT":3,"umidade":4})");
        QTest::newRow("array") << QByteArray("[1,2]");
        QTest::newRow("no temperature") << QByteArray(R"({"deviceId":"X","umidade":60})");
        QTest::newRow("no humidity") << QByteArray(R"({"deviceId":"X","tempDHT":30})");
        QTest::newRow("temperature as text") << QByteArray(R"({"deviceId":"X","tempDHT":"30","umidade":60})");
        QTest::newRow("NaN from a failed DHT read") << QByteArray(R"({"deviceId":"X","tempDHT":nan,"umidade":60})");
        QTest::newRow("empty") << QByteArray();
    }
    void refusesBrokenLines()
    {
        QFETCH(QByteArray, line);
        SensorPayload p;
        QString error;
        QVERIFY(!SensorPayload::fromJson(line, &p, &error));
        QVERIFY(!error.isEmpty());
    }

    void controlMessagesAreNotReadings()
    {
        // As respostas com "type" (ex.: fim de lote do despejo) não têm
        // tempDHT: se caíssem aqui seriam recusadas, não viram leituras.
        SensorPayload p;
        QVERIFY(!SensorPayload::fromJson(R"({"type":"dump","pos":1234,"count":25,"left":0})", &p, nullptr));
    }
};

QTEST_GUILESS_MAIN(TestPayload)
#include "tst_payload.moc"
