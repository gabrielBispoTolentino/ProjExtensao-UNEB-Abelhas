// Consulta de versão nova do AppUpdater contra um "GitHub" falso
// (BEEDUINO_UPDATE_API_URL): versão maior/igual/menor, rascunho e pré-versão,
// arquivo da plataforma, resumo das notas e erros. O download e a instalação
// foram testados à parte (eles fecham o app e abrem o instalador).

#include "appupdater.h"
#include "fakehttpserver.h"

#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace {

QByteArray release(const QString &tag, bool draft = false, bool prerelease = false,
                   const QStringList &assets = {QStringLiteral("BeeDuino-9.9.9-windows.zip"),
                                                QStringLiteral("BeeDuino-9.9.9-android.apk")})
{
    QJsonArray list;
    for (const QString &name : assets) {
        list.append(QJsonObject{{"name", name}, {"size", 1234},
                                {"digest", "sha256:" + QString(64, QLatin1Char('a'))},
                                {"browser_download_url", "http://127.0.0.1:1/" + name}});
    }
    return QJsonDocument(QJsonObject{
        {"tag_name", tag}, {"draft", draft}, {"prerelease", prerelease},
        {"html_url", "https://example.invalid/r"},
        {"body", "Resumo **curto**.\r\nSegunda linha.\r\n\r\n## Downloads\r\n\r\n| a | b |"},
        {"assets", list},
    }).toJson();
}

} // namespace

class TestUpdater : public QObject
{
    Q_OBJECT

    FakeHttpServer server;
    QByteArray reply;
    int status = 200;

    QString checkedState(AppUpdater &updater)
    {
        updater.check();
        if (updater.state() != QLatin1String("checking")) {
            return QStringLiteral("not checking: ") + updater.state();
        }
        QDeadlineTimer deadline(5000);
        while (updater.state() == QLatin1String("checking") && !deadline.hasExpired()) {
            QTest::qWait(20);
        }
        return updater.state();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(server.start());
        server.handler = [this](const FakeRequest &) { return FakeResponse{status, reply}; };
        qputenv("BEEDUINO_UPDATE_API_URL", (server.baseUrl() + "/releases/latest").toUtf8());
    }
    void init() { status = 200; }

    void newerVersionIsOffered()
    {
        reply = release(QStringLiteral("v9.9.9"));
        AppUpdater updater;
        QCOMPARE(checkedState(updater), QStringLiteral("available"));
        QCOMPARE(updater.latestVersion(), QStringLiteral("9.9.9"));
        QCOMPARE(updater.releaseNotes(), QStringLiteral("Resumo **curto**.\nSegunda linha."));
        QCOMPARE(updater.bytesTotal(), qint64(1234));   // o .zip, no Windows
        // O exe dos testes não é BeeDuino.exe: só abre a página de download.
        QVERIFY(!updater.canInstall());

        const FakeRequest request = server.requests.last();
        QVERIFY(request.headers.value("user-agent").startsWith("BeeDuino/1.4.0"));
        QCOMPARE(request.headers.value("accept"), QByteArray("application/vnd.github+json"));
    }

    void sameOrOlderVersionIsIgnored_data()
    {
        QTest::addColumn<QString>("tag");
        QTest::newRow("same") << QStringLiteral("v1.4.0");
        QTest::newRow("older") << QStringLiteral("v1.3.0");
        QTest::newRow("much older") << QStringLiteral("v0.9");
        QTest::newRow("not a version") << QStringLiteral("latest");
    }
    void sameOrOlderVersionIsIgnored()
    {
        QFETCH(QString, tag);
        reply = release(tag);
        AppUpdater updater;
        QCOMPARE(checkedState(updater), QStringLiteral("idle"));
        QVERIFY(updater.latestVersion().isEmpty());
    }

    void tagWithoutV()
    {
        reply = release(QStringLiteral("1.10.0"));   // 1.10 > 1.4 (não comparar como texto)
        AppUpdater updater;
        QCOMPARE(checkedState(updater), QStringLiteral("available"));
        QCOMPARE(updater.latestVersion(), QStringLiteral("1.10.0"));
    }

    void draftAndPrereleaseAreIgnored()
    {
        AppUpdater updater;
        reply = release(QStringLiteral("v9.9.9"), true, false);
        QCOMPARE(checkedState(updater), QStringLiteral("idle"));
        reply = release(QStringLiteral("v9.9.9"), false, true);
        QCOMPARE(checkedState(updater), QStringLiteral("idle"));
    }

    void releaseWithoutThisPlatformsFile()
    {
        reply = release(QStringLiteral("v9.9.9"), false, false, {QStringLiteral("BeeDuino-9.9.9-linux.tar.gz")});
        AppUpdater updater;
        QCOMPARE(checkedState(updater), QStringLiteral("available"));
        QVERIFY(!updater.canInstall());   // o botão abre a página
        QCOMPARE(updater.bytesTotal(), qint64(0));
    }

    void serverProblemsAreSilent_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("no releases") << 404 << QByteArray(R"({"message":"Not Found"})");
        QTest::newRow("rate limit") << 403 << QByteArray(R"({"message":"API rate limit exceeded"})");
        QTest::newRow("garbage") << 200 << QByteArray("<html>");
    }
    void serverProblemsAreSilent()
    {
        QFETCH(int, httpStatus);
        QFETCH(QByteArray, body);
        status = httpStatus;
        reply = body;
        AppUpdater updater;
        QCOMPARE(checkedState(updater), QStringLiteral("idle"));   // sem incomodar ninguém
        QVERIFY(updater.message().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestUpdater)
#include "tst_updater.moc"
