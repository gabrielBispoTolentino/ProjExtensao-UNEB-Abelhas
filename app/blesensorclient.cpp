// Cliente Bluetooth LE das placas (ver blesensorclient.h).

#include "blesensorclient.h"

#include <QBluetoothUuid>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyDescriptor>
#include <QPermissions>
#include <QStringList>

#ifdef Q_OS_ANDROID
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace {

// Nordic UART Service, mesmos UUIDs do firmware da ESP32.
const QBluetoothUuid nusServiceUuid(QStringLiteral("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"));
const QBluetoothUuid nusTxUuid(QStringLiteral("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")); // ESP32 -> app
const QBluetoothUuid nusRxUuid(QStringLiteral("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")); // app -> ESP32

constexpr auto deviceNamePrefix = "Colmeia-";

constexpr int scanTimeoutMs   = 10000;
constexpr int rescanDelayMs   = 30000;  // procura placas novas / perdidas de tempos em tempos
constexpr int maxLineBytes    = 4096;   // proteção contra um fluxo sem '\n' (a lista de redes Wi-Fi tem ~1 KB)
constexpr int minChunkBytes   = 20;     // carga ATT com o MTU padrão de 23
constexpr int maxChunkBytes   = 180;

} // namespace

BleSensorClient::BleSensorClient(QObject *parent)
    : QObject(parent)
{
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(rescanDelayMs);
    connect(&m_rescanTimer, &QTimer::timeout, this, &BleSensorClient::startScan);
}

BleSensorClient::~BleSensorClient()
{
    // O dono pode já estar sendo destruído; não chamar de volta.
    blockSignals(true);
    stop();
}

void BleSensorClient::start()
{
    m_stopped = false;
    requestPermissionThenScan();
}

void BleSensorClient::stop()
{
    m_stopped = true;
    m_rescanTimer.stop();
    if (m_agent) {
        m_agent->stop();
    }
    for (const QString &key : m_connections.keys()) {
        dropConnection(key);
    }
}

// Procura agora, sem esperar a próxima busca periódica (ex.: "Procurar de novo").
void BleSensorClient::scanNow()
{
    m_rescanTimer.stop();
    requestPermissionThenScan();
}

QMap<QString, QString> BleSensorClient::readyDevices() const
{
    QMap<QString, QString> devices;
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it) {
        if (it->ready) {
            devices.insert(it.key(), it->info.name());
        }
    }
    return devices;
}

// Enfileira uma linha JSON para o RX da placa, dividida para caber no MTU.
bool BleSensorClient::sendCommand(const QString &key, const QJsonObject &command)
{
    auto it = m_connections.find(key);
    if (it == m_connections.end() || !it->ready) {
        return false;
    }

    const QByteArray line = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    const int chunkSize = qBound(minChunkBytes, it->controller->mtu() - 3, maxChunkBytes);
    for (qsizetype i = 0; i < line.size(); i += chunkSize) {
        it->writeQueue.append(line.mid(i, chunkSize));
    }

    pumpWrites(key);
    return true;
}

// Uma escrita por vez, para os pedaços chegarem em ordem em qualquer plataforma.
void BleSensorClient::pumpWrites(const QString &key)
{
    auto it = m_connections.find(key);
    if (it == m_connections.end() || it->writing || it->writeQueue.isEmpty() || !it->service) {
        return;
    }

    const QLowEnergyCharacteristic rx = it->service->characteristic(nusRxUuid);
    if (!rx.isValid()) {
        it->writeQueue.clear();
        return;
    }

    it->writing = true;
    it->service->writeCharacteristic(rx, it->writeQueue.takeFirst(),
                                     QLowEnergyService::WriteWithResponse);
}

