#pragma once
#include "Poller.h"
#include "TimerQueue.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace obs::net {
// 一个线程一个 EventLoop；在执行 loop() 的同一线程构造并销毁。
// I/O 回调、连接状态和任务均在归属线程执行，不在回调中阻塞等待其他线程。
class EventLoop final {
  public:
    using Task = std::function<void()>;
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;
    void loop();
    void quit();                 // 可跨线程，唤醒 epoll 并请求停止。
    void runInLoop(Task task);   // 本线程直接执行，其他线程排队。
    void queueInLoop(Task task); // 总是排队，适合延迟释放/跨线程调度。
    TimerId runAfter(TimerQueue::Duration delay, Task task);
    TimerId runEvery(TimerQueue::Duration interval, Task task);
    void cancelTimer(const TimerId &id);
    bool isInLoopThread() const noexcept;
    void assertInLoopThread() const;
    void updateChannel(const std::shared_ptr<Channel> &channel);
    void removeChannel(Channel &channel) noexcept;

  private:
    void wakeup() noexcept;
    void drainWakeup() noexcept;
    void runPending();
    const std::thread::id m_ownerThread;
    Poller m_poller;
    UniqueFd m_wakeupFd;
    std::shared_ptr<Channel> m_wakeupChannel;
    std::unique_ptr<TimerQueue> m_timers;
    std::atomic<bool> m_quit{false};
    bool m_running = false;
    std::mutex m_mutex;
    std::vector<Task> m_pending;
};
} // namespace obs::net
