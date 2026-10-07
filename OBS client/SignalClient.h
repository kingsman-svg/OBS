#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
class QTcpSocket;

namespace csn {
// 单线程异步 TCP 信令：四字节大端长度 + UTF-8 JSON，不承担房间业务。
class SignalClient final : public QObject {
    Q_OBJECT
public:
    explicit SignalClient(QObject *parent = nullptr);
    ~SignalClient() override;
    void connectToServer(const QString &host, quint16 port, const QString &token);
    void disconnectFromServer(const QString &reason = QString());
    bool isReady() const { return m_ready; }
    QString request(const QString &type, const QJsonObject &fields = {});
signals:
    void stateChanged(bool ready, bool connecting, const QString &message);
    void response(const QString &id, const QString &type, bool ok,
                  const QJsonObject &data, const QString &error);
    void eventReceived(const QString &event, const QJsonObject &data);
private:
    QString send(const QString &type, const QJsonObject &fields);
    void readFrames();
    void handleMessage(const QJsonObject &message);
    void fail(const QString &message);
    QTcpSocket *m_socket = nullptr;
    QTimer m_deadline;
    QTimer m_heartbeat;
    QByteArray m_buffer;
    QHash<QString, QString> m_types;
    QHash<QString, QTimer *> m_timers;
    quint64 m_nextId = 1;
    bool m_ready = false;
};
}