// ── Permissões ─────────────────────────────────────────────────────────────
// Android 12+ pede BLUETOOTH_SCAN/CONNECT em tempo de execução; versões
// anteriores pedem localização precisa para procurar dispositivos.
void BleSensorClient::requestPermissionThenScan()
{
    if (m_stopped) {
        return;
    }

#if QT_CONFIG(permissions)
    QBluetoothPermission bluetooth;
    bluetooth.setCommunicationModes(QBluetoothPermission::Access);
    switch (qApp->checkPermission(bluetooth)) {
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(bluetooth, this, [this](const QPermission &permission) {
            if (permission.status() == Qt::PermissionStatus::Granted) {
                requestPermissionThenScan();
            } else {
                setStatus(QStringLiteral("BLE: Bluetooth permission denied"));
                setState(State::NoPermission);
            }
        });
        return;
    case Qt::PermissionStatus::Denied:
        setStatus(QStringLiteral("BLE: Bluetooth permission denied"));
        setState(State::NoPermission);
        return;
    case Qt::PermissionStatus::Granted:
        break;
    }

#ifdef Q_OS_ANDROID
    if (QNativeInterface::QAndroidApplication::sdkVersion() < 31) {
        QLocationPermission location;
        location.setAccuracy(QLocationPermission::Precise);
        switch (qApp->checkPermission(location)) {
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(location, this, [this](const QPermission &permission) {
                if (permission.status() == Qt::PermissionStatus::Granted) {
                    requestPermissionThenScan();
                } else {
                    setStatus(QStringLiteral("BLE: location permission denied (needed to scan)"));
                    setState(State::NoPermission);
                }
            });
            return;
        case Qt::PermissionStatus::Denied:
            setStatus(QStringLiteral("BLE: location permission denied (needed to scan)"));
            setState(State::NoPermission);
            return;
        case Qt::PermissionStatus::Granted:
            break;
        }
    }
#endif
#endif

    startScan();
}

// ── Busca de placas ────────────────────────────────────────────────────────
void BleSensorClient::startScan()
{
    if (m_stopped) {
        return;
    }

    if (!m_agent) {
        m_agent = new QBluetoothDeviceDiscoveryAgent(this);
        m_agent->setLowEnergyDiscoveryTimeout(scanTimeoutMs);
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
                this, &BleSensorClient::onDeviceDiscovered);
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished,
                this, &BleSensorClient::onScanFinished);
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
                this, &BleSensorClient::onScanError);
    }

    if (m_agent->isActive()) {
        return;
    }

    if (m_connections.isEmpty()) {
        setStatus(QStringLiteral("BLE: scanning for sensors..."));
        setState(State::Scanning);
    }
    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void BleSensorClient::onDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
    if (!isSensorDevice(info) || m_connections.contains(deviceKey(info))) {
        return;
    }
    connectToDevice(info);
}

void BleSensorClient::onScanFinished()
{
    if (m_connections.isEmpty()) {
        setStatus(QStringLiteral("BLE: no sensor found. Retrying in %1s...")
                      .arg(rescanDelayMs / 1000));
        setState(State::NotFound);
    }
    scheduleRescan();
}

void BleSensorClient::onScanError(QBluetoothDeviceDiscoveryAgent::Error error)
{
    switch (error) {
    case QBluetoothDeviceDiscoveryAgent::PoweredOffError:
        setStatus(QStringLiteral("BLE: Bluetooth is turned off"));
        setState(State::BluetoothOff);
        break;
    case QBluetoothDeviceDiscoveryAgent::MissingPermissionsError:
        setStatus(QStringLiteral("BLE: missing Bluetooth permission"));
        setState(State::NoPermission);
        break;
    case QBluetoothDeviceDiscoveryAgent::LocationServiceTurnedOffError:
        setStatus(QStringLiteral("BLE: turn on Location to scan for sensors"));
        setState(State::LocationOff);
        break;
    default:
        setStatus(QStringLiteral("BLE: scan error - %1").arg(m_agent->errorString()));
        if (m_connections.isEmpty()) {
            setState(State::Error);
        }
        break;
    }
    scheduleRescan();
}

