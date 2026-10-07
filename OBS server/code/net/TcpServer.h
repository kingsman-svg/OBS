#pragma once
#include "TcpConnection.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace obs::net {
class EventLoop;
class Acceptor;
// 服务级连接集合，拥有 Acceptor 和活跃连接，不解析任何业务协议。
// 构造/配置/start/stop/析构都在 loop 线程。跨线程停止应 queueInLoop 调用 stop。
class TcpServer final {
  public:
    using ConnectionCallback = std::function<void(const TcpConnection::Ptr &)>;
    using DisconnectCallback = TcpConnection::CloseCallback;
    using ErrorCallback = std::function<void(int)>;
    TcpServer(EventLoop &loop, std::string address, std::uint16_t port);
    ~TcpServer();
    void setConnectionCallback(ConnectionCallback callback);
    void setMessageCallback(TcpConnection::MessageCallback callback);
    void setDisconnectCallback(DisconnectCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void start();
    void stop();
    std::uint16_t port() const;
    std::size_t connectionCount() const;

  private:
    void onAccept(UniqueFd socket);
    EventLoop &m_loop;
    std::string m_address;
    std::uint16_t m_port;
    std::shared_ptr<Acceptor> m_acceptor;
    std::uint64_t m_nextId = 1;
    std::unordered_map<std::uint64_t, TcpConnection::Ptr> m_connections;
    ConnectionCallback m_connectionCallback;
    TcpConnection::MessageCallback m_messageCallback;
    DisconnectCallback m_disconnectCallback;
    ErrorCallback m_errorCallback;
};
} // namespace obs::net
