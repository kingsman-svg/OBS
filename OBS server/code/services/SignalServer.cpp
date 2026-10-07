#include "SignalServer.h"
#include <stdexcept>
#include <vector>

namespace obs::services {
using protocol::Json;
namespace {
Json ok(Json data = Json::object()) {
    return {{"ok", true}, {"data", std::move(data)}};
}
Json fail(const std::string &code, const std::string &message) {
    auto result = protocol::errorBody(code, message);
    result["ok"] = false;
    return result;
}
} // namespace
SignalServer::SignalServer(net::EventLoop &loop, const std::string &address, std::uint16_t port,
                           AuthService &auth, std::string rtmpBase, std::string playbackBase,
                           int idleSeconds, int authSeconds)
    : m_loop(loop), m_server(loop, address, port), m_auth(auth), m_rtmpBase(std::move(rtmpBase)),
      m_playbackBase(std::move(playbackBase)), m_idleSeconds(idleSeconds),
      m_authSeconds(authSeconds) {
    if (idleSeconds <= 0 || authSeconds <= 0)
        throw std::invalid_argument("invalid signal timeout");
    if (m_rtmpBase.size() > 256 || m_playbackBase.size() > 256 ||
        m_rtmpBase.rfind("rtmp://", 0) != 0 || m_playbackBase.rfind("http://", 0) != 0)
        throw std::invalid_argument("invalid media base URL");
    for (const auto *base : {&m_rtmpBase, &m_playbackBase})
        for (unsigned char c : *base)
            if (c <= 32 || c == 127 || c == '"' || c == '\\')
                throw std::invalid_argument("invalid media URL character");
    m_server.setConnectionCallback([this](const auto &connection) {
        if (m_sessions.size() >= 1024) {
            connection->forceClose();
            return;
        }
        SignalSession session;
        session.connection = connection;
        m_sessions.emplace(connection.get(), std::move(session));
    });
    m_server.setMessageCallback(
        [this](const auto &connection, auto &input) { receive(connection, input); });
    m_server.setDisconnectCallback([this](const auto &connection, const auto &) {
        leave(connection.get());
        m_sessions.erase(connection.get());
    });
    m_server.setErrorCallback([](int) { throw std::runtime_error("signal accept failed"); });
}
SignalServer::~SignalServer() {
    m_loop.cancelTimer(m_sweepTimer);
    m_server.stop(); // stop 解除业务回调，析构时不广播，避免触发重入。
}
void SignalServer::start() {
    m_server.start();
    m_sweepTimer = m_loop.runEvery(std::chrono::seconds(1), [this] { sweep(); });
}
void SignalServer::send(const net::TcpConnection::Ptr &connection, const Json &message) {
    connection->send(protocol::FrameCodec::encode(message.dump()));
}
void SignalServer::receive(const net::TcpConnection::Ptr &connection, net::Buffer &input) {
    // 限制单批请求数量，避免恶意粘包让一个连接长期独占 Reactor。
    for (unsigned int count = 0; count < 64; ++count) {
        std::string body;
        const auto result = protocol::FrameCodec::next(input, body);
        if (result == protocol::FrameCodec::Result::Incomplete)
            return;
        if (result == protocol::FrameCodec::Result::Invalid) {
            connection->forceClose();
            return;
        }
        Json id = nullptr;
        Json response;
        try {
            const auto request = Json::parse(body);
            id = request.at("id");
            if (!id.is_string() || id.get_ref<const std::string &>().empty() ||
                id.get_ref<const std::string &>().size() > 64) {
                id = nullptr; // 不回显超长/嵌套 id，避免错误响应反而超过帧上限。
                response = fail("INVALID_ID", "id 必须为 1 至 64 字节字符串");
            } else
                response = handle(connection, request);
        } catch (const Json::exception &) {
            id = nullptr;
            response = fail("INVALID_REQUEST", "请求 JSON 或字段类型错误");
        } catch (const std::exception &) {
            response = fail("INTERNAL_ERROR", "信令处理失败");
        }
        response["id"] = id;
        response["type"] = "response";
        send(connection, response);
        // 发送缓冲超限可能同步断开并删除会话，此后不再访问原输入/会话。
        if (connection->state() != net::TcpConnection::State::Connected)
            return;
    }
    if (input.readableBytes() > 0)
        connection->forceClose();
}
Json SignalServer::handle(const net::TcpConnection::Ptr &connection, const Json &request) {
    auto &session = m_sessions.at(connection.get());
    const auto now = std::chrono::steady_clock::now();
    if (now - session.rateWindow >= std::chrono::seconds(1)) {
        session.rateWindow = now;
        session.requests = 0;
    }
    if (++session.requests > 100)
        return fail("RATE_LIMITED", "信令请求过于频繁");
    const auto type = request.at("type").get<std::string>();
    if (type == "auth") {
        if (!session.userId.empty())
            return fail("ALREADY_AUTHENTICATED", "当前连接已经认证");
        std::string user;
        std::int64_t expiry = 0;
        if (!m_auth.verify(request.at("token").get<std::string>(), user, expiry))
            return fail("UNAUTHORIZED", "令牌无效或已过期");
        session.userId = user;
        session.expiresAt = expiry;
        session.lastSeen = now;
        return ok({{"userId", user}, {"heartbeatSeconds", std::max(1, m_idleSeconds / 3)}});
    }
    if (session.userId.empty() || session.expiresAt <= AuthService::unixSeconds())
        return fail("UNAUTHORIZED", "请先认证或重新登录");
    // 未认证请求不延长认证期限；已认证的有效业务请求也视为在线活动。
    session.lastSeen = now;
    if (type == "heartbeat")
        return ok({{"serverTime", AuthService::unixSeconds()}});
    if (type == "room.list") {
        // 限制单页数量，结合标题/URL 上限，保证最坏情况下响应仍小于 64 KiB。
        const auto offsetValue = request.value("offset", Json(0));
        if (!offsetValue.is_number_integer())
            return fail("INVALID_OFFSET", "offset 必须为非负整数");
        const auto offset = offsetValue.get<std::int64_t>();
        if (offset < 0 || offset > 128)
            return fail("INVALID_OFFSET", "offset 超出范围");
        auto rooms = Json::array();
        std::size_t index = 0;
        for (const auto &[id, room] : m_rooms) {
            (void)id;
            if (index++ >= static_cast<std::size_t>(offset) && rooms.size() < 32)
                rooms.push_back(roomData(room));
        }
        const auto next = static_cast<std::size_t>(offset) + rooms.size();
        return ok(
            {{"rooms", rooms}, {"nextOffset", next < m_rooms.size() ? Json(next) : Json(nullptr)}});
    }
    if (type == "room.create") {
        if (!session.roomId.empty())
            return fail("ALREADY_IN_ROOM", "请先离开当前房间");
        const auto title = request.at("title").get<std::string>();
        if (title.empty() || title.size() > 128)
            return fail("INVALID_TITLE", "房间标题需要 1 至 128 字节");
        if (m_rooms.size() >= 128)
            return fail("ROOM_LIMIT", "房间数量已达上限");
        LiveRoom room;
        room.id = AuthService::randomId();
        room.streamKey = AuthService::randomId();
        room.title = title;
        room.owner = connection.get();
        room.members.insert(connection.get());
        session.roomId = room.id;
        const auto data = roomData(room);
        m_rooms.emplace(room.id, std::move(room));
        return ok(data);
    }
    if (type == "room.join") {
        const auto id = request.at("roomId").get<std::string>();
        auto found = m_rooms.find(id);
        if (found == m_rooms.end())
            return fail("ROOM_NOT_FOUND", "房间不存在");
        if (!session.roomId.empty())
            return fail("ALREADY_IN_ROOM", "请先离开当前房间");
        found->second.members.insert(connection.get());
        session.roomId = id;
        return ok(roomData(found->second));
    }
    if (type == "room.leave") {
        leave(connection.get());
        return ok();
    }
    if (type == "live.start" || type == "live.stop") {
        const auto found = m_rooms.find(session.roomId);
        if (found == m_rooms.end())
            return fail("ROOM_NOT_FOUND", "请先创建房间");
        auto &room = found->second;
        // 权限属于创建房间的连接，而非 userId；多个 root 客户端也不能互相停播。
        if (room.owner != connection.get())
            return fail("FORBIDDEN", "只有主播连接可以开播或停播");
        room.streaming = type == "live.start";
        auto data = roomData(room);
        if (room.streaming)
            data["pushUrl"] = m_rtmpBase + "/" + room.streamKey;
        const auto response = ok(data);
        broadcast(room, {{"type", "event"}, {"event", "live.changed"}, {"data", roomData(room)}});
        return response;
    }
    return fail("UNKNOWN_TYPE", "未知信令类型");
}
Json SignalServer::roomData(const LiveRoom &room) const {
    // streamKey 只用于后续媒体链路；对观众返回拉流地址，不返回主播的推流字段。
    return {{"roomId", room.id},
            {"title", room.title},
            {"streaming", room.streaming},
            {"viewers", room.members.size() - 1},
            {"pullUrl", m_rtmpBase + "/" + room.streamKey},
            {"playbackUrl", m_playbackBase + "/" + room.streamKey + ".flv"}};
}
void SignalServer::broadcast(const LiveRoom &room, const Json &message) {
    std::vector<net::TcpConnection::Ptr> recipients;
    for (auto *member : room.members) {
        const auto it = m_sessions.find(member);
        if (it != m_sessions.end())
            if (auto connection = it->second.connection.lock())
                recipients.push_back(std::move(connection));
    }
    // send 可能因背压关闭连接并触发 leave；先复制接收者，避免迭代器失效。
    for (const auto &connection : recipients)
        send(connection, message);
}
void SignalServer::leave(net::TcpConnection *connection) {
    const auto session = m_sessions.find(connection);
    if (session == m_sessions.end())
        return;
    const auto found = m_rooms.find(session->second.roomId);
    session->second.roomId.clear();
    if (found == m_rooms.end())
        return;
    if (found->second.owner != connection) {
        found->second.members.erase(connection);
        return;
    }
    auto room = std::move(found->second);
    m_rooms.erase(found); // 先完成状态修改，再向观众通知，允许发送失败导致重入。
    room.members.erase(connection);
    for (auto *member : room.members) {
        const auto it = m_sessions.find(member);
        if (it != m_sessions.end())
            it->second.roomId.clear();
    }
    broadcast(room, {{"type", "event"}, {"event", "room.closed"}, {"data", {{"roomId", room.id}}}});
}
void SignalServer::sweep() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<net::TcpConnection::Ptr> expired;
    for (const auto &[pointer, session] : m_sessions) {
        (void)pointer;
        const auto limit =
            std::chrono::seconds(session.userId.empty() ? m_authSeconds : m_idleSeconds);
        if (now - session.lastSeen >= limit ||
            (!session.userId.empty() && session.expiresAt <= AuthService::unixSeconds()))
            if (auto connection = session.connection.lock())
                expired.push_back(std::move(connection));
    }
    for (const auto &connection : expired)
        connection->forceClose(); // 复用断开路径，保证超时也会清理房间并通知观众。
}
} // namespace obs::services
