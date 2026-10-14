#pragma once

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QJsonObject>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QMap>
#include <QObject>
#include <QTimer>

// Recebe as leituras das ESP32 por Bluetooth Low Energy.
//
// O firmware expõe um Nordic UART Service (NUS) e envia cada leitura como uma
// linha JSON terminada em '\n', dividida em notificações de 20 bytes. Este
// cliente procura placas que anunciam esse serviço (ou se chamam
// "Colmeia-XXXX"), mantém uma conexão por placa, remonta as linhas e emite
// cada uma com um pseudo-tópico "ble/<nome>" para o SensorMonitor.
//
// Comandos (ex.: configurar Wi-Fi) são escritos na característica RX do NUS
// como linhas JSON; as respostas da placa trazem um campo "type" e saem por
// controlMessageReceived, em vez de serem tratadas como leituras.
class BleSensorClient : public QObject
{
    Q_OBJECT

public:
    explicit BleSensorClient(QObject *parent = nullptr);
    ~BleSensorClient() override;

    // O que o Bluetooth está fazendo, para a interface.
    enum class State {
        Starting, Scanning, Connected, NotFound, BluetoothOff, NoPermission, LocationOff, Error
    };

    void start();
    void stop();
    void scanNow();

    State state() const { return m_state; }
    int connectedCount() const;   // placas cujas leituras estão chegando

    // Placas com o serviço UART pronto para comandos: chave -> nome BLE.
    QMap<QString, QString> readyDevices() const;
    bool sendCommand(const QString &key, const QJsonObject &command);

signals:
    void statusChanged(const QString &status);
    void rawMessageReceived(const QString &topic, const QByteArray &payload);
    void controlMessageReceived(const QString &key, const QJsonObject &message);
    void devicesChanged();
    void stateChanged();          // state() ou connectedCount() mudou

private slots:
    void startScan();
    void onDeviceDiscovered(const QBluetoothDeviceInfo &info);
    void onScanFinished();
    void onScanError(QBluetoothDeviceDiscoveryAgent::Error error);

private:
    struct Connection {
        QBluetoothDeviceInfo  info;
        QLowEnergyController *controller = nullptr;
        QLowEnergyService    *service    = nullptr;
        QByteArray            buffer;    // linha parcial até chegar o '\n'
        bool                  linked     = false;  // notificações ligadas: leituras chegam
        bool                  ready      = false;  // notificações ligadas e RX encontrado
        QList<QByteArray>     writeQueue;          // pedaços de comando ainda não escritos
        bool                  writing    = false;  // uma escrita no RX por vez
    };

    void requestPermissionThenScan();
    void connectToDevice(const QBluetoothDeviceInfo &info);
    void setupService(const QString &key);
    void handleNotification(const QString &key, const QByteArray &chunk);
    void pumpWrites(const QString &key);
    void dropConnection(const QString &key);
    void scheduleRescan();
    void setStatus(const QString &status);
    void setState(State state);
    void updateConnectedStatus();

    static bool isSensorDevice(const QBluetoothDeviceInfo &info);
    static QString deviceKey(const QBluetoothDeviceInfo &info);

    QBluetoothDeviceDiscoveryAgent *m_agent = nullptr;
    QMap<QString, Connection>       m_connections;
    QTimer                          m_rescanTimer;
    bool                            m_stopped = true;
    State                           m_state = State::Starting;
};
