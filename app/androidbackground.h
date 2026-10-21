#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class SensorMonitor;

// Segundo plano no Android: sobe o MonitorService (android/src/...), um
// serviço em primeiro plano que mantém o app rodando fora da tela, e põe na
// notificação dele o andamento dos registros (despejo do SD, envio à nuvem,
// placas conectadas). O botão "Encerrar" da notificação fecha o app.
//
// Limite do Android: com a tela desligada a busca por placas novas não acha
// nada; as que já estavam conectadas continuam enviando.
class AndroidBackground : public QObject
{
    Q_OBJECT

public:
    explicit AndroidBackground(SensorMonitor *monitor, QObject *parent = nullptr);
    ~AndroidBackground() override;

    // Depois do SensorMonitor::start(): mostra a notificação e a mantém em dia.
    void start();

private:
    void refresh();

    SensorMonitor *m_monitor;
    QTimer         m_throttle;   // junta as mudanças: o despejo muda o estado várias vezes por segundo
    QString        m_shownText;
    bool           m_askedPermission = false;
};
