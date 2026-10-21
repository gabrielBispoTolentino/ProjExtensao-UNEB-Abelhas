// Envio e download com o Supabase (CloudSync), contra um servidor falso em
// 127.0.0.1 (supabaseconfig_test.h): corpo e cabeçalhos do envio, registro
// recusado isolado, erros, cursor do download, paginação e a limpeza remota.
// Só os caminhos imediatos: as novas tentativas esperam 30 s ou mais.

#include "cloudsync.h"
#include "fakehttpserver.h"
#include "supabaseconfig_test.h"
#include "testutil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace testutil;

namespace {

QByteArray rowsJson(int firstId, int count, const QString &insertedAt = QStringLiteral("2026-10-21T17:00:00.000+00:00"))
{
    QJsonArray rows;
    for (int i = 0; i < count; ++i) {
        rows.append(QJsonObject{
            {"id", firstId + i}, {"device_id", deviceId}, {"device_name", "Colmeia Teste"},
            {"recorded_at", "2026-10-14T16:20:05+00:00"}, {"temperature", 32.5}, {"humidity", 50.0},
            {"boot", 9}, {"seq", firstId + i}, {"inserted_at", insertedAt},
        });
    }
    return QJsonDocument(rows).toJson(QJsonDocument::Compact);
}

// Supabase que aceita envios e não tem linhas novas.
FakeResponse acceptAll(const FakeRequest &request)
{
    if (request.method == "POST") {
        return {201, {}};
    }
    return {200, "[]"};
}

} // namespace

class TestCloudSync : public QObject
{
    Q_OBJECT

    FakeHttpServer *server = nullptr;

private slots:
    void init()
    {
        server = new FakeHttpServer;
        QVERIFY2(server->start(SupabaseConfig::testPort), "port 18767 busy");
    }
    void cleanup()
    {
        delete server;
        server = nullptr;
    }

    // ── Envio ──────────────────────────────────────────────────────────────
    void uploadsPendingRecords()
    {
        TempStore db;
        QString error;
        for (int seq = 1; seq <= 3; ++seq) {
            QVERIFY(db.store.insertReading(record(20, seq, 30 + seq, 50, secsAgo(600 - seq)), false, nullptr, &error));
        }
        SensorPayload unnamed = record(20, 4, 30, 50, secsAgo(60));
        unnamed.name.clear();
        QVERIFY(db.store.insertReading(unnamed, false, nullptr, &error));
        server->handler = acceptAll;

        CloudSync cloud(&db.store);
        cloud.start();
        QTRY_COMPARE_WITH_TIMEOUT(db.pending(), 0, 5000);
        QVERIFY(cloud.isOnline());

        const QList<FakeRequest> posts = server->requestsTo("POST", "/rest/v1/readings");
        QCOMPARE(posts.size(), 1);
        const FakeRequest &post = posts.first();
        QCOMPARE(post.query.queryItemValue("on_conflict"), QStringLiteral("device_id,boot,seq"));
        QCOMPARE(post.headers.value("apikey"), QByteArray("sb_publishable_test"));
        QVERIFY2(!post.headers.contains("authorization"), "sb_ keys must not go as a Bearer token");
        QVERIFY(post.headers.value("prefer").contains("resolution=ignore-duplicates"));
        QVERIFY(post.headers.value("content-type").startsWith("application/json"));

        const QJsonArray rows = QJsonDocument::fromJson(post.body).array();
        QCOMPARE(rows.size(), 4);
        const QJsonObject first = rows[0].toObject();
        QCOMPARE(first.value("device_id").toString(), deviceId);
        QCOMPARE(first.value("device_name").toString(), QStringLiteral("Colmeia Teste"));
        QCOMPARE(first.value("boot").toInt(), 20);
        QCOMPARE(first.value("seq").toInt(), 1);
        QCOMPARE(first.value("temperature").toDouble(), 31.0);
        QCOMPARE(first.value("source").toString(), QStringLiteral("app"));
        QVERIFY(first.value("recorded_at").toString().endsWith(QLatin1Char('Z')));
        // Todas as linhas com as mesmas chaves (o PostgREST recusa lotes misturados).
        const QJsonObject last = rows[3].toObject();
        QVERIFY(last.contains("device_name"));
        QVERIFY(last.value("device_name").isNull());
        QCOMPARE(last.keys(), first.keys());
    }

