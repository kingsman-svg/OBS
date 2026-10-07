#include "HttpClient.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QVariant>

namespace csn {
HttpClient::HttpClient(QObject *parent)
    : QObject(parent), m_manager(new QNetworkAccessManager(this)) {}

quint64 HttpClient::postJson(const QUrl &url, const QJsonObject &body, int timeoutMs)
{
    const quint64 id = m_nextRequestId++;
    m_requests.insert(id, nullptr);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))
        || !url.userInfo().isEmpty() || timeoutMs <= 0) {
        rejectLater(id, tr("Invalid HTTP address or timeout."));
        return id;
    }
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (payload.size() > 64 * 1024) {
        rejectLater(id, tr("The JSON request exceeds 64 KiB."));
        return id;
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = m_manager->post(request, payload);
    m_requests[id] = reply;
    reply->setReadBufferSize(MaxResponseBytes + 1);
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id, reply] {
        if (m_requests.contains(id)) {
            reply->setProperty("csnTimedOut", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > MaxResponseBytes) {
            reply->setProperty("csnTooLarge", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply, timer] {
        timer->stop();
        finish(id, reply);
    });
    timer->start(timeoutMs);
    return id;
}

void HttpClient::rejectLater(quint64 id, const QString &message)
{
    QTimer::singleShot(0, this, [this, id, message] {
        if (m_requests.remove(id))
            emit failed(id, message, 0);
    });
}

void HttpClient::cancel(quint64 id)
{
    auto it = m_requests.find(id);
    if (it == m_requests.end())
        return;
    QNetworkReply *reply = it.value();
    m_requests.erase(it);
    if (reply) {
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void HttpClient::finish(quint64 id, QNetworkReply *reply)
{
    if (!m_requests.remove(id))
        return;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->property("csnTimedOut").toBool()) {
        emit failed(id, tr("HTTP request timed out."), status);
        return;
    }
    if (reply->property("csnTooLarge").toBool()) {
        emit failed(id, tr("The HTTP response exceeds 1 MiB."), status);
        return;
    }
    const QByteArray bytes = reply->readAll();
    if (bytes.size() > MaxResponseBytes) {
        emit failed(id, tr("The HTTP response exceeds 1 MiB."), status);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (status < 200 || status >= 300) {
        const QString detail = document.object().value(QStringLiteral("error")).toObject()
                                   .value(QStringLiteral("message")).toString();
        const QString message = status == 0 ? reply->errorString()
            : tr("HTTP %1: %2").arg(status).arg(detail.isEmpty() ? tr("Request failed.") : detail);
        emit failed(id, message, status);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        emit failed(id, reply->errorString(), status);
        return;
    }
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit failed(id, tr("The server did not return a JSON object."), status);
        return;
    }
    emit succeeded(id, status, document.object());
}
} // namespace csn
