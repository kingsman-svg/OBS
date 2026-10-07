#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>
#include <utility>

namespace obs::net {
Channel::Channel(EventLoop &loop, int fd) : m_loop(loop), m_fd(fd) {
}
void Channel::setReadCallback(Callback cb) {
    m_loop.assertInLoopThread();
    m_readCallback = std::move(cb);
}
void Channel::setWriteCallback(Callback cb) {
    m_loop.assertInLoopThread();
    m_writeCallback = std::move(cb);
}
void Channel::setCloseCallback(Callback cb) {
    m_loop.assertInLoopThread();
    m_closeCallback = std::move(cb);
}
void Channel::tie(const std::shared_ptr<void> &owner) {
    m_loop.assertInLoopThread();
    m_owner = owner;
    m_tied = true;
}
void Channel::update() {
    m_loop.updateChannel(shared_from_this());
}
void Channel::enableReading() {
    m_loop.assertInLoopThread();
    m_events |= EPOLLIN | EPOLLRDHUP;
    update();
}
void Channel::disableReading() {
    m_loop.assertInLoopThread();
    m_events &= ~(EPOLLIN | EPOLLRDHUP);
    update();
}
void Channel::enableWriting() {
    m_loop.assertInLoopThread();
    m_events |= EPOLLOUT;
    update();
}
void Channel::disableWriting() {
    m_loop.assertInLoopThread();
    m_events &= ~EPOLLOUT;
    update();
}
void Channel::disableAll() {
    m_loop.assertInLoopThread();
    m_events = 0;
    m_loop.removeChannel(*this);
}
bool Channel::isWriting() const noexcept {
    return (m_events & EPOLLOUT) != 0;
}
void Channel::handleEvent(std::uint32_t ready) {
    m_loop.assertInLoopThread();
    // 活跃事件批次可能还保留已移除的 Channel，关闭后跳过，避免 fd 复用时误分发。
    if (m_events == 0)
        return;
    if (m_tied) {
        const auto guard = m_owner.lock();
        if (!guard)
            return;
        dispatch(ready); // guard 保证连接对象活到整个事件处理结束。
    } else
        dispatch(ready);
}
void Channel::dispatch(std::uint32_t ready) {
    if ((ready & EPOLLERR) || ((ready & EPOLLHUP) && !(ready & EPOLLIN))) {
        if (m_closeCallback)
            m_closeCallback();
        return;
    }
    if ((ready & (EPOLLIN | EPOLLRDHUP)) && m_readCallback)
        m_readCallback();
    // read 回调可能已关闭连接，不能继续执行这个 fd 的 write 回调。
    if (m_events != 0 && (ready & EPOLLOUT) && m_writeCallback)
        m_writeCallback();
}
} // namespace obs::net
