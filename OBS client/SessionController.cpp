#include "SessionController.h"
#include "LoginModel.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "mainwindow.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace csn {
namespace {
bool validRoom(const QJsonObject &room)
{
    return !room.value(QStringLiteral("roomId")).toString().isEmpty()
        && !room.value(QStringLiteral("title")).toString().isEmpty()
        && room.value(QStringLiteral("streaming")).isBool()
        && room.value(QStringLiteral("viewers")).isDouble();
}
}

SessionController::SessionController(MainWindow *view, LoginModel *login, SessionModel *model,
                                     SignalClient *signal, QObject *parent)
    : QObject(parent), m_view(view), m_login(login), m_model(model), m_signal(signal)
{
    m_expiry.setSingleShot(true);
    connect(&m_expiry, &QTimer::timeout, this, &SessionController::scheduleExpiry);
    connect(login, &LoginModel::changed, this, &SessionController::onLoginChanged);
    connect(model, &SessionModel::changed, this, &SessionController::refreshView);
    connect(view, &MainWindow::reconnectRequested, this, &SessionController::reconnect);
    connect(view, &MainWindow::refreshRoomsRequested, this, &SessionController::refreshRooms);
    connect(view, &MainWindow::createRoomRequested, this, [this](const QString &title) {
        if (m_view->role() != ClientRole::Publisher || !m_model->room().isEmpty()) return;
        if (title.trimmed().isEmpty() || title.toUtf8().size() > 128) {
            m_model->setBusy(false, tr("房间标题须为 1～128 个 UTF-8 字节。"));
            return;
        }
        execute(QStringLiteral("room.create"), {{QStringLiteral("title"), title.trimmed()}});
    });
    connect(view, &MainWindow::joinRoomRequested, this, [this](const QString &roomId) {
        if (m_view->role() != ClientRole::Player || !m_model->room().isEmpty() || roomId.isEmpty()) return;
        execute(QStringLiteral("room.join"), {{QStringLiteral("roomId"), roomId}});
    });
    connect(view, &MainWindow::leaveRoomRequested, this, [this] {
        if (!m_model->room().isEmpty()) execute(QStringLiteral("room.leave"));
    });
    connect(signal, &SignalClient::stateChanged, this, [this](bool ready, bool connecting, const QString &message) {
        if (!ready) {
            m_pending.clear();
            m_listing = {};
        }
        m_model->setConnection(ready, connecting, message);
        if (ready && m_view->role() == ClientRole::Player) refreshRooms();
    });
    connect(signal, &SignalClient::response, this, &SessionController::onResponse);
    connect(signal, &SignalClient::eventReceived, this, &SessionController::onEvent);
    refreshView();
    onLoginChanged();
}

void SessionController::onLoginChanged()
{
    const bool loggedIn = m_login->state() == LoginModel::State::LoggedIn;
    if (loggedIn == m_loggedIn) return;
    m_loggedIn = loggedIn;
    if (loggedIn) {
        scheduleExpiry();
        reconnect();
    } else {
        m_expiry.stop();
        m_signal->disconnectFromServer(tr("已退出登录。"));
    }
}

void SessionController::scheduleExpiry()
{
    const qint64 remaining = QDateTime::currentDateTimeUtc().msecsTo(m_login->expiresAt());
    if (remaining <= 0) {
        m_login->failLogin(tr("登录已过期，请重新登录。"));
        return;
    }
    m_expiry.start(int(std::min<qint64>(remaining, std::numeric_limits<int>::max())));
}

void SessionController::reconnect()
{
    if (!m_loggedIn || m_model->connecting() || m_signal->isReady()) return;
    if (m_login->expiresAt() <= QDateTime::currentDateTimeUtc()) {
        m_login->failLogin(tr("登录已过期，请重新登录。"));
        return;
    }
    m_signal->connectToServer(m_view->signalHost(), m_view->signalPort(), m_login->accessToken());
}

void SessionController::execute(const QString &type, const QJsonObject &fields)
{
    if (!m_signal->isReady() || !m_pending.isEmpty()) return;
    if (type == QStringLiteral("room.create") || type == QStringLiteral("room.join")) m_closedRoom.clear();
    m_model->setBusy(true, tr("正在处理房间请求…"));
    m_pending = m_signal->request(type, fields);
    if (m_pending.isEmpty() && m_signal->isReady()) m_model->setBusy(false, tr("未能发送房间请求。"));
}

