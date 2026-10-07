#pragma once
#include "UniqueFd.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace obs::net {
class EventLoop;
class Channel;
// 监听套接字的所有者。只接收连接，不参与业务协议和连接数据收发。
class Acceptor final : public std::enable_shared_from_this<Acceptor> {
  public:
    using AcceptCallback = std::function<void(UniqueFd)>;
    using ErrorCallback = std::function<void(int)>;
    Acceptor(EventLoop &loop, const std::string &address, std::uint16_t port);
    ~Acceptor();
    void setAcceptCallback(AcceptCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void start();
    void stop();
    std::uint16_t port() const noexcept {
        return m_port;
    }

  private:
    void handleRead();
    EventLoop &m_loop;
    UniqueFd m_socket;
    std::shared_ptr<Channel> m_channel;
    std::uint16_t m_port = 0;
    AcceptCallback m_acceptCallback;
    ErrorCallback m_errorCallback;
};
} // namespace obs::net
