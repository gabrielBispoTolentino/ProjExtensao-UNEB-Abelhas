#pragma once

#include "cloudsync.h"
#include "outsideweather.h"
#include "sensorreadingstore.h"

#ifdef HAVE_QT_BLUETOOTH
#include "blesensorclient.h"
#endif

#include <QObject>
#include <QMap>
#include <QUrl>
#include <QVariantList>

// Junta as leituras do Supabase (via CloudSync) e das placas por perto (via
// Bluetooth), mantém o banco local - espelho do Supabase mais os registros do
// Bluetooth ainda não enviados - e entrega ao QML a mais recente de cada placa.
class SensorMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // Estado das conexões mostrado ao usuário (texto + estado para os ícones).
    Q_PROPERTY(bool cloudConnected READ cloudConnected NOTIFY connectionChanged)
    Q_PROPERTY(QString cloudStatusText READ cloudStatusText NOTIFY connectionChanged)
    Q_PROPERTY(bool bleConnected READ bleConnected NOTIFY connectionChanged)
    Q_PROPERTY(QString bleStatusText READ bleStatusText NOTIFY connectionChanged)
    Q_PROPERTY(QString databasePath READ databasePath NOTIFY databasePathChanged)
    // Tela "Limpar dados".
    Q_PROPERTY(bool cloudConfigured READ cloudConfigured CONSTANT)
    Q_PROPERTY(bool cleaning READ cleaning NOTIFY cleanupChanged)
    Q_PROPERTY(QString cleanupMessage READ cleanupMessage NOTIFY cleanupChanged)
    Q_PROPERTY(bool cleanupFailed READ cleanupFailed NOTIFY cleanupChanged)

public:
    explicit SensorMonitor(QObject *parent = nullptr);

    void start(const QString &databasePath);
#ifdef HAVE_QT_BLUETOOTH
    BleSensorClient *bleClient() { return &m_bleClient; }
#endif
    OutsideWeather *outsideWeather() { return &m_weather; }

    // Tela da placa: estatísticas e dados do gráfico. rangeHours <= 0 = todas as leituras.
    Q_INVOKABLE QVariantMap deviceStats(const QString &deviceId, int rangeHours);
    Q_INVOKABLE QVariantMap deviceSeries(const QString &deviceId, int rangeHours, int maxPoints);

    // Limpeza: leituras de uma placa ("" = todas) mais antigas que
    // olderThanDays (0 = todas), neste aparelho e/ou no Supabase.
    // localDataCount: {count, pending (nunca enviados)} neste aparelho.
    Q_INVOKABLE QVariantMap localDataCount(const QString &deviceId, int olderThanDays);
    Q_INVOKABLE void cleanData(const QString &deviceId, int olderThanDays, bool local, bool cloud,
                               const QString &adminPassword);
    Q_INVOKABLE void clearCleanupMessage();

    // Exportação: leituras de uma placa ("" = todas) das últimas rangeHours
    // (0 = todas) num arquivo CSV escolhido pelo usuário (no Android, uma
    // URL content:// do seletor de arquivos). Devolve {ok, message}.
    Q_INVOKABLE QVariantMap exportCsv(const QUrl &file, const QString &deviceId, int rangeHours);
    Q_INVOKABLE int exportCount(const QString &deviceId, int rangeHours);
    bool cloudConfigured() const;
    bool cleaning() const { return m_cleaning; }
    QString cleanupMessage() const { return m_cleanupMessage; }
    bool cleanupFailed() const { return m_cleanupFailed; }

    QVariantList devices() const;
    QString status() const;
    bool cloudConnected() const;
    QString cloudStatusText() const;
    bool bleConnected() const;
    QString bleStatusText() const;
    QString databasePath() const;

signals:
    void statusChanged();
    void connectionChanged();
    void databasePathChanged();
    void devicesChanged();
    void deviceReadingAdded(const QString &deviceId);   // um registro novo foi guardado
    void cleanupChanged();
    void dataCleaned();                                 // leituras foram apagadas
private slots:
    void setStatus(const QString &status);
    void logMessage(const QString &message);
    void handleBleMessage(const QString &topic, const QByteArray &payload);
    void handleCloudReadings(const QList<SensorPayload> &readings);
    void handleRemoteDeleteFinished(CloudSync::DeleteResult result, qint64 deleted, int httpStatus);

private:
    // Apaga localmente as leituras da limpeza em curso; *deleted, ou false + mensagem.
    bool deleteLocalReadings(int *deleted, QString *errorMessage);
    void finishCleanup(const QString &message, bool failed);
    void loadLatestReadings();
    void showLatestOf(const QString &deviceId, const QString &source);
    void refreshRange(const QString &deviceId);
    void applyReading(const StoredSensorReading &reading, const QString &source);

    QMap<QString, StoredSensorReading> m_devices;   // leitura mais recente de cada placa
    QMap<QString, QString> m_deviceSources;         // deviceId -> "CLOUD" / "BLE"
    // Temperatura mín./máx. dos registros das últimas 24 h, por placa (a faixa
    // de cada cartão). Atualizada quando chega um registro, não a cada valor ao vivo.
    struct TemperatureRange { double min = 0.0; double max = 0.0; };
    QMap<QString, TemperatureRange> m_ranges;
    SensorReadingStore m_store;
    CloudSync m_cloud;
    OutsideWeather m_weather;
#ifdef HAVE_QT_BLUETOOTH
    BleSensorClient m_bleClient;
#endif
    QString m_status;

    // Limpeza em andamento (primeiro a parte da nuvem, depois a local).
    bool m_cleaning = false;
    bool m_cleanupLocal = false;
    QString m_cleanupDeviceId;
    QDateTime m_cleanupBefore;
    QString m_cleanupMessage;
    bool m_cleanupFailed = false;
};