void SessionController::refreshRooms()
{
    if (m_view->role() != ClientRole::Player || !m_pending.isEmpty() || !m_signal->isReady()) return;
    m_listing = {};
    m_offset = 0;
    execute(QStringLiteral("room.list"), {{QStringLiteral("offset"), 0}});
}

void SessionController::onResponse(const QString &id, const QString &type, bool ok,
                                   const QJsonObject &data, const QString &error)
{
    if (id != m_pending) return;
    m_pending.clear();
    if (!ok) {
        m_model->setBusy(false, error);
        return;
    }
    if (type == QStringLiteral("room.list")) {
        if (!data.value(QStringLiteral("rooms")).isArray()) {
            m_signal->disconnectFromServer(tr("房间列表格式无效。"));
            return;
        }
        const auto page = data.value(QStringLiteral("rooms")).toArray();
        for (const auto &room : page) {
            if (!validRoom(room.toObject()) || m_listing.size() >= 128) {
                m_signal->disconnectFromServer(tr("房间列表内容无效或超限。"));
                return;
            }
            m_listing.append(room);
        }
        const auto next = data.value(QStringLiteral("nextOffset"));
        if (!next.isNull()) {
            const double offset = next.toDouble(-1);
            if (!next.isDouble() || !std::isfinite(offset) || std::floor(offset) != offset
                || offset <= m_offset || offset > 128 || page.isEmpty()) {
                m_signal->disconnectFromServer(tr("房间列表分页无效。"));
                return;
            }
            m_offset = int(offset);
            execute(type, {{QStringLiteral("offset"), m_offset}});
            return;
        }
        m_model->setRooms(m_listing);
        m_model->setBusy(false, tr("已刷新 %1 个房间。未开播的房间可以先加入等待。").arg(m_listing.size()));
    } else if (type == QStringLiteral("room.leave")) {
        m_model->setRoom({}, tr("已退出房间。"));
        m_model->setBusy(false, tr("已退出房间。"));
        if (m_view->role() == ClientRole::Player) refreshRooms();
    } else {
        if (!validRoom(data)) {
            m_signal->disconnectFromServer(tr("房间响应格式无效。"));
            return;
        }
        // 广播发送失败时，服务器可能先通知关闭、再返回加入响应。
        // 不能用后来的响应恢复一个已经关闭的房间。
        if (data.value(QStringLiteral("roomId")).toString() == m_closedRoom) {
            m_model->setBusy(false, tr("房间已关闭，请刷新后重新选择。"));
            return;
        }
        m_model->setRoom(data, tr("房间已就绪；媒体链路待接入。"));
        m_model->setBusy(false, tr("房间已就绪；媒体链路待接入。"));
    }
}

void SessionController::onEvent(const QString &event, const QJsonObject &data)
{
    if (event == QStringLiteral("room.closed")) m_closedRoom = data.value(QStringLiteral("roomId")).toString();
    if (data.value(QStringLiteral("roomId")) != m_model->room().value(QStringLiteral("roomId"))) return;
    if (event == QStringLiteral("room.closed")) {
        auto rooms = m_model->rooms();
        for (qsizetype index = rooms.size(); index > 0; --index)
            if (rooms.at(index - 1).toObject().value(QStringLiteral("roomId")) == data.value(QStringLiteral("roomId"))) rooms.removeAt(index - 1);
        m_model->setRooms(rooms);
        m_model->setRoom({}, tr("主播已关闭房间。可刷新列表重新选择。"));
    } else if ((event == QStringLiteral("live.changed") || event == QStringLiteral("room.updated")) && validRoom(data)) {
        m_model->setRoom(data, data.value(QStringLiteral("streaming")).toBool() ? tr("直播中；媒体链路待接入。") : tr("房间已就绪，尚未开播；媒体链路待接入。"));
    }
}

void SessionController::refreshView()
{
    m_view->applySessionState(m_model->ready(), m_model->busy(), m_model->connecting(),
                             m_model->message(), m_model->room(), m_model->rooms());
}
}