void BleSensorClient::scheduleRescan()
{
    if (!m_stopped && !m_rescanTimer.isActive()) {
        m_rescanTimer.start();
    }
}

// ── Conexão com cada placa ─────────────────────────────────────────────────
void BleSensorClient::connectToDevice(const QBluetoothDeviceInfo &info)
{
    const QString key = deviceKey(info);
    setStatus(QStringLiteral("BLE: connecting to %1...").arg(info.name()));

    auto *controller = QLowEnergyController::createCentral(info, this);

    Connection connection;
    connection.info = info;
    connection.controller = controller;
    m_connections.insert(key, connection);

    connect(controller, &QLowEnergyController::connected, controller, [controller]() {
        controller->discoverServices();
    });
    connect(controller, &QLowEnergyController::discoveryFinished, this, [this, key]() {
        setupService(key);
    });
    connect(controller, &QLowEnergyController::disconnected, this, [this, key, info]() {
        setStatus(QStringLiteral("BLE: %1 disconnected").arg(info.name()));
        dropConnection(key);
        scheduleRescan();
    });
    connect(controller, &QLowEnergyController::errorOccurred, this,
            [this, key, info, controller](QLowEnergyController::Error) {
        setStatus(QStringLiteral("BLE: %1 - %2").arg(info.name(), controller->errorString()));
        dropConnection(key);
        scheduleRescan();
    });

    controller->connectToDevice();
}

void BleSensorClient::setupService(const QString &key)
{
    auto it = m_connections.find(key);
    if (it == m_connections.end()) {
        return;
    }

    QLowEnergyController *controller = it->controller;
    QLowEnergyService *service = controller->createServiceObject(nusServiceUuid, controller);
    if (!service) {
        setStatus(QStringLiteral("BLE: %1 has no UART service").arg(it->info.name()));
        controller->disconnectFromDevice();
        return;
    }
    it->service = service;

    connect(service, &QLowEnergyService::stateChanged, this,
            [this, service, key](QLowEnergyService::ServiceState state) {
        if (state != QLowEnergyService::RemoteServiceDiscovered) {
            return;
        }

        const QLowEnergyCharacteristic tx = service->characteristic(nusTxUuid);
        const QLowEnergyDescriptor cccd = tx.isValid() ? tx.clientCharacteristicConfiguration()
                                                       : QLowEnergyDescriptor();
        if (!cccd.isValid()) {
            setStatus(QStringLiteral("BLE: sensor TX characteristic not found"));
            return;
        }

        // Liga as notificações: cada leitura chega em characteristicChanged.
        service->writeDescriptor(cccd, QLowEnergyCharacteristic::CCCDEnableNotification);

        auto it = m_connections.find(key);
        if (it != m_connections.end()) {
            it->linked = true;
        }

        // Firmwares antigos não têm RX gravável: leituras funcionam, comandos não.
        if (it != m_connections.end() && service->characteristic(nusRxUuid).isValid()) {
            it->ready = true;
            // Passa o relógio do celular para a placa, para ela datar as
            // leituras mesmo sem internet (NTP).
            sendCommand(key, QJsonObject{
                {QStringLiteral("cmd"), QStringLiteral("time")},
                {QStringLiteral("ts"),  QDateTime::currentSecsSinceEpoch()},
            });
            emit devicesChanged();
        }
        updateConnectedStatus();
    });

    connect(service, &QLowEnergyService::characteristicWritten, this,
            [this, key](const QLowEnergyCharacteristic &characteristic, const QByteArray &) {
        auto it = m_connections.find(key);
        if (it != m_connections.end() && characteristic.uuid() == nusRxUuid) {
            it->writing = false;
            pumpWrites(key);
        }
    });

    connect(service, &QLowEnergyService::errorOccurred, this,
            [this, key](QLowEnergyService::ServiceError error) {
        auto it = m_connections.find(key);
        if (it == m_connections.end() || error != QLowEnergyService::CharacteristicWriteError) {
            return;
        }
        // Descarta o resto do comando; o tempo limite de quem pediu avisa a falha.
        it->writeQueue.clear();
        it->writing = false;
        setStatus(QStringLiteral("BLE: failed to send command to %1").arg(it->info.name()));
    });

    connect(service, &QLowEnergyService::characteristicChanged, this,
            [this, key](const QLowEnergyCharacteristic &characteristic, const QByteArray &value) {
        if (characteristic.uuid() == nusTxUuid) {
            handleNotification(key, value);
        }
    });

    service->discoverDetails();
}

