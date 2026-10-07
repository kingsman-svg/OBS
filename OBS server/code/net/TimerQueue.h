#pragma once
#include "UniqueFd.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <tuple>

namespace obs::net {
class EventLoop;
class Channel;
// 句柄只保存取消标记，不拥有队列。取消是幂等的，不能中断已经开始执行的回调。
using TimerId = std::shared_ptr<std::atomic<bool>>;

// 一个 loop 共用一个 CLOCK_MONOTONIC timerfd；所有容器操作都在 loop 线程。
class TimerQueue final {
  public:
    using Duration = std::chrono::nanoseconds;
    using Task = std::function<void()>;
    explicit TimerQueue(EventLoop &loop);
    ~TimerQueue();
    TimerId add(Duration delay, Duration interval, Task task);
    void cancel(const TimerId &id);

  private:
    using Clock = std::chrono::steady_clock;
    using Entry = std::tuple<Task, Duration, TimerId>;
    void handleRead();
    void removeCancelled();
    void arm();
    EventLoop &m_loop;
    UniqueFd m_fd;
    std::shared_ptr<Channel> m_channel;
    std::multimap<Clock::time_point, Entry> m_timers;
};
} // namespace obs::net
