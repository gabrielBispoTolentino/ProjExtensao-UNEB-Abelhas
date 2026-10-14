// Ferramentas da placa por Bluetooth (ver boardcontrol.h).

#include "boardcontrol.h"

#include "blesensorclient.h"

#include <QJsonArray>

namespace {

// Tempos limite das respostas. A placa responde entre outras tarefas: a
// busca de redes leva alguns segundos, uma rede é abandonada após 20 s e um
// envio ao Supabase pode ocupá-la por até ~10 s. Os limites só pegam uma
// placa que nunca responde; respostas que chegam depois ainda valem (ver
// onControlMessage).
constexpr int scanTimeoutMs    = 30000;
constexpr int connectTimeoutMs = 45000;
constexpr int replyTimeoutMs   = 20000;  // nome / intervalo / reinício

// Uma placa reiniciada volta ao Bluetooth em alguns segundos; procurá-la
// nessa hora em vez de esperar a próxima busca periódica.
constexpr int rescanAfterRestartMs = 8000;

constexpr int maxNameBytes = 24;  // mesmo limite do firmware (cabe na resposta de busca BLE)

QJsonObject command(const QString &name)
{
    return QJsonObject{{QStringLiteral("cmd"), name}};
}

} // namespace

BoardControl::BoardControl(BleSensorClient *bleClient, QObject *parent)
    : QObject(parent)
    , m_ble(bleClient)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        finishOperation();
        setMessage(QStringLiteral("A placa não respondeu. Verifique se ela está por perto e tente de novo."), true);
    });

    connect(m_ble, &BleSensorClient::devicesChanged, this, &BoardControl::onBleDevicesChanged);
    connect(m_ble, &BleSensorClient::controlMessageReceived, this, &BoardControl::onControlMessage);
}

QVariantList BoardControl::devices() const
{
    QVariantList list;
    const QMap<QString, QString> ready = m_ble->readyDevices();
    for (auto it = ready.cbegin(); it != ready.cend(); ++it) {
        list.append(deviceEntry(it.key(), it.value()));
    }
    return list;
}

QVariantMap BoardControl::selectedDevice() const
{
    const QMap<QString, QString> ready = m_ble->readyDevices();
    if (m_selectedKey.isEmpty() || !ready.contains(m_selectedKey)) {
        return {};
    }
    return deviceEntry(m_selectedKey, ready.value(m_selectedKey));
}

QVariantList BoardControl::networks() const
{
    return m_networks;
}

bool BoardControl::busy() const
{
    return m_operation != Operation::None;
}

bool BoardControl::scanning() const
{
    return m_operation == Operation::Scan;
}

bool BoardControl::connecting() const
{
    return m_operation == Operation::Wifi;
}

bool BoardControl::renaming() const
{
    return m_operation == Operation::Rename;
}

bool BoardControl::restarting() const
{
    return m_operation == Operation::Restart;
}

bool BoardControl::savingInterval() const
{
    return m_operation == Operation::Interval;
}

QString BoardControl::message() const
{
    return m_message;
}

bool BoardControl::messageIsError() const
{
    return m_messageIsError;
}

// ── Ações vindas da interface ──────────────────────────────────────────────
void BoardControl::searchDevices()
{
    m_ble->scanNow();
    setMessage(QStringLiteral("Procurando placas por perto..."));
}

void BoardControl::selectDevice(const QString &key)
{
    if (key == m_selectedKey) {
        return;
    }

    finishOperation();
    m_selectedKey = key;
    m_networks.clear();
    emit networksChanged();
    emit devicesChanged();
    setMessage(QString());

    send(command(QStringLiteral("status")));
}

// Cada tela do menu começa limpa, sem o resultado da ferramenta anterior.
void BoardControl::clearMessage()
{
    if (!busy()) {
        setMessage(QString());
    }
}

void BoardControl::scanNetworks()
{
    if (busy()) {
        return;
    }
    if (send(command(QStringLiteral("scan")))) {
        startOperation(Operation::Scan, scanTimeoutMs);
        setMessage(QStringLiteral("A placa está procurando redes Wi-Fi..."));
    }
}

