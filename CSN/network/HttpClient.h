#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
class QNetworkAccessManager;
class QNetworkReply;

namespace csn {
// One event-loop thread. Each POST has its own identity and timeout.
class HttpClient final : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaxResponseBytes = 1024 * 1024;
    explicit HttpClient(QObject *parent = nullptr);
    quint64 postJson(const QUrl &url, const QJsonObject &body, int timeoutMs = 5000);
    // Cancellation suppresses both result signals.
    void cancel(quint64 requestId);
signals:
    void succeeded(quint64 requestId, int httpStatus, const QJsonObject &body);
    void failed(quint64 requestId, const QString &message, int httpStatus);
private:
    void finish(quint64 requestId, QNetworkReply *reply);
    void rejectLater(quint64 requestId, const QString &message);
    QNetworkAccessManager *m_manager;
    QHash<quint64, QNetworkReply *> m_requests;
    quint64 m_nextRequestId = 1;
};
} // namespace csn