    void refusedRecordIsIsolated()
    {
        // Um registro inválido não pode travar os outros: o lote é dividido
        // até isolá-lo, os outros sobem e ele fica marcado como recusado.
        TempStore db;
        QString error;
        for (int seq = 1; seq <= 4; ++seq) {
            QVERIFY(db.store.insertReading(record(20, seq, 30, 50, secsAgo(600 - seq)), false, nullptr, &error));
        }
        server->handler = [](const FakeRequest &request) -> FakeResponse {
            if (request.method == "POST") {
                const QJsonArray rows = QJsonDocument::fromJson(request.body).array();
                for (const QJsonValue &row : rows) {
                    if (row.toObject().value("seq").toInt() == 3) {
                        return {400, R"({"code":"22P02","message":"invalid input"})"};
                    }
                }
                return {201, {}};
            }
            return {200, "[]"};
        };

        CloudSync cloud(&db.store);
        cloud.start();
        QTRY_COMPARE_WITH_TIMEOUT(db.rejected(), 1, 10000);
        // Depois de separar o recusado, o que sobrou do lote sobe na sequência.
        QTRY_COMPARE_WITH_TIMEOUT(db.pending(), 0, 5000);
        QCOMPARE(cloud.rejectedCount(), 1);

        QList<SensorReadingStore::PendingUpload> left;
        QVERIFY(db.store.pendingUploads(10, &left, &error));
        QVERIFY(left.isEmpty());
    }