void BoardControl::connectToWifi(const QString &ssid, const QString &password)
{
    if (busy()) {
        return;
    }

    const QString trimmedSsid = ssid.trimmed();
    if (trimmedSsid.isEmpty()) {
        setMessage(QStringLiteral("Escolha ou digite o nome de uma rede."), true);
        return;
    }
    if (trimmedSsid.toUtf8().size() > 32 || password.toUtf8().size() > 63) {
        setMessage(QStringLiteral("O nome da rede ou a senha é longo demais."), true);
        return;
    }

    QJsonObject wifi = command(QStringLiteral("wifi"));
    wifi.insert(QStringLiteral("ssid"), trimmedSsid);
    wifi.insert(QStringLiteral("pass"), password);
    if (send(wifi)) {
        startOperation(Operation::Wifi, connectTimeoutMs);
        setMessage(QStringLiteral("Enviando a rede para a placa..."));
    }
}

void BoardControl::renameBoard(const QString &name)
{
    if (busy()) {
        return;
    }

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        setMessage(QStringLiteral("Digite um nome para a placa."), true);
        return;
    }
    if (trimmed.toUtf8().size() > maxNameBytes) {
        setMessage(QStringLiteral("O nome é longo demais (máximo de %1 caracteres).").arg(maxNameBytes), true);
        return;
    }
    if (trimmed.contains(QLatin1Char('"')) || trimmed.contains(QLatin1Char('\\'))) {
        setMessage(QStringLiteral("O nome não pode conter \" nem \\."), true);
        return;
    }

    QJsonObject rename = command(QStringLiteral("name"));
    rename.insert(QStringLiteral("name"), trimmed);
    if (send(rename)) {
        startOperation(Operation::Rename, replyTimeoutMs);
        setMessage(QStringLiteral("Renomeando a placa..."));
    }
}

void BoardControl::restartBoard()
{
    if (busy()) {
        return;
    }
    if (send(command(QStringLiteral("restart")))) {
        startOperation(Operation::Restart, replyTimeoutMs);
        setMessage(QStringLiteral("Pedindo para a placa reiniciar..."));
    }
}

void BoardControl::setRecordInterval(int minutes)
{
    if (busy()) {
        return;
    }
    if (minutes < 1 || minutes > 60) {
        setMessage(QStringLiteral("O intervalo deve ser de 1 a 60 minutos."), true);
        return;
    }

    QJsonObject interval = command(QStringLiteral("interval"));
    interval.insert(QStringLiteral("minutes"), minutes);
    if (send(interval)) {
        startOperation(Operation::Interval, replyTimeoutMs);
        setMessage(QStringLiteral("Salvando o intervalo na placa..."));
    }
}

// ── Mensagens das placas ───────────────────────────────────────────────────
void BoardControl::onBleDevicesChanged()
{
    const QMap<QString, QString> ready = m_ble->readyDevices();

    // Esquece as placas que sumiram.
    for (auto it = m_status.begin(); it != m_status.end();) {
        it = ready.contains(it.key()) ? std::next(it) : m_status.erase(it);
    }

    if (!m_selectedKey.isEmpty() && !ready.contains(m_selectedKey)) {
        const Operation interrupted = m_operation;
        finishOperation();
        m_selectedKey.clear();
        m_networks.clear();
        emit networksChanged();

        if (interrupted == Operation::Restart) {
            // A placa reiniciou antes da resposta chegar: também é sucesso.
            setMessage(QStringLiteral("A placa está reiniciando. Ela volta a se conectar em alguns segundos."));
            QTimer::singleShot(rescanAfterRestartMs, m_ble, &BleSensorClient::scanNow);
        } else if (interrupted != Operation::None) {
            setMessage(QStringLiteral("A placa se desconectou. Aproxime-a e tente de novo."), true);
        }
    }

    // Pergunta a cada placa nova como ela está (Wi-Fi, nome).
    for (auto it = ready.cbegin(); it != ready.cend(); ++it) {
        if (!m_status.contains(it.key())) {
            m_status.insert(it.key(), BoardStatus());
            m_ble->sendCommand(it.key(), command(QStringLiteral("status")));
        }
    }

    if (m_selectedKey.isEmpty() && ready.size() == 1) {
        m_selectedKey = ready.firstKey();
    }

    emit devicesChanged();
}

