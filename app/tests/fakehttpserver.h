#pragma once

// Servidor HTTP mínimo para os testes, no lugar do Supabase e do GitHub:
// guarda cada pedido recebido e responde com o que o teste mandar (handler).
// Uma resposta por conexão ("Connection: close"), o suficiente para o
// QNetworkAccessManager.

#include <QHash>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>

struct FakeRequest
{
    QByteArray method;
    QString path;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;   // nomes em minúsculas
    QByteArray body;
};

struct FakeResponse
{
    int status = 200;
    QByteArray body;
    QByteArray contentType = "application/json";
};

class FakeHttpServer : public QTcpServer
{
public:
    std::function<FakeResponse(const FakeRequest &)> handler;
    QList<FakeRequest> requests;

    bool start(quint16 port = 0) { return listen(QHostAddress::LocalHost, port); }
    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }

    QList<FakeRequest> requestsTo(const QByteArray &method, const QString &path) const
    {
        QList<FakeRequest> found;
        for (const FakeRequest &request : requests) {
            if (request.method == method && request.path == path) {
                found.append(request);
            }
        }
        return found;
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(descriptor);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            buffer->append(socket->readAll());
            handle(socket, buffer.get());
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

private:
    void handle(QTcpSocket *socket, QByteArray *buffer)
    {
        const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        FakeRequest request;
        request.method = requestLine.value(0);
        const QUrl url(QStringLiteral("http://test") + QString::fromLatin1(requestLine.value(1)));
        request.path = url.path();
        request.query = QUrlQuery(url);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0) {
                request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
            }
        }
        const qsizetype length = request.headers.value("content-length").toLongLong();
        if (buffer->size() < headerEnd + 4 + length) {
            return;   // o corpo ainda está chegando
        }
        request.body = buffer->mid(headerEnd + 4, length);
        buffer->clear();
        requests.append(request);

        const FakeResponse response = handler ? handler(request) : FakeResponse{404, {}, "text/plain"};
        QByteArray reply = "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\n";
        reply += "Content-Type: " + response.contentType + "\r\n";
        reply += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        reply += "Connection: close\r\n\r\n";
        reply += response.body;
        socket->write(reply);
        socket->disconnectFromHost();
    }
};