    void serverErrorKeepsRecordsPending()
    {
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(60)), false, nullptr, &error));
        server->handler = [](const FakeRequest &request) -> FakeResponse {
            return request.method == "POST" ? FakeResponse{503, "busy"} : FakeResponse{200, "[]"};
        };
        CloudSync cloud(&db.store);
        cloud.start();
        QTRY_VERIFY_WITH_TIMEOUT(!server->requestsTo("POST", "/rest/v1/readings").isEmpty(), 5000);
        QTest::qWait(300);
        QCOMPARE(db.pending(), 1);   // tenta de novo mais tarde
        QCOMPARE(db.rejected(), 0);  // 5xx não é culpa do dado
        QVERIFY(cloud.isOnline());
    }

    void offlineKeepsRecordsPending()
    {
        delete server;   // ninguém escutando: "sem internet"
        server = nullptr;
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(20, 1, 30, 50, secsAgo(60)), false, nullptr, &error));
        CloudSync cloud(&db.store);
        QSignalSpy state(&cloud, &CloudSync::stateChanged);
        cloud.start();
        QTRY_VERIFY_WITH_TIMEOUT(state.count() > 0 && !cloud.isOnline(), 5000);
        QTest::qWait(300);
        QCOMPARE(db.pending(), 1);
        QCOMPARE(cloud.pendingCount(), 1);
    }

    // ── Download ───────────────────────────────────────────────────────────
    void pullDeliversRowsAndMovesCursor()
    {
        TempStore db;
        server->handler = [](const FakeRequest &request) -> FakeResponse {
            return request.method == "GET" ? FakeResponse{200, rowsJson(10, 2)} : FakeResponse{201, {}};
        };
        CloudSync cloud(&db.store);
        QSignalSpy received(&cloud, &CloudSync::readingsReceived);
        cloud.start();
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 5000);

        const auto readings = received.first().first().value<QList<SensorPayload>>();
        QCOMPARE(readings.size(), 2);
        QCOMPARE(readings[0].deviceId, deviceId);
        QCOMPARE(readings[0].boot, 9);
        QCOMPARE(readings[0].seq, 10);
        QCOMPARE(readings[0].tempDht, 32.5);
        QVERIFY(readings[0].recordFlag);
        QCOMPARE(readings[0].timestamp, QDateTime(QDate(2026, 10, 14), QTime(16, 20, 5), QTimeZone::UTC).toSecsSinceEpoch());
        QTRY_COMPARE_WITH_TIMEOUT(db.store.syncValue("supabase_last_id"), QStringLiteral("11"), 2000);

        const FakeRequest first = server->requestsTo("GET", "/rest/v1/readings").first();
        QCOMPARE(first.query.queryItemValue("id"), QStringLiteral("gt.0"));
        QCOMPARE(first.query.queryItemValue("order"), QStringLiteral("id.asc"));

        // Próxima abertura do app: continua do cursor e reconfere os últimos 2 min.
        server->requests.clear();
        server->handler = acceptAll;
        CloudSync again(&db.store);
        again.start();
        QTRY_VERIFY_WITH_TIMEOUT(!server->requestsTo("GET", "/rest/v1/readings").isEmpty(), 5000);
        const QString orFilter = server->requestsTo("GET", "/rest/v1/readings").first().query.queryItemValue("or", QUrl::FullyDecoded);
        QVERIFY2(orFilter.startsWith("(id.gt.11,inserted_at.gte."), qPrintable(orFilter));
        QVERIFY2(orFilter.contains("2026-10-21T16:58:00"), qPrintable(orFilter));   // 17:00 - 120 s
    }

    void pullPagesThroughLargeBacklog()
    {
        TempStore db;
        server->handler = [](const FakeRequest &request) -> FakeResponse {
            if (request.method != "GET") {
                return {201, {}};
            }
            const QString id = request.query.queryItemValue("id");
            if (id == "gt.0") return {200, rowsJson(1, 1000)};
            if (id == "gt.1000") return {200, rowsJson(1001, 3)};
            return {200, "[]"};
        };
        CloudSync cloud(&db.store);
        QObject::connect(&cloud, &CloudSync::readingsReceived, [&db](const QList<SensorPayload> &readings) {
            QString error;
            db.store.insertCloudReadings(readings, &error);
        });
        cloud.start();
        QTRY_COMPARE_WITH_TIMEOUT(db.total(), 1003, 10000);
        QCOMPARE(db.store.syncValue("supabase_last_id"), QStringLiteral("1003"));
    }

    void pulledCopyCoversPendingRecord()
    {
        // A placa enviou pelo Wi-Fi o mesmo registro que o celular recebeu
        // por Bluetooth: ao baixar, o pendente passa a sincronizado.
        TempStore db;
        QString error;
        QVERIFY(db.store.insertReading(record(9, 10, 32.5, 50, secsAgo(60)), false, nullptr, &error));
        server->handler = [](const FakeRequest &request) -> FakeResponse {
            // Envio falha (5xx) para que só o download resolva o pendente.
            return request.method == "GET" ? FakeResponse{200, rowsJson(10, 1)} : FakeResponse{503, {}};
        };
        CloudSync cloud(&db.store);
        QObject::connect(&cloud, &CloudSync::readingsReceived, [&db](const QList<SensorPayload> &readings) {
            QString e;
            db.store.insertCloudReadings(readings, &e);
        });
        cloud.start();
        QTRY_COMPARE_WITH_TIMEOUT(db.pending(), 0, 5000);
        QCOMPARE(db.total(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(cloud.pendingCount(), 0, 2000);
    }

    void unauthorizedIsAServerError()
    {
        TempStore db;
        server->handler = [](const FakeRequest &) -> FakeResponse { return {401, R"({"message":"Invalid API key"})"}; };
        CloudSync cloud(&db.store);
        cloud.start();
        QTRY_VERIFY_WITH_TIMEOUT(cloud.hasServerError(), 5000);
        QVERIFY(cloud.isOnline());   // alcançou o servidor, que recusou
    }

    void nullHumidityFromCloud()
    {
        // Umidade NULL no Supabase continua desconhecida aqui (antes virava 0).
        TempStore db;
        server->handler = [](const FakeRequest &request) -> FakeResponse {
            if (request.method != "GET") return {201, {}};
            QJsonArray rows{QJsonObject{{"id", 1}, {"device_id", deviceId}, {"recorded_at", "2026-10-14T16:20:05+00:00"},
                                        {"temperature", 30.0}, {"humidity", QJsonValue::Null}, {"boot", 1}, {"seq", 1},
                                        {"inserted_at", "2026-10-21T17:00:00+00:00"}}};
            return {200, QJsonDocument(rows).toJson()};
        };
        CloudSync cloud(&db.store);
        QSignalSpy received(&cloud, &CloudSync::readingsReceived);
        cloud.start();
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 5000);
        const auto readings = received.first().first().value<QList<SensorPayload>>();
        QVERIFY(qIsNaN(readings[0].humidity));
        QString error;
        QVERIFY(db.store.insertCloudReadings(readings, &error));
        StoredSensorReading latest;
        bool found = false;
        QVERIFY(db.store.latestReading(deviceId, &latest, &found, &error));
        QVERIFY(found && qIsNaN(latest.humidity));
    }

    // ── Limpeza na nuvem ───────────────────────────────────────────────────
    void deleteRemoteInBatches()
    {
        TempStore db;
        int calls = 0;
        server->handler = [&calls](const FakeRequest &request) -> FakeResponse {
            if (request.path == "/rest/v1/rpc/delete_readings") {
                return {200, ++calls == 1 ? "10000" : "5"};
            }
            return acceptAll(request);
        };
        CloudSync cloud(&db.store);
        QSignalSpy finished(&cloud, &CloudSync::remoteDeleteFinished);
        cloud.deleteRemote(QStringLiteral("segredo"), deviceId, QDateTime(QDate(2026, 10, 1), QTime(0, 0), QTimeZone::UTC));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.first().at(0).value<CloudSync::DeleteResult>(), CloudSync::DeleteResult::Done);
        QCOMPARE(finished.first().at(1).toLongLong(), 10005);
        QCOMPARE(calls, 2);

        const QJsonObject args = QJsonDocument::fromJson(server->requestsTo("POST", "/rest/v1/rpc/delete_readings").first().body).object();
        QCOMPARE(args.value("admin_password").toString(), QStringLiteral("segredo"));
        QCOMPARE(args.value("target_device_id").toString(), deviceId);
        QVERIFY(args.value("recorded_before").toString().startsWith("2026-10-01T00:00:00"));
        QCOMPARE(args.value("max_rows").toInt(), 10000);
    }

    void deleteRemoteErrors_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("expected");
        QTest::newRow("wrong password") << 400 << QByteArray(R"({"code":"28P01"})") << int(CloudSync::DeleteResult::WrongPassword);
        QTest::newRow("password not set") << 400 << QByteArray(R"({"code":"28000"})") << int(CloudSync::DeleteResult::PasswordNotSet);
        QTest::newRow("function missing") << 404 << QByteArray(R"({"code":"PGRST202"})") << int(CloudSync::DeleteResult::NotInstalled);
        QTest::newRow("other") << 500 << QByteArray("oops") << int(CloudSync::DeleteResult::ServerError);
        QTest::newRow("not a number") << 200 << QByteArray("\"ok\"") << int(CloudSync::DeleteResult::ServerError);
    }
    void deleteRemoteErrors()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, expected);
        TempStore db;
        server->handler = [status, body](const FakeRequest &) -> FakeResponse { return {status, body}; };
        CloudSync cloud(&db.store);
        QSignalSpy finished(&cloud, &CloudSync::remoteDeleteFinished);
        cloud.deleteRemote(QStringLiteral("x"), QString(), QDateTime());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(int(finished.first().at(0).value<CloudSync::DeleteResult>()), expected);
    }

    void deleteRemoteOffline()
    {
        delete server;
        server = nullptr;
        TempStore db;
        CloudSync cloud(&db.store);
        QSignalSpy finished(&cloud, &CloudSync::remoteDeleteFinished);
        cloud.deleteRemote(QStringLiteral("x"), QString(), QDateTime());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(finished.first().at(0).value<CloudSync::DeleteResult>(), CloudSync::DeleteResult::Offline);
    }
};

QTEST_GUILESS_MAIN(TestCloudSync)
#include "tst_cloudsync.moc"
