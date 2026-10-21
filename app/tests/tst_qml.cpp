// Carrega a janela principal (Main.qml) e abre cada tela, com um SensorMonitor
// de verdade sobre um banco temporário com leituras: qualquer aviso ou erro
// do QML (propriedade inexistente, TypeError, import quebrado) falha o teste.
// Usa o módulo QML compilado pelo app (BEEDUINO_APP_BUILD_DIR), copiado para
// uma pasta temporária sem a linha "prefer" do qmldir.

#include "appupdater.h"
#include "sensormonitor.h"
#include "testutil.h"

#include <QDirIterator>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>

#include <functional>
#include <QTest>

using namespace testutil;

namespace {

bool copyModule(const QString &from, const QString &to)
{
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString target = to + QDir(from).relativeFilePath(file).prepend(QLatin1Char('/'));
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (QFileInfo(file).fileName() == QLatin1String("qmldir")) {
            QFile in(file), out(target);
            if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) return false;
            for (const QByteArray &line : in.readAll().split('\n')) {
                if (!line.startsWith("prefer ")) out.write(line + '\n');
            }
        } else if (!QFile::copy(file, target)) {
            return false;
        }
    }
    return true;
}

} // namespace

class TestQml : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QStringList problems;

private slots:
    void everyScreenLoadsWithoutErrors()
    {
        const QString appBuild = QStringLiteral(BEEDUINO_APP_BUILD_DIR);
        if (appBuild.isEmpty() || !QFileInfo::exists(appBuild + "/BeeDuino/qmldir")) {
            QSKIP("BEEDUINO_APP_BUILD_DIR not set to a built app (needs <build>/BeeDuino/qmldir)");
        }
        const QString modules = dir.filePath("qml");
        QVERIFY(copyModule(appBuild + "/BeeDuino", modules + "/BeeDuino"));

        QQuickStyle::setStyle(QStringLiteral("Basic"));   // como no main.cpp
        QFontDatabase::addApplicationFont(modules + "/BeeDuino/fonts/AtkinsonHyperlegible-Regular.ttf");

        // Uma segunda colmeia, vinda da nuvem sem umidade: o cartão mostra "–".
        {
            SensorReadingStore store;
            QString error;
            QVERIFY(store.open(dir.filePath("sensor_readings.db"), &error));
            SensorPayload noHumidity = record(9, 1, 31.0, qQNaN(), secsAgo(30), QStringLiteral("AABBCCDDEEFF"));
            noHumidity.name = QStringLiteral("Sem umidade");
            QVERIFY(store.insertCloudReadings({noHumidity}, &error));
        }
        SensorMonitor monitor;
        monitor.start(dir.filePath("sensor_readings.db"));
        // Uma colmeia com 2 dias de registros, para estatísticas e gráfico terem dados.
        for (int i = 0; i < 96; ++i) {
            QMetaObject::invokeMethod(&monitor, "handleBleMessage", Q_ARG(QString, QStringLiteral("k")),
                                      Q_ARG(QByteArray, recordLine(20, i + 1, 30 + (i % 10) * 0.3, 50 + (i % 7),
                                                                   secsAgo(48 * 3600 - i * 1800))));
        }
        AppUpdater updater;

        QQmlApplicationEngine engine;
        QObject::connect(&engine, &QQmlEngine::warnings, this, [this](const QList<QQmlError> &warnings) {
            for (const QQmlError &warning : warnings) problems << warning.toString();
        });
        engine.addImportPath(modules);
        engine.rootContext()->setContextProperty("sensorMonitor", &monitor);
        engine.rootContext()->setContextProperty("boardControl", static_cast<QObject *>(nullptr));
        engine.rootContext()->setContextProperty("outsideWeather", monitor.outsideWeather());
        engine.rootContext()->setContextProperty("appUpdater", &updater);
        engine.loadFromModule("BeeDuino", "Main");
        QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(problems.join('\n')));
        QObject *window = engine.rootObjects().first();
        QTest::qWait(200);

        // Lista de colmeias, cada ferramenta de dados e a tela da placa.
        const QStringList pages{"export", "weather", "cleanup", ""};
        for (const QString &page : pages) {
            window->setProperty("currentPage", page);
            QTest::qWait(200);
        }
        window->setProperty("selectedDeviceId", deviceId);
        window->setProperty("selectedDeviceName", "Colmeia Teste");
        window->setProperty("currentPage", "device");
        QTest::qWait(600);
        window->setProperty("currentPage", "");
        QTest::qWait(100);

        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        // Os delegates da lista estão na árvore visual, não na de QObject.
        std::function<bool(QQuickItem *)> showsDash = [&showsDash](QQuickItem *item) {
            if (item->property("text").toString() == QStringLiteral("–")) return true;
            for (QQuickItem *child : item->childItems()) {
                if (showsDash(child)) return true;
            }
            return false;
        };
        QVERIFY2(showsDash(qobject_cast<QQuickWindow *>(window)->contentItem()), "unknown humidity shown as a dash");
        QCOMPARE(window->property("title").toString(), QStringLiteral("BeeDuino"));
    }
};

QTEST_MAIN(TestQml)
#include "tst_qml.moc"
