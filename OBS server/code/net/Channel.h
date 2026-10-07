#pragma once
#include <cstdint>
#include <functional>
#include <memory>

namespace obs::net {
class EventLoop;
class Poller;
// 一个 fd 的事件订阅与回调集合；不拥有 fd。fd 由 Acceptor/TcpConnection 等持有。
// 所有方法只能在归属的 EventLoop 线程调用。
class Channel final : public std::enable_shared_from_this<Channel> {
  public:
    using Callback = std::function<void()>;
    Channel(EventLoop &loop, int fd);
    int fd() const noexcept {
        return m_fd;
    }
    std::uint32_t events() const noexcept {
        return m_events;
    }
    void setReadCallback(Callback callback);
    void setWriteCallback(Callback callback);
    void setCloseCallback(Callback callback);
    void tie(const std::shared_ptr<void> &owner);
    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();
    bool isWriting() const noexcept;
    void handleEvent(std::uint32_t ready);

  private:
    friend class Poller;
    void update();
    void dispatch(std::uint32_t ready);
    EventLoop &m_loop;
    int m_fd;
    std::uint32_t m_events = 0;
    std::uint64_t m_token = 0;
    std::weak_ptr<void> m_owner;
    bool m_tied = false;
    Callback m_readCallback, m_writeCallback, m_closeCallback;
};
} // namespace obs::net
