#pragma once
#include <QDateTime>
#include <QObject>
#include <QString>
namespace csn {
class LoginModel final : public QObject {
    Q_OBJECT
public:
    enum class State { LoggedOut, LoggingIn, LoggedIn };
    Q_ENUM(State)
    explicit LoginModel(QObject *parent = nullptr);
    State state() const;
    QString message() const;
    QString accessToken() const;
    QString userId() const;
    QDateTime expiresAt() const;
    void beginLogin();
    void completeLogin(const QString &token, const QString &userId,
                       const QString &displayName, int expiresIn);
    void failLogin(const QString &message);
    void logout();
signals:
    void changed();
private:
    void clearSession();
    State m_state = State::LoggedOut;
    QString m_message;
    QString m_accessToken;
    QString m_userId;
    QDateTime m_expiresAt;
};
} // namespace csn
