#include "LoginController.h"
#include "LoginModel.h"
#include "mainwindow.h"
#include "network/HttpClient.h"
#include <cmath>
#include <limits>
namespace csn {
LoginController::LoginController(MainWindow *view, LoginModel *model, HttpClient *http, QObject *parent)
    : QObject(parent), m_view(view), m_model(model), m_http(http)
{
    connect(view, &MainWindow::loginRequested, this, &LoginController::login);
    connect(view, &MainWindow::cancelRequested, this, &LoginController::cancel);
    connect(view, &MainWindow::logoutRequested, this, &LoginController::logout);
    connect(model, &LoginModel::changed, this, &LoginController::refreshView);
    connect(http, &HttpClient::succeeded, this, &LoginController::onSucceeded);
    connect(http, &HttpClient::failed, this, &LoginController::onFailed);
    refreshView();
}
void LoginController::login(const QUrl &endpoint, const QString &account, const QString &password)
{
    if (m_requestId != 0 || m_model->state() == LoginModel::State::LoggedIn)
        return;
    if (account.trimmed().isEmpty() || password.isEmpty()) {
        m_model->failLogin(tr("账号和密码不能为空。"));
        return;
    }
    if (!endpoint.isValid() || endpoint.host().isEmpty()
        || (endpoint.scheme() != QStringLiteral("http") && endpoint.scheme() != QStringLiteral("https"))
        || !endpoint.userInfo().isEmpty()) {
        m_model->failLogin(tr("请输入有效的 HTTP 或 HTTPS 登录接口地址。"));
        return;
    }
    m_model->beginLogin();
    m_requestId = m_http->postJson(endpoint,
        {{QStringLiteral("account"), account.trimmed()}, {QStringLiteral("password"), password}});
}
void LoginController::onSucceeded(quint64 id, int, const QJsonObject &body)
{
    if (id != m_requestId)
        return;
    m_requestId = 0;
    const QString token = body.value(QStringLiteral("accessToken")).toString();
    const QJsonObject user = body.value(QStringLiteral("user")).toObject();
    const QString userId = user.value(QStringLiteral("id")).toString();
    const QString name = user.value(QStringLiteral("displayName")).toString();
    const double lifetime = body.value(QStringLiteral("expiresIn")).toDouble(0);
    if (token.isEmpty() || userId.isEmpty() || name.isEmpty() || !std::isfinite(lifetime)
        || lifetime <= 0 || lifetime > std::numeric_limits<int>::max() || std::floor(lifetime) != lifetime) {
        m_model->failLogin(tr("登录响应缺少有效的会话或用户信息。"));
        return;
    }
    m_model->completeLogin(token, userId, name, int(lifetime));
}
void LoginController::onFailed(quint64 id, const QString &message, int)
{
    if (id != m_requestId)
        return;
    m_requestId = 0;
    m_model->failLogin(message);
}
void LoginController::cancel()
{
    if (m_requestId == 0)
        return;
    const quint64 id = m_requestId;
    m_requestId = 0;
    m_http->cancel(id);
    m_model->failLogin(tr("已取消登录。"));
}
void LoginController::logout()
{
    if (m_requestId != 0)
        cancel();
    m_model->logout();
}
void LoginController::refreshView()
{
    m_view->applyLoginState(m_model->message(),
        m_model->state() == LoginModel::State::LoggingIn,
        m_model->state() == LoginModel::State::LoggedIn);
}
} // namespace csn
