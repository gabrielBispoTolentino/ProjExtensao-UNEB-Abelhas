// Temperatura externa do Open-Meteo (ver outsideweather.h).

#include "outsideweather.h"

#include "sensorreadingstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace {

constexpr int refreshIntervalMs = 60 * 60 * 1000;   // o modelo tem um valor por hora
constexpr int retryIntervalMs   = 10 * 60 * 1000;   // depois de uma falha
constexpr int requestTimeoutMs  = 30000;
// Máximo que a previsão do Open-Meteo devolve para trás; antes disso, só
// pelo arquivo histórico.
constexpr int maxPastDays       = 92;

constexpr auto nameKey      = "outside_location_name";
constexpr auto latitudeKey  = "outside_latitude";
constexpr auto longitudeKey = "outside_longitude";

QNetworkRequest jsonRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(requestTimeoutMs);
    return request;
}

} // namespace

OutsideWeather::OutsideWeather(SensorReadingStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &OutsideWeather::refresh);
}

void OutsideWeather::start()
{
    m_locationName = m_store->syncValue(QString::fromLatin1(nameKey));
    m_latitude = m_store->syncValue(QString::fromLatin1(latitudeKey)).toDouble();
    m_longitude = m_store->syncValue(QString::fromLatin1(longitudeKey)).toDouble();
    emit locationChanged();
    refresh();
}

// ── Local ──────────────────────────────────────────────────────────────────
void OutsideWeather::setLocation(const QString &name, double latitude, double longitude)
{
    QString errorMessage;
    // Coordenadas guardadas com 4 casas (~10 m).
    const bool samePlace = hasLocation() && std::abs(latitude - m_latitude) < 1e-4
                           && std::abs(longitude - m_longitude) < 1e-4;
    if (!m_store->setSyncValue(QString::fromLatin1(nameKey), name, &errorMessage)
        || !m_store->setSyncValue(QString::fromLatin1(latitudeKey), QString::number(latitude, 'f', 4), &errorMessage)
        || !m_store->setSyncValue(QString::fromLatin1(longitudeKey), QString::number(longitude, 'f', 4), &errorMessage)) {
        setStatus(QStringLiteral("Não foi possível salvar o local: %1").arg(errorMessage), true);
        return;
    }
    m_locationName = name;
    m_latitude = latitude;
    m_longitude = longitude;
    m_searchResults.clear();
    m_searchMessage.clear();
    emit searchChanged();
    emit locationChanged();
    if (samePlace) {
        return;
    }

    // As horas guardadas eram de outro lugar. Apagadas antes do abort(): a
    // resposta cancelada já dispara a busca do novo local, que precisa
    // encontrar a tabela vazia para pedir os 92 dias.
    ++m_generation;
    m_backfillTried = false;
    m_lastUpdate = QDateTime();
    if (!m_store->clearOutsideTemperatures(&errorMessage)) {
        setStatus(errorMessage, true);
    }
    if (m_reply) {
        m_reply->abort();
    }
    emit dataChanged();
    refresh();
}

void OutsideWeather::clearLocation()
{
    // Sem local antes do abort(): a resposta cancelada não dispara outra busca.
    m_locationName.clear();
    m_latitude = 0.0;
    m_longitude = 0.0;
    m_lastUpdate = QDateTime();
    ++m_generation;
    m_backfillTried = false;
    m_timer.stop();
    if (m_reply) {
        m_reply->abort();
    }
    QString errorMessage;
    m_store->setSyncValue(QString::fromLatin1(nameKey), QString(), &errorMessage);
    m_store->setSyncValue(QString::fromLatin1(latitudeKey), QString(), &errorMessage);
    m_store->setSyncValue(QString::fromLatin1(longitudeKey), QString(), &errorMessage);
    m_store->clearOutsideTemperatures(&errorMessage);
    emit locationChanged();
    emit dataChanged();
    setStatus(QString(), false);
}

