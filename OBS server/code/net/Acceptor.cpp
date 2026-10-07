#include "Acceptor.h"
#include "Channel.h"
#include "EventLoop.h"
#include <arpa/inet.h>
#include <cerrno>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace obs::net {
// 监听 fd 由 Acceptor 独占。构造只 bind，start 才 listen 并注册事件，
// 因而端口冲突等错误会在服务启动阶段暴露，不会藏到第一次连接之后。
Acceptor::Acceptor(EventLoop &loop, const std::string &address, std::uint16_t port)
    : m_loop(loop), m_socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
    loop.assertInLoopThread();
    if (m_socket.get() < 0)
        throw std::system_error(errno, std::generic_category(), "socket");
    const int reuse = 1;
    // 允许进程重启后重新绑定地址；不启用 SO_REUSEPORT，避免意外启动多个监听者。
    if (::setsockopt(m_socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
        throw std::system_error(errno, std::generic_category(), "SO_REUSEADDR");
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1)
        throw std::invalid_argument("listen address must be an IPv4 literal");
    if (::bind(m_socket.get(), reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0)
        throw std::system_error(errno, std::generic_category(), "bind");
    socklen_t length = sizeof(local);
    if (::getsockname(m_socket.get(), reinterpret_cast<sockaddr *>(&local), &length) < 0)
        throw std::system_error(errno, std::generic_category(), "getsockname");
    m_port = ntohs(local.sin_port); // 允许端口 0，由系统分配测试端口。
    m_channel = std::make_shared<Channel>(loop, m_socket.get());
    m_channel->setReadCallback([this] { handleRead(); });
}
Acceptor::~Acceptor() {
    stop();
}
void Acceptor::setAcceptCallback(AcceptCallback cb) {
    m_loop.assertInLoopThread();
    m_acceptCallback = std::move(cb);
}
void Acceptor::setErrorCallback(ErrorCallback cb) {
    m_loop.assertInLoopThread();
    m_errorCallback = std::move(cb);
}
void Acceptor::start() {
    // Channel 不持有监听器。tie 让一次事件处理期间的 shared_ptr 保活监听器。
    m_loop.assertInLoopThread();
    if (::listen(m_socket.get(), SOMAXCONN) < 0)
        throw std::system_error(errno, std::generic_category(), "listen");
    m_channel->tie(shared_from_this());
    m_channel->enableReading();
}
void Acceptor::stop() {
    // 先移除关注再关闭 fd，避免 epoll 的旧事件误命中系统刚复用的描述符。
    m_loop.assertInLoopThread();
    if (m_channel)
        m_channel->disableAll();
    m_socket.reset();
    m_acceptCallback = {};
}
void Acceptor::handleRead() {
    // 每轮最多接入 64 个，LT 会继续通知剩余连接，避免接入洪峰饿死既有连接。
    for (int i = 0; i < 64; ++i) {
        UniqueFd client(::accept4(m_socket.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (client.get() >= 0) {
            // UniqueFd 随回调转移；未设置回调或回调抛异常时自动关闭，避免漏 fd。
            const auto callback = m_acceptCallback;
            if (callback)
                callback(std::move(client));
            // 连接回调可以停止 Server；这时不能继续使用已关闭的监听 fd。
            if (m_socket.get() < 0)
                break;
            continue;
        }
        const int error = errno;
        if (error == EINTR || error == ECONNABORTED)
            continue;
        if (error != EAGAIN && error != EWOULDBLOCK) {
            // fd 耗尽等错误不能保持 LT 监听空转。暂停监听并交给上层恢复/停止。
            // 上层可释放资源后 stop/start Server；业务服务器当前选择报告并终止进程。
            m_channel->disableAll();
            const auto callback = m_errorCallback;
            if (callback)
                callback(error);
        }
        break;
    }
}
} // namespace obs::net
