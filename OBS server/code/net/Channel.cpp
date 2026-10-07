#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>
#include <utility>

namespace obs::net {
// Channel 只描述“fd 关注哪些事件、就绪后调用谁”，不负责关闭 fd 或管理业务状态。
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
    // RDHUP 表示对端关闭发送方向，交给读路径先取完尾部数据再处理 EOF。
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
    // 仅输出缓冲非空时关注 EPOLLOUT；长期订阅可写事件会导致空转占用 CPU。
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
    // HUP 与 IN 同时出现时仍先读，保留对端断开前已经发送的数据。
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
