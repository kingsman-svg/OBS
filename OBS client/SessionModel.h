#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

namespace csn {
// 只保存工作台状态；不持有 socket，也不直接操作控件。
class SessionModel final : public QObject {
    Q_OBJECT
public:
    explicit SessionModel(QObject *parent = nullptr) : QObject(parent) {}
    bool ready() const { return m_ready; }
    bool busy() const { return m_busy; }
    bool connecting() const { return m_connecting; }
    QString message() const { return m_message; }
    QJsonObject room() const { return m_room; }
    QJsonArray rooms() const { return m_rooms; }
    void setConnection(bool ready, bool connecting, const QString &message);
    void setBusy(bool busy, const QString &message);
    void setRoom(const QJsonObject &room, const QString &message);
    void setRooms(const QJsonArray &rooms);
signals:
    void changed();
private:
    bool m_ready = false;
    bool m_busy = false;
    bool m_connecting = false;
    QString m_message;
    QJsonObject m_room;
    QJsonArray m_rooms;
};
}
