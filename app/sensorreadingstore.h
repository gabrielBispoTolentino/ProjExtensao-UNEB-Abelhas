#pragma once

#include "sensorpayload.h"

#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariant>
#include <QVariantMap>

// Uma linha da tabela local sensor_readings.
struct StoredSensorReading
{
    qint64 id = 0;
    QString recordedAt;
    double tempDht = 0.0;
    double humidity = 0.0;
    QString deviceId;
    QString deviceName;
};

// Banco SQLite local: espelho do Supabase + cópia dos registros recebidos
// por Bluetooth que ainda não subiram. Estatísticas e gráficos leem daqui.
class SensorReadingStore
{
public:
    SensorReadingStore();
    ~SensorReadingStore();

    bool open(const QString &path, QString *errorMessage);

    // ── Gravação ───────────────────────────────────────────────────────────
    // Guarda um registro. fromCloud: veio do Supabase, então já está
    // sincronizado; senão (Bluetooth) fica aguardando envio.
    // *inserted é false quando o registro já existia (mesmo
    // device/boot/seq); se a cópia veio da nuvem, ele passa a sincronizado.
    bool insertReading(const SensorPayload &reading, bool fromCloud, bool *inserted, QString *errorMessage);
    // O mesmo para um lote vindo da nuvem, numa única transação.
    bool insertCloudReadings(const QList<SensorPayload> &readings, QString *errorMessage);

    // Leitura mais recente de cada placa / de uma placa.
    bool latestReadings(QList<StoredSensorReading> *readings, QString *errorMessage);
    bool latestReading(const QString &deviceId, StoredSensorReading *reading, bool *found,
                       QString *errorMessage);

    // ── Envio ao Supabase ──────────────────────────────────────────────────
    // Registros recebidos por Bluetooth que o Supabase ainda não tem.
    struct PendingUpload {
        qint64  localId = 0;
        QString deviceId;
        QString deviceName;
        QString recordedAt;   // ISO, UTC
        double  temperature = 0.0;
        QVariant humidity;    // pode ser NULL
        qint64  boot = 0;
        qint64  seq = 0;
    };
    bool pendingUploads(int limit, QList<PendingUpload> *uploads, QString *errorMessage);
    bool pendingUploadCount(int *count, QString *errorMessage);
    bool markSynced(const QList<qint64> &localIds, QString *errorMessage);
    // O Supabase recusou este registro (ex.: dado inválido): não tentar de novo.
    bool markRejected(qint64 localId, QString *errorMessage);
    bool rejectedCount(int *count, QString *errorMessage);

    // ── Limpeza ────────────────────────────────────────────────────────────
    // Leituras de uma placa (todas, com deviceId vazio) gravadas antes de
    // `before` (qualquer data, se inválido). *pending: quantas delas nunca
    // foram enviadas ao Supabase.
    bool countReadings(const QString &deviceId, const QDateTime &before, int *count, int *pending,
                       QString *errorMessage);
    // Apaga essas leituras e compacta o arquivo para liberar o espaço.
    bool deleteReadings(const QString &deviceId, const QDateTime &before, int *deleted,
                        QString *errorMessage);

    // Tabelinha chave/valor do controle de sincronização (ex.: último id do Supabase).
    QString syncValue(const QString &key) const;
    bool setSyncValue(const QString &key, const QString &value, QString *errorMessage);

    // ── Consultas para a tela ──────────────────────────────────────────────
    // Resumo das leituras de uma placa desde `since` (todas, se inválido):
    // quantidade, período e, para temperatura e umidade, atual, média,
    // mín./máx. (+ quando), desvio padrão, variação e a correlação entre elas.
    bool readingStats(const QString &deviceId, const QDateTime &since,
                      QVariantMap *stats, QString *errorMessage);
    // Menor/maior temperatura de uma placa desde `since` (*count 0 = nenhuma).
    bool temperatureRange(const QString &deviceId, const QDateTime &since,
                          double *min, double *max, int *count, QString *errorMessage);
    // Leituras agrupadas em até maxPoints intervalos de tempo, da mais antiga
    // para a mais nova: {bucketMs, firstMs, lastMs, points: [{t (ms desde a
    // época), temp, hum}]}; firstMs/lastMs são a primeira e a última leitura.
    bool readingSeries(const QString &deviceId, const QDateTime &since, int maxPoints,
                       QVariantMap *series, QString *errorMessage);
    QString databasePath() const;

private:
    bool createSchema(QString *errorMessage);
    bool ensureColumn(const QString &name, const QString &definition, QString *errorMessage,
                      bool *added = nullptr);
    void setSqlError(const QString &context, const QSqlError &error, QString *errorMessage) const;

    QString m_connectionName;
    QSqlDatabase m_database;
    QString m_databasePath;
};
