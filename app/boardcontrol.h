#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class BleSensorClient;

// Controla uma ESP32 pelo Bluetooth: coloca na internet (pede a lista de
// redes Wi-Fi e envia o SSID/senha escolhidos), renomeia, reinicia e define
// de quanto em quanto tempo ela grava um registro. Cada ferramenta do menu
// lateral do app usa este objeto.
//
// Protocolo (linhas JSON pelo Nordic UART Service, ver o firmware):
//   {"cmd":"status"}                        -> {"type":"status", ...}
//   {"cmd":"scan"}                          -> {"type":"networks","networks":[...]}
//   {"cmd":"wifi","ssid":"..","pass":".."}  -> {"type":"wifi","state":"connecting|connected|failed"}
//   {"cmd":"name","name":".."}              -> {"type":"name","name":".."}
//   {"cmd":"restart"}                       -> {"type":"restart"}, e a placa reinicia
//   {"cmd":"interval","minutes":N}          -> {"type":"interval","minutes":N}
class BoardControl : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantMap selectedDevice READ selectedDevice NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList networks READ networks NOTIFY networksChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY busyChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY busyChanged)
    Q_PROPERTY(bool renaming READ renaming NOTIFY busyChanged)
    Q_PROPERTY(bool restarting READ restarting NOTIFY busyChanged)
    Q_PROPERTY(bool savingInterval READ savingInterval NOTIFY busyChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)

public:
    explicit BoardControl(BleSensorClient *bleClient, QObject *parent = nullptr);

    QVariantList devices() const;
    QVariantMap selectedDevice() const;
    QVariantList networks() const;
    bool busy() const;
    bool scanning() const;
    bool connecting() const;
    bool renaming() const;
    bool restarting() const;
    bool savingInterval() const;
    QString message() const;
    bool messageIsError() const;

    Q_INVOKABLE void searchDevices();
    Q_INVOKABLE void selectDevice(const QString &key);
    Q_INVOKABLE void clearMessage();
    Q_INVOKABLE void scanNetworks();
    Q_INVOKABLE void connectToWifi(const QString &ssid, const QString &password);
    Q_INVOKABLE void renameBoard(const QString &name);
    Q_INVOKABLE void restartBoard();
    Q_INVOKABLE void setRecordInterval(int minutes);

signals:
    void devicesChanged();
    void networksChanged();
    void busyChanged();
    void messageChanged();

private:
    enum class Operation { None, Scan, Wifi, Rename, Restart, Interval };

    // Último estado que a placa informou sobre si mesma.
    struct BoardStatus {
        bool    known = false;
        QString deviceId;
        QString name;
        bool    wifi = false;
        QString ssid;
        QString ip;
        QString savedSsid;
        bool    cloud = false;   // o último envio ao Supabase deu certo
        int     interval = 0;    // intervalo de registro em minutos (0 = desconhecido / firmware antigo)
    };

    void onBleDevicesChanged();
    void onControlMessage(const QString &key, const QJsonObject &message);
    void handleNetworks(const QJsonObject &message);
    void handleWifiResult(const QString &key, const QJsonObject &message);
    QVariantMap deviceEntry(const QString &key, const QString &bleName) const;
    bool send(const QJsonObject &command);
    void startOperation(Operation operation, int timeoutMs);
    void finishOperation();
    void setMessage(const QString &message, bool isError = false);

    BleSensorClient           *m_ble;
    QMap<QString, BoardStatus> m_status;
    QString                    m_selectedKey;
    QVariantList               m_networks;
    Operation                  m_operation = Operation::None;
    QString                    m_message;
    bool                       m_messageIsError = false;
    QTimer                     m_timeout;
};