void BoardControl::onControlMessage(const QString &key, const QJsonObject &message)
{
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("status")) {
        BoardStatus &status = m_status[key];
        status.known     = true;
        status.deviceId  = message.value(QStringLiteral("deviceId")).toString();
        status.name      = message.value(QStringLiteral("name")).toString();
        status.wifi      = message.value(QStringLiteral("wifi")).toBool();
        status.ssid      = message.value(QStringLiteral("ssid")).toString();
        status.ip        = message.value(QStringLiteral("ip")).toString();
        status.savedSsid = message.value(QStringLiteral("savedSsid")).toString();
        status.cloud     = message.value(QStringLiteral("cloud")).toBool();
        status.interval  = message.value(QStringLiteral("interval")).toInt();
        emit devicesChanged();
        return;
    }

    // Mudanças confirmadas pela placa valem mesmo com resposta atrasada
    // (depois do tempo limite) ou de uma placa que não está mais selecionada.
    if (type == QLatin1String("name")) {
        m_status[key].name = message.value(QStringLiteral("name")).toString();
        emit devicesChanged();
    } else if (type == QLatin1String("interval")) {
        m_status[key].interval = message.value(QStringLiteral("minutes")).toInt();
        emit devicesChanged();
    }

    if (key != m_selectedKey) {
        return;
    }
    // Resposta a esta operação, ou resposta atrasada sem outra operação em curso.
    const auto answers = [this](Operation operation) {
        return m_operation == operation || m_operation == Operation::None;
    };

    if (type == QLatin1String("networks") && answers(Operation::Scan)) {
        handleNetworks(message);
    } else if (type == QLatin1String("wifi")) {
        handleWifiResult(key, message);
    } else if (type == QLatin1String("name") && answers(Operation::Rename)) {
        finishOperation();
        setMessage(QStringLiteral("Agora a placa se chama \"%1\".").arg(m_status[key].name));
    } else if (type == QLatin1String("interval") && answers(Operation::Interval)) {
        finishOperation();
        const int minutes = m_status[key].interval;
        setMessage(minutes == 1
                       ? QStringLiteral("Pronto: a placa grava um registro por minuto.")
                       : QStringLiteral("Pronto: a placa grava um registro a cada %1 minutos.").arg(minutes));
    } else if (type == QLatin1String("restart") && m_operation == Operation::Restart) {
        finishOperation();
        setMessage(QStringLiteral("A placa está reiniciando. Ela volta a se conectar em alguns segundos."));
        QTimer::singleShot(rescanAfterRestartMs, m_ble, &BleSensorClient::scanNow);
    } else if (type == QLatin1String("error")) {
        finishOperation();
        setMessage(message.value(QStringLiteral("message")).toString(), true);
    }
}

void BoardControl::handleNetworks(const QJsonObject &message)
{
    finishOperation();

    if (message.contains(QStringLiteral("error"))) {
        setMessage(QStringLiteral("A placa não conseguiu procurar redes: %1")
                       .arg(message.value(QStringLiteral("error")).toString()), true);
        return;
    }

    m_networks.clear();
    const QJsonArray list = message.value(QStringLiteral("networks")).toArray();
    for (const QJsonValue &value : list) {
        const QJsonObject network = value.toObject();
        QVariantMap entry;
        entry.insert(QStringLiteral("ssid"),   network.value(QStringLiteral("ssid")).toString());
        entry.insert(QStringLiteral("rssi"),   network.value(QStringLiteral("rssi")).toInt());
        entry.insert(QStringLiteral("secure"), network.value(QStringLiteral("secure")).toBool(true));
        m_networks.append(entry);
    }
    emit networksChanged();

    setMessage(m_networks.isEmpty()
                   ? QStringLiteral("Nenhuma rede encontrada. Você ainda pode digitar o nome abaixo.")
                   : QStringLiteral("Escolha a rede que a placa deve usar."));
}

