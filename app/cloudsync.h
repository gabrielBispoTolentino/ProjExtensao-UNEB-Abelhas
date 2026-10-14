#pragma once

#include "sensorpayload.h"

#include <QDateTime>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>

class QNetworkReply;
class SensorReadingStore;

// Mantém o banco local em dia com o Supabase, a cópia de referência
// compartilhada por todos os celulares e PCs:
//
//  * baixar: a cada poucos segundos busca as linhas novas no Supabase desde a
//    última vista (pelo id, então envios atrasados de leituras antigas também
//    chegam) e as entrega ao SensorMonitor, que guarda e atualiza a tela.
//    As linhas dos últimos ~2 minutos são conferidas de novo: dois envios
//    simultâneos podem aparecer fora da ordem de id, e uma linha pulada pelo
//    cursor nunca chegaria;
//  * enviar: sobe os registros que chegaram por Bluetooth enquanto a placa
//    estava sem internet. O Supabase ignora os que já tem (mesmo
//    device/boot/seq), então placa, celular e PC podem enviar o mesmo
//    registro sem problema.
//
// "Offline" significa que o servidor não foi alcançado. Um servidor que
// responde mas recusa os dados é informado à parte, com novas tentativas cada
// vez mais espaçadas, e um único registro ruim é separado para não travar os
// outros envios.
class CloudSync : public QObject
{
    Q_OBJECT

public:
    explicit CloudSync(SensorReadingStore *store, QObject *parent = nullptr);

    static bool isConfigured();

    void start();
    bool isOnline() const { return m_online; }
    bool hasServerError() const { return !m_pullError.isEmpty(); }
    int pendingCount() const { return m_pendingCount; }
    int rejectedCount() const { return m_rejectedCount; }

    // Um registro foi guardado localmente (Bluetooth): enviar em breve.
    void scheduleUpload();

    // Leituras foram apagadas do banco local: não baixá-las de novo (a
    // janela de conferência traria as mais recentes de volta).
    void localReadingsDeleted();

    // Apaga leituras no Supabase pela função delete_readings, que exige a
    // senha de administrador (supabase/schema.sql). Mesmos filtros de
    // SensorReadingStore::deleteReadings. Termina com remoteDeleteFinished.
    enum class DeleteResult { Done, Offline, WrongPassword, PasswordNotSet, NotInstalled, ServerError };
    void deleteRemote(const QString &adminPassword, const QString &deviceId, const QDateTime &before);
    bool isDeleting() const { return m_deleting; }

signals:
    void readingsReceived(const QList<SensorPayload> &readings);
    void stateChanged();          // online / erro / pendentes / recusados mudou
    void logMessage(const QString &message);
    // deleted: linhas apagadas, também quando parou no meio (result != Done).
    // httpStatus: preenchido em ServerError.
    void remoteDeleteFinished(CloudSync::DeleteResult result, qint64 deleted, int httpStatus);

private:
    void pull(bool continuation = false);
    void onPullFinished(QNetworkReply *reply, int generation);
    void deleteRemoteBatch();
    void onDeleteBatchFinished(QNetworkReply *reply);
    void push();
    void onPushFinished(QNetworkReply *reply, const QList<qint64> &localIds);
    void retryPushLater();
    QNetworkRequest request(const QString &pathAndQuery) const;
    void setOnline(bool online);
    void setPullError(const QString &error);
    void refreshCounts();

    SensorReadingStore    *m_store;
    QNetworkAccessManager  m_network;
    QTimer                 m_pullTimer;
    QTimer                 m_pushTimer;

    // Cursor do download: maior id visto e inserted_at mais recente visto
    // (para a janela de conferência).
    qint64                 m_lastRemoteId = 0;
    QDateTime              m_latestInsertedAt;
    // A conferência nunca volta antes deste ponto (definido após apagar localmente).
    QDateTime              m_recheckFloor;
    // Muda a cada limpeza local: um download em andamento pode trazer linhas apagadas.
    int                    m_pullGeneration = 0;
    bool                   m_pulling = false;
    QString                m_pullError;

    bool                   m_pushing = false;
    int                    m_batchLimit;        // diminui enquanto isola um registro recusado
    int                    m_pushRetryMs;       // cresce a cada envio que falha
    bool                   m_pushWaitsForNetwork = false;

    // Limpeza remota em andamento (um lote por requisição).
    bool                   m_deleting = false;
    QString                m_deletePassword;
    QString                m_deleteDeviceId;
    QDateTime              m_deleteBefore;
    qint64                 m_deletedCount = 0;

    bool                   m_online = false;
    int                    m_pendingCount = 0;
    int                    m_rejectedCount = 0;
};
