// Ponto de entrada: configura idioma, aparência e banco local, cria os
// objetos C++ usados pelo QML e abre a janela principal (Main.qml).

#include <QGuiApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>

#include "sensormonitor.h"

#ifdef HAVE_QT_BLUETOOTH
#include "boardcontrol.h"
#endif

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AbelhasPetterson"));
    QCoreApplication::setApplicationName(QStringLiteral("SensorMonitor"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/AbelhasPetterson/icons/bee.svg")));

    // Interface em português do Brasil: vírgula decimal, datas dd/MM.
    QLocale::setDefault(QLocale(QLocale::Portuguese, QLocale::Brazil));

    // Mesma aparência em todas as plataformas: controles Basic, com as cores
    // de Theme.qml, e a fonte Atkinson Hyperlegible embutida no app.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/AbelhasPetterson/fonts/AtkinsonHyperlegible-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/AbelhasPetterson/fonts/AtkinsonHyperlegible-Bold.ttf"));
    QFont appFont(QStringLiteral("Atkinson Hyperlegible"));
    appFont.setPixelSize(15);
    QGuiApplication::setFont(appFont);

    // Local fixo, independente de como o app é aberto:
    //   Windows: C:\Users\<você>\AppData\Roaming\AbelhasPetterson\SensorMonitor\sensor_readings.db
    //   Android: área privada do app (/data/data/<pacote>/files/sensor_readings.db)
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);

    SensorMonitor sensorMonitor;
#ifdef HAVE_QT_BLUETOOTH
    // Ferramentas da placa por Bluetooth: Wi-Fi, nome, reinício... (null as esconde no QML).
    BoardControl boardControl(sensorMonitor.bleClient());
    QObject *boardControlObject = &boardControl;
#else
    QObject *boardControlObject = nullptr;
#endif

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("sensorMonitor"), &sensorMonitor);
    engine.rootContext()->setContextProperty(QStringLiteral("boardControl"), boardControlObject);
    engine.rootContext()->setContextProperty(QStringLiteral("outsideWeather"), sensorMonitor.outsideWeather());
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("AbelhasPetterson", "Main");

    sensorMonitor.start(QDir(dataDir).filePath(QStringLiteral("sensor_readings.db")));

    return QCoreApplication::exec();
}