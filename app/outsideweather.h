#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

class QNetworkReply;
class SensorReadingStore;

// Temperatura do ar lá fora, no local do apiário, para comparar com a da
// colmeia: sem ela não dá para saber se 30 °C na melgueira é a colônia
// aquecendo ou só uma tarde quente.
//
// Vem do Open-Meteo (open-meteo.com, gratuito e sem chave): temperatura a
// 2 m do chão, hora a hora, de modelos meteorológicos para as coordenadas
// escolhidas. Não é medida no apiário, mas segue bem o dia, a noite e as
// frentes frias. As horas ficam no banco local (outside_temperature), então
// gráfico, estatísticas e exportação funcionam sem internet.
//
// Um só local para todas as colmeias, escolhido pelo nome da cidade (tela
// "Local do apiário"). A cada hora busca os últimos dias; na primeira vez,
// os últimos 92 dias e, se houver leituras mais antigas, o arquivo
// histórico do Open-Meteo até a mais antiga.
class OutsideWeather : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasLocation READ hasLocation NOTIFY locationChanged)
    Q_PROPERTY(QString locationName READ locationName NOTIFY locationChanged)
    Q_PROPERTY(double latitude READ latitude NOTIFY locationChanged)
    Q_PROPERTY(double longitude READ longitude NOTIFY locationChanged)
    // Andamento da busca das temperaturas (vazio sem local).
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool updating READ updating NOTIFY statusChanged)
    Q_PROPERTY(bool statusFailed READ statusFailed NOTIFY statusChanged)
    // Busca de cidades: [{label, name, latitude, longitude}].
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchChanged)
    Q_PROPERTY(QString searchMessage READ searchMessage NOTIFY searchChanged)

public:
    explicit OutsideWeather(SensorReadingStore *store, QObject *parent = nullptr);

    // Depois de abrir o banco: carrega o local salvo e começa a atualizar.
    void start();

    Q_INVOKABLE void search(const QString &text);
    Q_INVOKABLE void setLocation(const QString &name, double latitude, double longitude);
    Q_INVOKABLE void clearLocation();
    Q_INVOKABLE void refresh();

    // Temperaturas guardadas entre fromMs e toMs, em médias de bucketMs (no
    // mínimo 1 h): [{t (ms desde a época), temp}].
    Q_INVOKABLE QVariantList series(double fromMs, double toMs, double bucketMs);

    bool hasLocation() const { return !m_locationName.isEmpty(); }
    QString locationName() const { return m_locationName; }
    double latitude() const { return m_latitude; }
    double longitude() const { return m_longitude; }
    QString statusText() const { return m_statusText; }
    bool updating() const { return m_reply != nullptr; }
    bool statusFailed() const { return m_statusFailed; }
    bool searching() const { return m_searchReply != nullptr; }
    QVariantList searchResults() const { return m_searchResults; }
    QString searchMessage() const { return m_searchMessage; }

signals:
    void locationChanged();
    void statusChanged();
    void searchChanged();
    void dataChanged();   // horas novas guardadas, ou todas apagadas (troca de local)

private:
    enum class Fetch { Recent, Archive };
    void fetch(Fetch kind, const QUrl &url);
    void onFetchFinished(QNetworkReply *reply, Fetch kind, int generation);
    void onSearchFinished(QNetworkReply *reply);
    // Pede o arquivo histórico se há leituras mais antigas que as horas guardadas.
    bool backfillIfNeeded();
    void setStatus(const QString &text, bool failed);
    void scheduleNext(int ms);

    SensorReadingStore    *m_store;
    QNetworkAccessManager  m_network;
    QTimer                 m_timer;

    QString                m_locationName;
    double                 m_latitude = 0.0;
    double                 m_longitude = 0.0;
    // Muda a cada troca de local: respostas do local anterior são descartadas.
    int                    m_generation = 0;
    // O arquivo histórico é pedido uma vez por local e sessão (algumas horas
    // antigas podem não existir lá, e não vale pedir de novo a cada hora).
    bool                   m_backfillTried = false;

    QPointer<QNetworkReply> m_reply;
    QString                m_statusText;
    bool                   m_statusFailed = false;
    QDateTime              m_lastUpdate;

    QPointer<QNetworkReply> m_searchReply;
    QVariantList           m_searchResults;
    QString                m_searchMessage;
};
