#include "EventLoop.h"
#include "Channel.h"
#include <cerrno>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>

namespace obs::net {
EventLoop::EventLoop()
    : m_ownerThread(std::this_thread::get_id()),
      m_wakeupFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (m_wakeupFd.get() < 0)
        throw std::system_error(errno, std::generic_category(), "eventfd");
    m_wakeupChannel = std::make_shared<Channel>(*this, m_wakeupFd.get());
    m_wakeupChannel->setReadCallback([this] { drainWakeup(); });
    m_wakeupChannel->enableReading();
}
EventLoop::~EventLoop() {
    // 生命周期约定：先停止生产任务的线程和 TcpServer，再销毁 loop。
    if (!isInLoopThread() || m_running)
        std::terminate();
    m_wakeupChannel->disableAll();
}
bool EventLoop::isInLoopThread() const noexcept {
    return std::this_thread::get_id() == m_ownerThread;
}
void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread())
        throw std::logic_error("operation must run in EventLoop owner thread");
}
void EventLoop::loop() {
    assertInLoopThread();
    if (m_running)
        throw std::logic_error("EventLoop is already running");
    m_running = true;
    try {
        while (!m_quit.load()) {
            for (auto &[channel, ready] : m_poller.poll(1000))
                channel->handleEvent(ready);
            runPending();
        }
    } catch (...) {
        m_running = false;
        throw;
    }
    m_running = false;
}
void EventLoop::quit() {
    m_quit.store(true);
    wakeup();
}
void EventLoop::runInLoop(Task task) {
    if (isInLoopThread())
        task();
    else
        queueInLoop(std::move(task));
}
void EventLoop::queueInLoop(Task task) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(std::move(task));
    }
    wakeup();
}
void EventLoop::wakeup() noexcept {
    const std::uint64_t value = 1;
    ssize_t result;
    do {
        result = ::write(m_wakeupFd.get(), &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    // EAGAIN 表示计数器已有唤醒积压，不需要重复写，也不能在此阻塞。
}
void EventLoop::drainWakeup() noexcept {
    std::uint64_t value;
    while (::read(m_wakeupFd.get(), &value, sizeof(value)) > 0) {
    }
}
void EventLoop::runPending() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tasks.swap(m_pending);
    }
    // 不持锁执行：回调可以继续 queueInLoop，留到下一轮，避免死锁和递归。
    for (auto &task : tasks)
        task();
}
void EventLoop::updateChannel(const std::shared_ptr<Channel> &channel) {
    assertInLoopThread();
    m_poller.update(channel);
}
void EventLoop::removeChannel(Channel &channel) noexcept {
    if (!isInLoopThread())
        std::terminate();
    m_poller.remove(channel);
}
} // namespace obs::net
