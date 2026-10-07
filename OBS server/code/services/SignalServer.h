#pragma once
#include "AuthService.h"
#include "FrameCodec.h"
#include "HttpServer.h"
#include <map>
#include <set>

namespace obs::services {
struct SignalSession {
    std::weak_ptr<net::TcpConnection> connection;
    std::string userId, roomId;
    std::int64_t expiresAt = 0;
    std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point rateWindow = lastSeen;
    unsigned int requests = 0;
};
struct LiveRoom {
    std::string id, title, streamKey;
    net::TcpConnection *owner = nullptr;
    std::set<net::TcpConnection *> members;
    bool streaming = false;
};

// 只处理控制消息；媒体数据由客户端通过 FFmpeg 与后续部署的媒体服务器交换。
class SignalServer final {
  public:
    SignalServer(net::EventLoop &loop, const std::string &address, std::uint16_t port,
                 AuthService &auth, std::string rtmpBase, std::string playbackBase,
                 int idleSeconds = 45, int authSeconds = 5);
    ~SignalServer();
    void start();

  private:
    void receive(const net::TcpConnection::Ptr &connection, net::Buffer &input);
    protocol::Json handle(const net::TcpConnection::Ptr &connection, const protocol::Json &request);
    void send(const net::TcpConnection::Ptr &connection, const protocol::Json &message);
    void broadcast(const LiveRoom &room, const protocol::Json &message);
    void leave(net::TcpConnection *connection);
    void sweep();
    protocol::Json roomData(const LiveRoom &room) const;
    net::EventLoop &m_loop;
    net::TcpServer m_server;
    AuthService &m_auth;
    std::string m_rtmpBase, m_playbackBase;
    int m_idleSeconds, m_authSeconds;
    net::TimerId m_sweepTimer;
    std::unordered_map<net::TcpConnection *, SignalSession> m_sessions;
    std::map<std::string, LiveRoom> m_rooms;
};
} // namespace obs::services
