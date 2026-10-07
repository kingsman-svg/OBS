#pragma once
#include <QObject>
#include <QTcpServer>
#include <QUrl>
#include <QJsonObject>

class QTcpSocket;
namespace csn {
// Development-only HTTP service. Never binds to an external network interface.
class LocalAuthServer : public QObject
{
    Q_OBJECT
public:
    explicit LocalAuthServer(QObject *parent = nullptr);
    bool start();
    QUrl endpoint() const;
    QString errorString() const;
private:
    void acceptConnections();
    void readRequest(QTcpSocket *socket);
    void respond(QTcpSocket *socket, int status, const QJsonObject &body);
    QTcpServer m_server;
};
}
