#pragma once
#include "UniqueFd.h"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace obs::net {
class Channel;
// epoll 适配层：使用 LT 水平触发，不把 epoll 细节传播到业务服务。
// 每次注册使用新的 token，而非裸 fd / 裸指针，避免 fd 复用和悬空事件。
class Poller final {
  public:
    using ActiveEvents = std::vector<std::pair<std::shared_ptr<Channel>, std::uint32_t>>;
    Poller();
    ActiveEvents poll(int timeoutMs);
    void update(const std::shared_ptr<Channel> &channel);
    void remove(Channel &channel) noexcept;

  private:
    UniqueFd m_epoll;
    std::uint64_t m_nextToken = 1;
    std::unordered_map<std::uint64_t, std::weak_ptr<Channel>> m_channels;
};
} // namespace obs::net
