#include "SignalClient.h"
#include <QJsonDocument>
#include <QTcpSocket>
#include <QtEndian>
#include <cmath>
#include <utility>

namespace csn {
SignalClient::SignalClient(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail(tr("信令连接或认证超时。")); });
    connect(&m_heartbeat, &QTimer::timeout, this, [this] { send(QStringLiteral("heartbeat"), {}); });
}

SignalClient::~SignalClient()
{
    // 析构时不再向业务层发送状态通知。
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

void SignalClient::connectToServer(const QString &host, quint16 port, const QString &token)
{
    disconnectFromServer();
    if (host.trimmed().isEmpty() || !port || token.isEmpty()) {
        fail(tr("信令地址、端口或登录凭证无效。"));
        return;
    }
    // 每次连接使用新 socket，旧连接的排队事件不会污染新会话。
    m_socket = new QTcpSocket(this);
    m_socket->setReadBufferSize(65540);
    connect(m_socket, &QTcpSocket::connected, this, [this, token] {
        m_deadline.start(5000);
        send(QStringLiteral("auth"), {{QStringLiteral("token"), token}});
    });
    connect(m_socket, &QTcpSocket::readyRead, this, &SignalClient::readFrames);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        fail(tr("信令连接失败：%1").arg(m_socket->errorString()));
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] { fail(tr("信令连接已断开，请重新连接。")); });
    emit stateChanged(false, true, tr("正在连接并认证信令服务器…"));
    m_deadline.start(5000);
    m_socket->connectToHost(host.trimmed(), port);
}

void SignalClient::disconnectFromServer(const QString &reason)
{
    m_deadline.stop();
    m_heartbeat.stop();
    m_ready = false;
    m_buffer.clear();
    // 超时结果可能意味着服务器已经执行操作。断开后由服务器清理房间，
    // 避免客户端误以为“创建失败”而留下无法管理的房间。
    for (auto *timer : std::as_const(m_timers)) {
        timer->stop();
        timer->deleteLater();
    }
    m_timers.clear();
    m_types.clear();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    emit stateChanged(false, false, reason.isEmpty() ? tr("信令未连接。") : reason);
}

void SignalClient::fail(const QString &message) { disconnectFromServer(message); }

QString SignalClient::request(const QString &type, const QJsonObject &fields)
{
    if (!m_ready || type == QStringLiteral("auth") || type == QStringLiteral("heartbeat"))
        return {};
    return send(type, fields);
}

QString SignalClient::send(const QString &type, const QJsonObject &fields)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return {};
    const QString id = QString::number(m_nextId++);
    QJsonObject object = fields;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("type"), type);
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    // 同时约束单帧、未完成请求和 socket 写缓冲，慢服务端不会无限占用内存。
    if (payload.isEmpty() || payload.size() > 65536 || m_types.size() >= 32
        || m_socket->bytesToWrite() + payload.size() + 4 > 256 * 1024) {
        fail(tr("信令发送队列超限。"));
        return {};
    }
    QByteArray frame(4, '\0');
    qToBigEndian<quint32>(quint32(payload.size()), frame.data());
    frame += payload;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_types.insert(id, type);
    m_timers.insert(id, timer);
    connect(timer, &QTimer::timeout, this, [this] { fail(tr("信令请求超时，连接已重置。")); });
    timer->start(5000);
    if (m_socket->write(frame) != frame.size()) {
        fail(tr("信令发送失败。"));
        return {};
    }
    return id;
}

void SignalClient::readFrames()
{
    m_buffer += m_socket->readAll();
    // 一次 readyRead 可以包含半帧或多帧，只有完整帧才交给 JSON 解析。
    while (m_socket && m_buffer.size() >= 4) {
        const quint32 size = qFromBigEndian<quint32>(m_buffer.constData());
        if (!size || size > 65536) {
            fail(tr("信令帧长度无效。"));
            return;
        }
        if (m_buffer.size() < qsizetype(size) + 4)
            return;
        const QByteArray bytes = m_buffer.mid(4, size);
        m_buffer.remove(0, qsizetype(size) + 4);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(tr("信令 JSON 格式无效。"));
            return;
        }
        handleMessage(document.object());
    }
}

void SignalClient::handleMessage(const QJsonObject &message)
{
    const QString kind = message.value(QStringLiteral("type")).toString();
    if (kind == QStringLiteral("event") && m_ready) {
        if (!message.value(QStringLiteral("data")).isObject()) {
            fail(tr("信令事件格式无效。"));
            return;
        }
        emit eventReceived(message.value(QStringLiteral("event")).toString(), message.value(QStringLiteral("data")).toObject());
        return;
    }
    const QString id = message.value(QStringLiteral("id")).toString();
    if (kind != QStringLiteral("response") || !m_types.contains(id)
        || !message.value(QStringLiteral("ok")).isBool()) {
        fail(tr("信令响应编号或格式无效。"));
        return;
    }
    const QString type = m_types.take(id);
    auto *timer = m_timers.take(id);
    timer->stop();
    timer->deleteLater();
    const bool ok = message.value(QStringLiteral("ok")).toBool();
    const QJsonObject data = message.value(QStringLiteral("data")).toObject();
    const QString error = message.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(tr("信令请求失败。"));
    if (type == QStringLiteral("auth")) {
        const double seconds = data.value(QStringLiteral("heartbeatSeconds")).toDouble();
        if (!ok || data.value(QStringLiteral("userId")).toString().isEmpty()
            || !std::isfinite(seconds) || seconds < 1 || seconds > 30 || std::floor(seconds) != seconds) {
            fail(ok ? tr("信令认证响应无效。") : error);
            return;
        }
        m_deadline.stop();
        m_ready = true;
        m_heartbeat.start(int(seconds) * 1000);
        // connected 回调仅用于发送认证，及时释放它捕获的令牌。
        QObject::disconnect(m_socket, &QTcpSocket::connected, this, nullptr);
        emit stateChanged(true, false, tr("信令已连接，可以操作房间。"));
    } else if (type == QStringLiteral("heartbeat")) {
        if (!ok)
            fail(error);
    } else {
        emit response(id, type, ok, data, error);
    }
}
}
