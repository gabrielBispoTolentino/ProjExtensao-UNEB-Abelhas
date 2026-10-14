#pragma once

#include <QByteArray>
#include <QString>

// Uma leitura recebida de uma placa (linha JSON por Bluetooth ou linha do
// Supabase já convertida).
struct SensorPayload
{
    double tempDht = 0.0;
    double humidity = 0.0;
    QString deviceId;
    qint64 timestamp = 0;   // quando a placa mediu (segundos Unix, UTC); 0 = desconhecido
    QString name;           // nome amigável da placa, se ela enviou
    // boot + seq identificam um *registro* (a média da placa no intervalo de
    // registro); o mesmo registro pode chegar pelo Supabase e pelo Bluetooth.
    qint64 boot = 0;
    qint64 seq = 0;
    // Por Bluetooth, um registro também traz "rec": true. Valores ao vivo
    // (a cada 2 s, só para a tela) não trazem - nem as leituras a cada 2 s
    // de firmwares antigos, que tinham boot/seq mas não são registros.
    bool recordFlag = false;

    bool hasRecordId() const { return boot > 0 && seq > 0; }
    static bool fromJson(const QByteArray &json, SensorPayload *payload, QString *errorMessage);
};
