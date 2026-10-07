#pragma once
#include "Buffer.h"
#include "UniqueFd.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace obs::net {
class EventLoop;
class Channel;
// 一个已接入 TCP 连接。状态和缓冲只在 EventLoop 线程访问。
// send/shutdown/forceClose 可跨线程：用 shared_ptr 保活后投递到归属线程。
class TcpConnection final : public std::enable_shared_from_this<TcpConnection> {
  public:
    using Ptr = std::shared_ptr<TcpConnection>;
    using MessageCallback = std::function<void(const Ptr &, Buffer &)>;
    using CloseCallback = std::function<void(const Ptr &, const std::string &)>;
    enum class State { Connecting, Connected, Disconnecting, Disconnected };
    TcpConnection(EventLoop &loop, UniqueFd socket);
    ~TcpConnection();
    void setMessageCallback(MessageCallback callback);
    void setCloseCallback(CloseCallback callback);
    void establish();
    void send(std::string_view bytes);
    void shutdown();
    void forceClose();
    State state() const;
    EventLoop &loop() const noexcept {
        return m_loop;
    }
    std::size_t pendingBytes() const;

  private:
    void sendInLoop(const std::string &bytes);
    void shutdownInLoop();
    void handleRead();
    void handleWrite();
    void handleClose(const std::string &reason);
    EventLoop &m_loop;
    UniqueFd m_socket;
    std::shared_ptr<Channel> m_channel;
    State m_state = State::Connecting;
    Buffer m_input, m_output;
    bool m_peerEof = false;
    MessageCallback m_messageCallback;
    CloseCallback m_closeCallback;
};
} // namespace obs::net
