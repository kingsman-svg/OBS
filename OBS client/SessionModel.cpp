#include "SessionModel.h"

namespace csn {
void SessionModel::setConnection(bool ready, bool connecting, const QString &message)
{
    m_ready = ready;
    m_connecting = connecting;
    m_message = message;
    if (!ready) {
        m_busy = false;
        m_room = {};
        m_rooms = {};
    }
    emit changed();
}
void SessionModel::setBusy(bool busy, const QString &message)
{
    m_busy = busy;
    m_message = message;
    emit changed();
}
void SessionModel::setRoom(const QJsonObject &room, const QString &message)
{
    m_room = room;
    m_message = message;
    for (qsizetype index = 0; index < m_rooms.size(); ++index)
        if (!room.isEmpty() && m_rooms.at(index).toObject().value(QStringLiteral("roomId")) == room.value(QStringLiteral("roomId")))
            m_rooms[index] = room;
    emit changed();
}
void SessionModel::setRooms(const QJsonArray &rooms)
{
    m_rooms = rooms;
    emit changed();
}
}
