#include "LocalAuthServer.h"
#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <QVariant>

namespace csn {
namespace {
QJsonObject error(const QString &code, const QString &message)
{
    return {{QStringLiteral("error"), QJsonObject{
        {QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}
}
LocalAuthServer::LocalAuthServer(QObject *parent) : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &LocalAuthServer::acceptConnections);
}
bool LocalAuthServer::start()
{
    return m_server.isListening() || m_server.listen(QHostAddress::LocalHost, 0);
}
QUrl LocalAuthServer::endpoint() const
{
    return m_server.isListening()
        ? QUrl(QStringLiteral("http://127.0.0.1:%1/auth/login").arg(m_server.serverPort())) : QUrl();
}
QString LocalAuthServer::errorString() const { return m_server.errorString(); }
void LocalAuthServer::acceptConnections()
{
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        socket->setParent(this);
        if (findChildren<QTcpSocket *>(QString(), Qt::FindDirectChildrenOnly).size() > 16) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setReadBufferSize(24 * 1024);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRequest(socket); });
        QTimer::singleShot(5000, socket, [socket] { socket->abort(); socket->deleteLater(); });
        if (socket->bytesAvailable()) readRequest(socket);
    }
}
void LocalAuthServer::readRequest(QTcpSocket *socket)
{
    if (socket->property("handled").toBool()) return;
    const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
    socket->setProperty("request", request);
    const auto reject = [this, socket](int status, const QString &code, const QString &message) {
        respond(socket, status, error(code, message));
    };
    const qsizetype end = request.indexOf("\r\n\r\n");
    if (end > 8192 || (end < 0 && request.size() > 8192) || request.size() > 24 * 1024) {
        reject(413, QStringLiteral("REQUEST_TOO_LARGE"), tr("请求过大。")); return;
    }
    if (end < 0) return;
    const auto lines = request.left(end).split('\n');
    const auto first = lines.first().trimmed().split(' ');
    if (first.size() != 3 || first[2] != "HTTP/1.1") {
        reject(400, QStringLiteral("BAD_REQUEST"), tr("无效 HTTP 请求。")); return;
    }
    if (first[1] != "/auth/login") {
        reject(404, QStringLiteral("NOT_FOUND"), tr("接口不存在。")); return;
    }
    if (first[0] != "POST") {
        reject(405, QStringLiteral("METHOD_NOT_ALLOWED"), tr("登录接口只接受 POST。")); return;
    }
    qint64 length = -1;
    bool json = false;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            reject(400, QStringLiteral("BAD_REQUEST"), tr("无效 HTTP 请求头。")); return;
        }
        const QByteArray key = line.left(colon).toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (key == "transfer-encoding") {
            reject(400, QStringLiteral("BAD_REQUEST"), tr("请使用 Content-Length。")); return;
        }
        if (key == "content-length") {
            bool ok = false;
            const qint64 parsed = value.toLongLong(&ok);
            if (!ok || parsed < 0 || length >= 0) {
                reject(400, QStringLiteral("BAD_REQUEST"), tr("无效请求长度。")); return;
            }
            length = parsed;
        }
        if (key == "content-type") json = value.toLower().split(';').first().trimmed() == "application/json";
    }
    if (length < 0 || !json) {
        reject(400, QStringLiteral("BAD_REQUEST"), tr("需要 JSON 请求体及 Content-Length。")); return;
    }
    if (length > 16 * 1024) {
        reject(413, QStringLiteral("REQUEST_TOO_LARGE"), tr("请求体过大。")); return;
    }
    if (request.size() < end + 4 + length) return;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(request.mid(end + 4, length), &parseError);
    const auto body = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !body.value(QStringLiteral("account")).isString()
        || !body.value(QStringLiteral("password")).isString()) {
        reject(400, QStringLiteral("BAD_REQUEST"), tr("账号和密码必须为字符串。")); return;
    }
    if (body.value(QStringLiteral("account")).toString() != QStringLiteral("root")
        || body.value(QStringLiteral("password")).toString() != QStringLiteral("root")) {
        reject(401, QStringLiteral("INVALID_CREDENTIALS"), tr("账号或密码错误。")); return;
    }
    // This token demonstrates the client session only; there are no protected APIs yet.
    respond(socket, 200, {{QStringLiteral("accessToken"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("expiresIn"), 3600},
        {QStringLiteral("user"), QJsonObject{{QStringLiteral("id"), QStringLiteral("root")},
                                             {QStringLiteral("displayName"), QStringLiteral("root")}}}});
}
void LocalAuthServer::respond(QTcpSocket *socket, int status, const QJsonObject &body)
{
    socket->setProperty("handled", true);
    socket->setProperty("request", QVariant());
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\n"
        "Content-Type: application/json; charset=utf-8\r\nConnection: close\r\nContent-Length: "
        + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
    socket->disconnectFromHost();
}
}
