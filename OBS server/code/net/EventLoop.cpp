#include "EventLoop.h"
#include "Channel.h"
#include <cerrno>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>

namespace obs::net {
// eventfd 负责跨线程任务唤醒，timerfd 负责期限唤醒，两者都走同一 epoll 分发链路。
EventLoop::EventLoop()
    : m_ownerThread(std::this_thread::get_id()),
      m_wakeupFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (m_wakeupFd.get() < 0)
        throw std::system_error(errno, std::generic_category(), "eventfd");
    m_wakeupChannel = std::make_shared<Channel>(*this, m_wakeupFd.get());
    m_wakeupChannel->setReadCallback([this] { drainWakeup(); });
    m_wakeupChannel->enableReading();
    m_timers = std::make_unique<TimerQueue>(*this);
}
EventLoop::~EventLoop() {
    // 生命周期约定：先停止生产任务的线程和 TcpServer，再销毁 loop。
    if (!isInLoopThread() || m_running)
        std::terminate();
    m_timers.reset();
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
    // 事件和任务串行执行，因此连接/定时器内部无需互斥锁；业务回调不能阻塞。
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
    // 原子标记可跨线程设置；写 eventfd 让阻塞在 epoll_wait 的线程及时退出。
    m_quit.store(true);
    wakeup();
}
void EventLoop::runInLoop(Task task) {
    // 本线程立即执行可减少调度开销，但调用者应考虑回调的同步重入。
    if (isInLoopThread())
        task();
    else
        queueInLoop(std::move(task));
}
void EventLoop::queueInLoop(Task task) {
    // 锁只保护任务容器，执行回调时不持锁；无论调用线程是否归属本 loop 都排队。
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(std::move(task));
    }
    wakeup();
}
TimerId EventLoop::runAfter(TimerQueue::Duration delay, Task task) {
    return m_timers->add(delay, TimerQueue::Duration::zero(), std::move(task));
}
TimerId EventLoop::runEvery(TimerQueue::Duration interval, Task task) {
    if (interval <= TimerQueue::Duration::zero())
        throw std::invalid_argument("repeating timer interval must be positive");
    return m_timers->add(interval, interval, std::move(task));
}
void EventLoop::cancelTimer(const TimerId &id) {
    m_timers->cancel(id);
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
