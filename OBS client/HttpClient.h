#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
class QNetworkAccessManager;
class QNetworkReply;

namespace csn {
// 在 GUI 事件循环中异步收发 JSON；每次请求独立编号、超时、取消。
class HttpClient final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaxResponseBytes = 1024 * 1024;
    explicit HttpClient(QObject *parent = nullptr);
    quint64 postJson(const QUrl &url, const QJsonObject &body, int timeoutMs = 5000);
    quint64 getJson(const QUrl &url, int timeoutMs = 5000);
    // Cancellation suppresses both result signals.
    void cancel(quint64 requestId);
signals:
    void succeeded(quint64 requestId, int httpStatus, const QJsonObject &body);
    void failed(quint64 requestId, const QString &message, int httpStatus);
private:
    quint64 requestJson(const QUrl &url, const QJsonObject &body, bool post, int timeoutMs);
    void finish(quint64 requestId, QNetworkReply *reply);
    void rejectLater(quint64 requestId, const QString &message);
    QNetworkAccessManager *m_manager;
    QHash<quint64, QNetworkReply *> m_requests;
    quint64 m_nextRequestId = 1;
};
} // namespace csn
