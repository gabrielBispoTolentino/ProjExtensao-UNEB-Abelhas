// Leitura do JSON enviado pelas placas (ver SensorPayload).

#include "sensorpayload.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace {

bool readNumber(const QJsonObject &object, const QString &key, double *value, QString *errorMessage)
{
    const QJsonValue jsonValue = object.value(key);
    if (!jsonValue.isDouble()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Expected numeric field '%1'").arg(key);
        }
        return false;
    }

    *value = jsonValue.toDouble();
    return true;
}

} // namespace

bool SensorPayload::fromJson(const QByteArray &json, SensorPayload *payload, QString *errorMessage)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorMessage) {
            *errorMessage = parseError.errorString();
        }
        return false;
    }

    if (!document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Payload root must be a JSON object");
        }
        return false;
    }

    // tempDHT e umidade são obrigatórios; campos desconhecidos são ignorados.
    // deviceId é opcional aqui: o SensorMonitor usa "unknown" se faltar.
    SensorPayload parsed;
    const QJsonObject object = document.object();
    if (!readNumber(object, QStringLiteral("tempDHT"), &parsed.tempDht, errorMessage)
        || !readNumber(object, QStringLiteral("umidade"), &parsed.humidity, errorMessage))
    {
        return false;
    }

    parsed.deviceId = object.value(QStringLiteral("deviceId")).toString();

    // ts é opcional: placas ainda sem relógio acertado (e firmwares antigos) não enviam.
    const QJsonValue timestamp = object.value(QStringLiteral("ts"));
    if (timestamp.isDouble() && timestamp.toDouble() > 0) {
        parsed.timestamp = static_cast<qint64>(timestamp.toDouble());
    }

    parsed.name = object.value(QStringLiteral("name")).toString();
    parsed.boot = static_cast<qint64>(object.value(QStringLiteral("boot")).toDouble());
    parsed.seq  = static_cast<qint64>(object.value(QStringLiteral("seq")).toDouble());
    parsed.recordFlag = object.value(QStringLiteral("rec")).toBool();

    if (payload) {
        *payload = parsed;
    }
    return true;
}