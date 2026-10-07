#pragma once
#include <QJsonObject>
#include <QObject>
#include <QUrl>
class MainWindow;
namespace csn {
class HttpClient;
class LoginModel;
class LoginController final : public QObject {
    Q_OBJECT
public:
    LoginController(MainWindow *view, LoginModel *model, HttpClient *http, QObject *parent = nullptr);
public slots:
    void login(const QUrl &endpoint, const QString &account, const QString &password);
    void cancel();
    void logout();
private:
    void onSucceeded(quint64 requestId, int httpStatus, const QJsonObject &body);
    void onFailed(quint64 requestId, const QString &message, int httpStatus);
    void refreshView();
    MainWindow *m_view;
    LoginModel *m_model;
    HttpClient *m_http;
    quint64 m_requestId = 0;
    bool m_discovering = false;
    QUrl m_discoveryUrl;
    QJsonObject m_credentials;
};
} // namespace csn
