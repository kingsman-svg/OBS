#pragma once
#include <unistd.h>
#include <utility>

namespace obs::net {
// Linux 文件描述符的唯一所有者。移动即转移所有权，避免失败分支漏关或重复关闭。
// close() 被 EINTR 中断时不重试：Linux 可能已释放 fd，重试可能误关复用后的 fd。
class UniqueFd final {
  public:
    explicit UniqueFd(int fd = -1) noexcept : m_fd(fd) {
    }
    ~UniqueFd() {
        reset();
    }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&other) noexcept : m_fd(other.release()) {
    }
    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }
    int get() const noexcept {
        return m_fd;
    }
    int release() noexcept {
        return std::exchange(m_fd, -1);
    }
    void reset(int fd = -1) noexcept {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = fd;
    }

  private:
    int m_fd;
};
} // namespace obs::net
