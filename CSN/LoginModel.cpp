#include "LoginModel.h"
namespace csn {
LoginModel::LoginModel(QObject *parent) : QObject(parent), m_message(tr("请输入账号和密码。")) {}
LoginModel::State LoginModel::state() const { return m_state; }
QString LoginModel::message() const { return m_message; }
QString LoginModel::accessToken() const { return m_accessToken; }
QString LoginModel::userId() const { return m_userId; }
QDateTime LoginModel::expiresAt() const { return m_expiresAt; }
void LoginModel::beginLogin()
{
    clearSession();
    m_state = State::LoggingIn;
    m_message = tr("正在登录……");
    emit changed();
}
void LoginModel::completeLogin(const QString &token, const QString &userId,
                              const QString &displayName, int expiresIn)
{
    m_accessToken = token;
    m_userId = userId;
    m_expiresAt = QDateTime::currentDateTimeUtc().addSecs(expiresIn);
    m_state = State::LoggedIn;
    m_message = tr("已登录：%1").arg(displayName);
    emit changed();
}
void LoginModel::failLogin(const QString &message)
{
    clearSession();
    m_state = State::LoggedOut;
    m_message = message;
    emit changed();
}
void LoginModel::logout()
{
    clearSession();
    m_state = State::LoggedOut;
    m_message = tr("已退出登录。");
    emit changed();
}
void LoginModel::clearSession()
{
    m_accessToken.clear();
    m_userId.clear();
    m_expiresAt = QDateTime();
}
} // namespace csn