// ── Busca de cidades (geocodificação do Open-Meteo) ────────────────────────
void OutsideWeather::search(const QString &text)
{
    if (m_searchReply) {
        m_searchReply->abort();
        m_searchReply = nullptr;
    }
    const QString name = text.trimmed();
    if (name.size() < 2) {
        m_searchResults.clear();
        m_searchMessage = QStringLiteral("Digite pelo menos 2 letras do nome da cidade.");
        emit searchChanged();
        return;
    }

    QUrl url(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("name"), name);
    query.addQueryItem(QStringLiteral("count"), QStringLiteral("8"));
    query.addQueryItem(QStringLiteral("language"), QStringLiteral("pt"));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(query);

    m_searchMessage.clear();
    m_searchReply = m_network.get(jsonRequest(url));
    QNetworkReply *reply = m_searchReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onSearchFinished(reply); });
    emit searchChanged();
}

void OutsideWeather::onSearchFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        return;   // substituída por uma busca nova
    }
    m_searchReply = nullptr;
    m_searchResults.clear();

    if (reply->error() != QNetworkReply::NoError) {
        m_searchMessage = QStringLiteral("Sem conexão com o serviço de busca. Tente de novo.");
        emit searchChanged();
        return;
    }

    const QJsonArray results = QJsonDocument::fromJson(reply->readAll()).object()
                                   .value(QStringLiteral("results")).toArray();
    for (const QJsonValue &value : results) {
        const QJsonObject place = value.toObject();
        // "Cruz das Almas, Bahia, Brasil": o estado distingue cidades homônimas.
        QStringList parts{place.value(QStringLiteral("name")).toString()};
        for (const auto key : {"admin1", "country"}) {
            const QString part = place.value(QLatin1String(key)).toString();
            if (!part.isEmpty() && !parts.contains(part)) {
                parts.append(part);
            }
        }
        m_searchResults.append(QVariantMap{
            {QStringLiteral("label"), parts.join(QStringLiteral(", "))},
            {QStringLiteral("latitude"), place.value(QStringLiteral("latitude")).toDouble()},
            {QStringLiteral("longitude"), place.value(QStringLiteral("longitude")).toDouble()},
        });
    }
    if (m_searchResults.isEmpty()) {
        m_searchMessage = QStringLiteral("Nenhuma cidade encontrada com esse nome.");
    }
    emit searchChanged();
}

// ── Temperaturas ───────────────────────────────────────────────────────────
void OutsideWeather::refresh()
{
    if (!hasLocation() || m_reply) {
        return;
    }

    // Só o que falta desde a última hora guardada (com folga de um dia).
    qint64 firstSecs = 0;
    qint64 lastSecs = 0;
    QString errorMessage;
    m_store->outsideTemperatureBounds(&firstSecs, &lastSecs, &errorMessage);
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    const int pastDays = lastSecs == 0
                             ? maxPastDays
                             : int(std::clamp<qint64>((nowSecs - lastSecs) / 86400 + 2, 2, maxPastDays));

    QUrl url(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("latitude"), QString::number(m_latitude, 'f', 4));
    query.addQueryItem(QStringLiteral("longitude"), QString::number(m_longitude, 'f', 4));
    query.addQueryItem(QStringLiteral("hourly"), QStringLiteral("temperature_2m"));
    query.addQueryItem(QStringLiteral("past_days"), QString::number(pastDays));
    query.addQueryItem(QStringLiteral("forecast_days"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("timezone"), QStringLiteral("UTC"));
    query.addQueryItem(QStringLiteral("timeformat"), QStringLiteral("unixtime"));
    url.setQuery(query);
    fetch(Fetch::Recent, url);
}

bool OutsideWeather::backfillIfNeeded()
{
    if (m_backfillTried) {
        return false;
    }
    qint64 firstSecs = 0;
    qint64 lastSecs = 0;
    qint64 oldestSecs = 0;
    QString errorMessage;
    if (!m_store->outsideTemperatureBounds(&firstSecs, &lastSecs, &errorMessage)
        || !m_store->oldestReadingSecs(&oldestSecs, &errorMessage)
        || oldestSecs == 0 || firstSecs == 0 || oldestSecs >= firstSecs - 3600) {
        return false;
    }
    m_backfillTried = true;

    // O arquivo trabalha com dias inteiros (UTC); a última hora já guardada
    // entra de novo, sem problema (a gravação substitui).
    const auto day = [](qint64 secs) {
        return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC).date().toString(Qt::ISODate);
    };
    QUrl url(QStringLiteral("https://archive-api.open-meteo.com/v1/archive"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("latitude"), QString::number(m_latitude, 'f', 4));
    query.addQueryItem(QStringLiteral("longitude"), QString::number(m_longitude, 'f', 4));
    query.addQueryItem(QStringLiteral("start_date"), day(oldestSecs));
    query.addQueryItem(QStringLiteral("end_date"), day(firstSecs));
    query.addQueryItem(QStringLiteral("hourly"), QStringLiteral("temperature_2m"));
    query.addQueryItem(QStringLiteral("timezone"), QStringLiteral("UTC"));
    query.addQueryItem(QStringLiteral("timeformat"), QStringLiteral("unixtime"));
    url.setQuery(query);
    fetch(Fetch::Archive, url);
    return true;
}

void OutsideWeather::fetch(Fetch kind, const QUrl &url)
{
    m_reply = m_network.get(jsonRequest(url));
    QNetworkReply *reply = m_reply;
    const int generation = m_generation;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, kind, generation]() { onFetchFinished(reply, kind, generation); });
    setStatus(kind == Fetch::Archive ? QStringLiteral("Buscando o histórico da temperatura externa...")
                                     : QStringLiteral("Buscando a temperatura externa..."),
              false);
}

