#include "TimerQueue.h"
#include "Channel.h"
#include "EventLoop.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <sys/timerfd.h>
#include <system_error>
#include <vector>

namespace obs::net {
TimerQueue::TimerQueue(EventLoop &loop)
    : m_loop(loop), m_fd(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) {
    loop.assertInLoopThread();
    if (m_fd.get() < 0)
        throw std::system_error(errno, std::generic_category(), "timerfd_create");
    m_channel = std::make_shared<Channel>(loop, m_fd.get());
    m_channel->setReadCallback([this] { handleRead(); });
    m_channel->enableReading();
}
TimerQueue::~TimerQueue() {
    m_loop.assertInLoopThread();
    m_channel->disableAll(); // 先撤销 epoll 订阅，再由 UniqueFd 关闭 fd。
    for (auto &[deadline, entry] : m_timers) {
        (void)deadline;
        std::get<2>(entry)->store(true);
    }
}
TimerId TimerQueue::add(Duration delay, Duration interval, Task task) {
    // 限制到一年，避免 time_point 加法溢出；零延迟表示下一次事件分发。
    const auto maximum = std::chrono::hours(24 * 365);
    if (!task || delay < Duration::zero() || interval < Duration::zero() || delay > maximum ||
        interval > maximum)
        throw std::invalid_argument("invalid timer duration or callback");
    auto id = std::make_shared<std::atomic<bool>>(false);
    const auto deadline = Clock::now() + delay;
    m_loop.runInLoop([this, deadline, interval, task = std::move(task), id]() mutable {
        // 跨线程 add 后立即 cancel 时，插入任务可能尚未执行；标记保证不会复活。
        if (id->load())
            return;
        m_timers.emplace(deadline, Entry{std::move(task), interval, id});
        arm();
    });
    return id;
}
void TimerQueue::cancel(const TimerId &id) {
    if (!id)
        return;
    id->store(true); // 先设置标记，同批次尚未执行的到期回调也会跳过。
    m_loop.runInLoop([this] {
        removeCancelled();
        arm();
    });
}
void TimerQueue::removeCancelled() {
    for (auto it = m_timers.begin(); it != m_timers.end();) {
        if (std::get<2>(it->second)->load())
            it = m_timers.erase(it);
        else
            ++it;
    }
}
void TimerQueue::arm() {
    itimerspec next{};
    if (!m_timers.empty()) {
        // timerfd 的全零值表示撤防，已经到期的任务要用至少 1ns 才能触发。
        const auto delta =
            std::max(Duration(1),
                     std::chrono::duration_cast<Duration>(m_timers.begin()->first - Clock::now()));
        next.it_value.tv_sec = delta.count() / 1000000000;
        next.it_value.tv_nsec = delta.count() % 1000000000;
    }
    if (::timerfd_settime(m_fd.get(), 0, &next, nullptr) < 0)
        throw std::system_error(errno, std::generic_category(), "timerfd_settime");
}
void TimerQueue::handleRead() {
    std::uint64_t expirations = 0;
    ssize_t count;
    do {
        count = ::read(m_fd.get(), &expirations, sizeof(expirations));
    } while (count < 0 && errno == EINTR);
    if (count < 0 && errno != EAGAIN)
        throw std::system_error(errno, std::generic_category(), "read timerfd");

    // 先摘出本批次，再调用业务：回调允许新增定时器或取消自己/同批其他任务。
    std::vector<Entry> ready;
    const auto now = Clock::now();
    while (!m_timers.empty() && m_timers.begin()->first <= now) {
        ready.push_back(std::move(m_timers.begin()->second));
        m_timers.erase(m_timers.begin());
    }
    try {
        for (auto &[task, interval, id] : ready) {
            if (id->load())
                continue;
            task();
            // 固定延迟重复：从回调完成时重新计时，不补发阻塞期间错过的次数。
            if (interval > Duration::zero() && !id->load())
                m_timers.emplace(Clock::now() + interval, Entry{task, interval, id});
            else
                id->store(true);
        }
    } catch (...) {
        // 异常按 EventLoop 约定继续向上传播，未重排的本批句柄全部结束。
        for (auto &entry : ready)
            std::get<2>(entry)->store(true);
        removeCancelled();
        arm();
        throw;
    }
    removeCancelled();
    arm();
}
} // namespace obs::net
