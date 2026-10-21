// Segundo plano e notificação no Android (ver androidbackground.h).

#include "androidbackground.h"

#include "sensormonitor.h"

#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>

namespace {

constexpr auto serviceClass = "com/abelhaspetterson/sensormonitor/MonitorService";
constexpr int throttleMs = 1000;

// Botão "Encerrar" da notificação (chamado na thread do Android).
void quitRequested(JNIEnv *, jclass)
{
    QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
}

QJniObject androidContext()
{
    return QNativeInterface::QAndroidApplication::context();
}

} // namespace

AndroidBackground::AndroidBackground(SensorMonitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
    m_throttle.setSingleShot(true);
    m_throttle.setInterval(throttleMs);
    connect(&m_throttle, &QTimer::timeout, this, &AndroidBackground::refresh);
    connect(m_monitor, &SensorMonitor::connectionChanged, &m_throttle, [this] {
        if (!m_throttle.isActive()) {
            m_throttle.start();
        }
    });
    // Uma notificação publicada antes da permissão (Android 13+) é descartada,
    // e nada a publica de novo sozinho. O pedido de permissão pausa o app;
    // ao voltar (e também depois de ativar as notificações nas
    // configurações), a notificação é publicada outra vez.
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            m_shownText.clear();
            refresh();
        }
    });
}

// Roda quando o app termina (voltar na lista de colmeias ou "Encerrar"):
// para o serviço e termina o processo logo depois (ver finishAndExit()).
AndroidBackground::~AndroidBackground()
{
    QJniObject::callStaticMethod<void>(serviceClass, "finishAndExit", "(Landroid/content/Context;)V",
                                       androidContext().object<jobject>());
}

void AndroidBackground::start()
{
    QJniEnvironment env;
    const JNINativeMethod methods[] = {
        { "nativeQuitRequested", "()V", reinterpret_cast<void *>(quitRequested) },
    };
    env.registerNativeMethods(serviceClass, methods, 1);
    refresh();
}

void AndroidBackground::refresh()
{
    const QJniObject context = androidContext();

    // A permissão de notificação (Android 13+) só depois de a do Bluetooth
    // ter sido respondida: o Android descarta um segundo pedido simultâneo.
    if (!m_askedPermission && m_monitor->blePermissionAnswered()) {
        m_askedPermission = true;
        QJniObject::callStaticMethod<void>(serviceClass, "requestNotificationPermission",
                                           "(Landroid/content/Context;)V", context.object<jobject>());
    }

    const QString text = m_monitor->backgroundStatusText();
    if (text == m_shownText) {
        return;
    }
    m_shownText = text;
    QJniObject::callStaticMethod<void>(serviceClass, "update",
                                       "(Landroid/content/Context;Ljava/lang/String;)V",
                                       context.object<jobject>(),
                                       QJniObject::fromString(text).object<jstring>());
}
