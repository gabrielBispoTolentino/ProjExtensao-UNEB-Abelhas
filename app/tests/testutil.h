#pragma once

// Ajudas comuns aos testes: leituras de exemplo nos formatos que a placa
// envia de fato (iguais a sendRecordBLE() e ao valor ao vivo do loop() no
// firmware) e um banco local numa pasta temporária.

#include "sensorpayload.h"
#include "sensorreadingstore.h"

#include <QDateTime>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

namespace testutil {

inline const QString deviceId = QStringLiteral("3076F590C1D0");

// Uma linha de registro como o firmware manda por Bluetooth.
inline QByteArray recordLine(qint64 boot, qint64 seq, double temp, double hum, qint64 ts,
                             const QString &device = deviceId, const QString &name = QStringLiteral("Colmeia Teste"))
{
    QByteArray line = QStringLiteral(R"({"deviceId":"%1","name":"%2","tempDHT":%3,"umidade":%4,"boot":%5,"seq":%6,"rec":true)")
                          .arg(device, name).arg(temp).arg(hum).arg(boot).arg(seq).toUtf8();
    if (ts > 0) {
        line += ",\"ts\":" + QByteArray::number(ts);
    }
    return line + "}";
}

// Registro do despejo do SD: igual, com "sd": true (sendRecordBLE(r, false)).
inline QByteArray sdLine(qint64 boot, qint64 seq, double temp, double hum, qint64 ts)
{
    QByteArray line = recordLine(boot, seq, temp, hum, ts);
    line.chop(1);
    return line + ",\"sd\":true}";
}

// Valor ao vivo (a cada 2 s): sem boot/seq/rec.
inline QByteArray liveLine(double temp, double hum, qint64 ts)
{
    return QStringLiteral(R"({"deviceId":"%1","name":"Colmeia Teste","tempDHT":%2,"umidade":%3,"ts":%4})")
        .arg(deviceId).arg(temp).arg(hum).arg(ts).toUtf8();
}

inline SensorPayload record(qint64 boot, qint64 seq, double temp, double hum, qint64 ts,
                            const QString &device = deviceId)
{
    SensorPayload payload;
    payload.deviceId = device;
    payload.name = QStringLiteral("Colmeia Teste");
    payload.boot = boot;
    payload.seq = seq;
    payload.tempDht = temp;
    payload.humidity = hum;
    payload.timestamp = ts;
    payload.recordFlag = true;
    return payload;
}

inline qint64 secsAgo(qint64 secs)
{
    return QDateTime::currentSecsSinceEpoch() - secs;
}

// Banco novo numa pasta temporária (apagada no fim do teste).
struct TempStore
{
    QTemporaryDir dir;
    SensorReadingStore store;

    TempStore()
    {
        QString error;
        if (!store.open(path(), &error)) {
            qFatal("could not open test database: %s", qPrintable(error));
        }
    }
    QString path() const { return dir.filePath(QStringLiteral("sensor_readings.db")); }

    int pending()
    {
        int count = 0;
        QString error;
        store.pendingUploadCount(&count, &error);
        return count;
    }
    int rejected()
    {
        int count = 0;
        QString error;
        store.rejectedCount(&count, &error);
        return count;
    }
    int total()
    {
        int count = 0, pendingCount = 0;
        QString error;
        store.countReadings(QString(), QDateTime(), &count, &pendingCount, &error);
        return count;
    }
};

} // namespace testutil