void OutsideWeather::onFetchFinished(QNetworkReply *reply, Fetch kind, int generation)
{
    reply->deleteLater();
    if (reply == m_reply) {
        m_reply = nullptr;
    }
    if (generation != m_generation) {
        refresh();   // o local mudou durante a busca
        return;
    }

    const auto failed = [this](const QString &text) {
        setStatus(m_lastUpdate.isValid()
                      ? text + QStringLiteral(" Usando as horas já guardadas.")
                      : text,
                  true);
        scheduleNext(retryIntervalMs);
    };
    if (reply->error() != QNetworkReply::NoError) {
        failed(QStringLiteral("Sem conexão com o serviço do tempo (Open-Meteo)."));
        return;
    }

    const QJsonObject hourly = QJsonDocument::fromJson(reply->readAll()).object()
                                   .value(QStringLiteral("hourly")).toObject();
    const QJsonArray times = hourly.value(QStringLiteral("time")).toArray();
    const QJsonArray temps = hourly.value(QStringLiteral("temperature_2m")).toArray();
    if (times.isEmpty() || times.size() != temps.size()) {
        failed(QStringLiteral("O serviço do tempo respondeu num formato inesperado."));
        return;
    }

    // Só horas que já passaram (a previsão das próximas horas fica de fora)
    // e com valor (o arquivo pode não ter as horas mais recentes).
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    QList<QPair<qint64, double>> hours;
    for (qsizetype i = 0; i < times.size(); ++i) {
        const qint64 hour = qint64(times.at(i).toDouble());
        if (hour <= nowSecs && temps.at(i).isDouble()) {
            hours.append({hour, temps.at(i).toDouble()});
        }
    }
    QString errorMessage;
    if (!m_store->saveOutsideTemperatures(hours, &errorMessage)) {
        setStatus(errorMessage, true);
        scheduleNext(retryIntervalMs);
        return;
    }
    if (!hours.isEmpty()) {
        emit dataChanged();
    }

    if (kind == Fetch::Recent) {
        m_lastUpdate = QDateTime::currentDateTime();
        if (backfillIfNeeded()) {
            return;   // o status agora é o do histórico
        }
    }
    setStatus(QStringLiteral("Atualizada às %1.").arg(m_lastUpdate.toString(QStringLiteral("HH:mm"))), false);
    scheduleNext(refreshIntervalMs);
}

QVariantList OutsideWeather::series(double fromMs, double toMs, double bucketMs)
{
    QVariantList points;
    QString errorMessage;
    if (!m_store->outsideSeries(qint64(fromMs), qint64(toMs), qint64(bucketMs), &points, &errorMessage)) {
        setStatus(errorMessage, true);
    }
    return points;
}

void OutsideWeather::setStatus(const QString &text, bool failed)
{
    m_statusText = text;
    m_statusFailed = failed;
    emit statusChanged();
}

void OutsideWeather::scheduleNext(int ms)
{
    if (hasLocation()) {
        m_timer.start(ms);
    }
}
