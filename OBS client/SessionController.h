#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
class MainWindow;
namespace csn {
class LoginModel;
class SessionModel;
class SignalClient;

// 编排登录后的信令生命周期与房间业务；两端共享代码但限制不同角色的操作。
class SessionController final : public QObject {
    Q_OBJECT
public:
    SessionController(MainWindow *view, LoginModel *login, SessionModel *model,
                      SignalClient *signal, QObject *parent = nullptr);
private:
    void onLoginChanged();
    void reconnect();
    void refreshView();
    void execute(const QString &type, const QJsonObject &fields = {});
    void refreshRooms();
    void onResponse(const QString &id, const QString &type, bool ok,
                    const QJsonObject &data, const QString &error);
    void onEvent(const QString &event, const QJsonObject &data);
    void scheduleExpiry();
    MainWindow *m_view;
    LoginModel *m_login;
    SessionModel *m_model;
    SignalClient *m_signal;
    QTimer m_expiry;
    bool m_loggedIn = false;
    QString m_pending;
    QString m_closedRoom;
    QJsonArray m_listing;
    int m_offset = 0;
};
}