// O firmware divide cada linha JSON em notificações de 20 bytes; aqui elas
// são remontadas e cada linha completa vira uma mensagem.
void BleSensorClient::handleNotification(const QString &key, const QByteArray &chunk)
{
    auto it = m_connections.find(key);
    if (it == m_connections.end()) {
        return;
    }

    it->buffer.append(chunk);

    qsizetype newline;
    while ((newline = it->buffer.indexOf('\n')) >= 0) {
        const QByteArray line = it->buffer.left(newline).trimmed();
        it->buffer.remove(0, newline + 1);
        if (!line.isEmpty()) {
            // Respostas a comandos têm "type"; leituras não.
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (document.isObject() && document.object().contains(QLatin1String("type"))) {
                emit controlMessageReceived(key, document.object());
            } else {
                emit rawMessageReceived(QStringLiteral("ble/%1").arg(it->info.name()), line);
            }
            // O emit pode reentrar aqui; o iterador deixa de ser confiável.
            it = m_connections.find(key);
            if (it == m_connections.end()) {
                return;
            }
        }
    }

    if (it->buffer.size() > maxLineBytes) {
        it->buffer.clear();
    }
}

void BleSensorClient::dropConnection(const QString &key)
{
    auto it = m_connections.find(key);
    if (it == m_connections.end()) {
        return;
    }

    QLowEnergyController *controller = it->controller;
    const bool wasReady = it->ready;
    m_connections.erase(it);

    controller->disconnect(this);
    if (QLowEnergyService *service = controller->findChild<QLowEnergyService *>()) {
        service->disconnect(this);
    }
    controller->disconnectFromDevice();
    controller->deleteLater();   // apaga também o serviço (objeto filho)

    updateConnectedStatus();
    if (wasReady) {
        emit devicesChanged();
    }
}

// ── Auxiliares ─────────────────────────────────────────────────────────────
void BleSensorClient::updateConnectedStatus()
{
    if (m_connections.isEmpty()) {
        // Há uma nova busca agendada; até lá, nenhuma placa por perto.
        setState(m_state == State::Connected ? State::NotFound : m_state);
        emit stateChanged();   // connectedCount() mudou
        return;
    }
    if (connectedCount() > 0) {
        setState(State::Connected);
    }
    emit stateChanged();       // connectedCount() pode ter mudado

    QStringList names;
    for (const Connection &c : std::as_const(m_connections)) {
        names.append(c.info.name());
    }
    setStatus(QStringLiteral("BLE: connected to %1").arg(names.join(QStringLiteral(", "))));
}

void BleSensorClient::setStatus(const QString &status)
{
    emit statusChanged(status);
}

void BleSensorClient::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

int BleSensorClient::connectedCount() const
{
    int count = 0;
    for (const Connection &c : m_connections) {
        count += c.linked ? 1 : 0;
    }
    return count;
}

bool BleSensorClient::isSensorDevice(const QBluetoothDeviceInfo &info)
{
    if (!(info.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration)) {
        return false;
    }
    return info.serviceUuids().contains(nusServiceUuid)
           || info.name().startsWith(QLatin1String(deviceNamePrefix));
}

// macOS/iOS escondem o endereço MAC e mostram um UUID por computador.
QString BleSensorClient::deviceKey(const QBluetoothDeviceInfo &info)
{
    return info.address().isNull() ? info.deviceUuid().toString() : info.address().toString();
}