void BoardControl::handleWifiResult(const QString &key, const QJsonObject &message)
{
    const QString state = message.value(QStringLiteral("state")).toString();
    const QString ssid  = message.value(QStringLiteral("ssid")).toString();

    if (state == QLatin1String("connecting")) {
        setMessage(QStringLiteral("A placa está se conectando a \"%1\"...").arg(ssid));
        return;
    }

    // O resultado da placa vale mesmo se chegou depois do nosso tempo limite.
    if (m_operation != Operation::Wifi && m_operation != Operation::None) {
        return;   // resposta atrasada durante outra operação: a atualização de estado dela cobre
    }
    finishOperation();

    if (state == QLatin1String("connected")) {
        const QString ip = message.value(QStringLiteral("ip")).toString();
        BoardStatus &status = m_status[key];
        status.wifi      = true;
        status.ssid      = ssid;
        status.ip        = ip;
        status.savedSsid = ssid;
        emit devicesChanged();
        setMessage(QStringLiteral("Conectada a \"%1\" (IP %2). As leituras agora vão pela internet.")
                       .arg(ssid, ip));
        return;
    }

    const QString reason = message.value(QStringLiteral("reason")).toString();
    QString text;
    if (reason == QLatin1String("wrong_password")) {
        text = QStringLiteral("Senha incorreta para \"%1\".").arg(ssid);
    } else if (reason == QLatin1String("not_found")) {
        text = QStringLiteral("A placa não encontrou \"%1\". Confira o nome e se a rede é de 2,4 GHz.").arg(ssid);
    } else {
        text = QStringLiteral("Não foi possível conectar a \"%1\". Confira a senha e tente de novo.").arg(ssid);
    }
    setMessage(text, true);

    // A placa voltou para a rede anterior; atualiza o que ela informa.
    send(command(QStringLiteral("status")));
}

// ── Auxiliares ─────────────────────────────────────────────────────────────
QVariantMap BoardControl::deviceEntry(const QString &key, const QString &bleName) const
{
    const BoardStatus status = m_status.value(key);

    QVariantMap entry;
    entry.insert(QStringLiteral("key"),         key);
    // O que a placa informa é mais recente que o nome visto na busca.
    entry.insert(QStringLiteral("name"),        status.name.isEmpty() ? bleName : status.name);
    entry.insert(QStringLiteral("statusKnown"), status.known);
    entry.insert(QStringLiteral("deviceId"),    status.deviceId);
    entry.insert(QStringLiteral("wifi"),        status.wifi);
    entry.insert(QStringLiteral("ssid"),        status.ssid);
    entry.insert(QStringLiteral("ip"),          status.ip);
    entry.insert(QStringLiteral("savedSsid"),   status.savedSsid);
    entry.insert(QStringLiteral("cloud"),       status.cloud);
    entry.insert(QStringLiteral("interval"),    status.interval);
    entry.insert(QStringLiteral("selected"),    key == m_selectedKey);
    return entry;
}

bool BoardControl::send(const QJsonObject &command)
{
    if (m_selectedKey.isEmpty() || !m_ble->sendCommand(m_selectedKey, command)) {
        setMessage(QStringLiteral("Conecte-se a uma placa por Bluetooth primeiro."), true);
        return false;
    }
    return true;
}

void BoardControl::startOperation(Operation operation, int timeoutMs)
{
    m_operation = operation;
    m_timeout.start(timeoutMs);
    emit busyChanged();
}

void BoardControl::finishOperation()
{
    m_timeout.stop();
    if (m_operation == Operation::None) {
        return;
    }
    m_operation = Operation::None;
    emit busyChanged();
}

void BoardControl::setMessage(const QString &message, bool isError)
{
    if (m_message == message && m_messageIsError == isError) {
        return;
    }
    m_message        = message;
    m_messageIsError = isError;
    emit messageChanged();
}
