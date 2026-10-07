#include "Poller.h"
#include "Channel.h"
#include <cerrno>
#include <sys/epoll.h>
#include <system_error>

namespace obs::net {
// epoll 实例由 RAII fd 管理；注册表只保留 weak_ptr，不延长业务对象生命周期。
Poller::Poller() : m_epoll(::epoll_create1(EPOLL_CLOEXEC)) {
    if (m_epoll.get() < 0)
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
}
Poller::ActiveEvents Poller::poll(int timeoutMs) {
    // EINTR 当成空批次返回，由 EventLoop 继续处理任务，不把普通信号视为致命错误。
    epoll_event events[64]{};
    const int count = ::epoll_wait(m_epoll.get(), events, 64, timeoutMs);
    if (count < 0 && errno != EINTR)
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    ActiveEvents active;
    for (int i = 0; i < count; ++i) {
        const auto found = m_channels.find(events[i].data.u64);
        if (found != m_channels.end()) {
            // 本批次临时转为 shared_ptr，处理期间即使 Server 删除连接也不会悬空。
            if (auto channel = found->second.lock())
                active.emplace_back(std::move(channel),
                                    static_cast<std::uint32_t>(events[i].events));
        }
    }
    return active;
}
void Poller::update(const std::shared_ptr<Channel> &channel) {
    if (channel->events() == 0) {
        remove(*channel);
        return;
    }
    epoll_event event{};
    event.events = channel->events();
    const bool adding = channel->m_token == 0;
    // 每次新注册分配新 token，不能用 fd 作身份：close 后 fd 很快可能被复用。
    const auto token = adding ? m_nextToken++ : channel->m_token;
    event.data.u64 = token;
    // 先分配 map 节点，再提交 epoll，避免内存分配失败留下未跟踪的注册。
    if (adding)
        m_channels.emplace(token, channel);
    if (::epoll_ctl(m_epoll.get(), adding ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, channel->fd(), &event) <
        0) {
        const int error = errno;
        if (adding)
            m_channels.erase(token);
        throw std::system_error(error, std::generic_category(), "epoll_ctl");
    }
    channel->m_token = token;
}
void Poller::remove(Channel &channel) noexcept {
    if (channel.m_token == 0)
        return;
    // 释放路径不抛异常。即使内核已移除 fd，也必须删除用户态 token。
    ::epoll_ctl(m_epoll.get(), EPOLL_CTL_DEL, channel.fd(), nullptr);
    m_channels.erase(channel.m_token);
    channel.m_token = 0;
}
} // namespace obs::net
