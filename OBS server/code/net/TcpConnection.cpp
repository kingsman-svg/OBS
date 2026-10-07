#include "TcpConnection.h"
#include "Channel.h"
#include "EventLoop.h"
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

namespace obs::net {
// 状态流转：Connecting -> Connected -> Disconnecting -> Disconnected。
// 正常关闭尽量排空输出；协议错误、超限或强制关闭则立即释放 fd 和缓冲。
TcpConnection::TcpConnection(EventLoop &loop, UniqueFd socket)
    : m_loop(loop), m_socket(std::move(socket)),
      m_channel(std::make_shared<Channel>(loop, m_socket.get())) {
    loop.assertInLoopThread();
    if (m_socket.get() < 0)
        throw std::invalid_argument("invalid connection socket");
    const int on = 1;
    // 控制消息通常很小，关闭 Nagle 以降低交互延迟；不改变 TCP 消息边界语义。
    ::setsockopt(m_socket.get(), IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    m_channel->setReadCallback([this] { handleRead(); });
    m_channel->setWriteCallback([this] { handleWrite(); });
    m_channel->setCloseCallback([this] { handleClose("socket error or hangup"); });
}
TcpConnection::~TcpConnection() {
    // Server 先断开、从 epoll 移除后才释放；外部可以继续持有已关闭连接的 shared_ptr。
    if (m_state != State::Disconnected && m_state != State::Connecting)
        std::terminate();
}
void TcpConnection::setMessageCallback(MessageCallback cb) {
    m_loop.assertInLoopThread();
    m_messageCallback = std::move(cb);
}
void TcpConnection::setCloseCallback(CloseCallback cb) {
    m_loop.assertInLoopThread();
    m_closeCallback = std::move(cb);
}
TcpConnection::State TcpConnection::state() const {
    m_loop.assertInLoopThread();
    return m_state;
}
std::size_t TcpConnection::pendingBytes() const {
    m_loop.assertInLoopThread();
    return m_output.readableBytes();
}
void TcpConnection::establish() {
    // 必须在 make_shared 完成之后调用，构造函数内不能使用 shared_from_this。
    m_loop.assertInLoopThread();
    if (m_state != State::Connecting)
        throw std::logic_error("connection already established");
    m_channel->tie(shared_from_this());
    m_channel->enableReading();
    m_state = State::Connected;
}
void TcpConnection::send(std::string_view bytes) {
    // 立即复制调用方数据，异步执行时不借用临时字符串 / Buffer 内部地址。
    auto self = shared_from_this();
    m_loop.runInLoop([self, data = std::string(bytes)] { self->sendInLoop(data); });
}
void TcpConnection::sendInLoop(const std::string &bytes) {
    if (m_state != State::Connected || bytes.empty())
        return;
    if (bytes.size() > Buffer::MaxBytes - m_output.readableBytes()) {
        handleClose("output buffer exceeds 8 MiB");
        return;
    }
    // 先尝试直接发送，部分写或 EAGAIN 的剩余字节进入输出缓冲。
    ssize_t written = 0;
    if (!m_channel->isWriting() && m_output.readableBytes() == 0) {
        do {
            written = ::send(m_socket.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        if (written < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                handleClose("send failed");
                return;
            }
            written = 0;
        }
    }
    if (static_cast<std::size_t>(written) < bytes.size()) {
        m_output.append(std::string_view(bytes).substr(static_cast<std::size_t>(written)));
        m_channel->enableWriting();
    }
}
void TcpConnection::handleWrite() {
    // 每轮最多写 256 KiB，缓冲未排空时保持 EPOLLOUT，兼顾多个连接公平性。
    std::size_t budget = 256 * 1024;
    while (m_output.readableBytes() > 0 && budget > 0) {
        const auto bytes = m_output.view().substr(0, budget);
        const auto count = ::send(m_socket.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count > 0) {
            m_output.retrieve(static_cast<std::size_t>(count));
            budget -= static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        else {
            handleClose("buffered send failed");
            return;
        }
    }
    if (m_output.readableBytes() == 0) {
        m_channel->disableWriting();
        if (m_peerEof)
            handleClose("peer EOF after output drained");
        else if (m_state == State::Disconnecting)
            shutdownInLoop();
    }
}
void TcpConnection::handleRead() {
    // 一轮读到 EAGAIN 或预算耗尽，再把累计字节交给协议层；协议层自行保留半包。
    char bytes[16384];
    std::size_t total = 0;
    bool eof = false;
    while (total < 256 * 1024) {
        const auto count = ::recv(m_socket.get(), bytes, sizeof(bytes), 0);
        if (count > 0) {
            const auto size = static_cast<std::size_t>(count);
            if (size > Buffer::MaxBytes - m_input.readableBytes()) {
                handleClose("input buffer exceeds 8 MiB");
                return;
            }
            m_input.append(std::string_view(bytes, size));
            total += size;
        } else if (count == 0) {
            eof = true;
            break;
        } else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        else {
            handleClose("recv failed");
            return;
        }
    }
    // 先通知已经接收到的字节，再处理 EOF，避免最后一批请求被丢弃。
    const auto callback = m_messageCallback;
    if (total > 0 && callback)
        callback(shared_from_this(), m_input);
    if (eof && m_state != State::Disconnected) {
        m_peerEof = true;
        m_state = State::Disconnecting;
        m_channel->disableReading();
        // 对端 shutdown(SHUT_WR) 后仍可接收响应，待输出缓冲排空才真正关闭。
        if (m_output.readableBytes() == 0)
            handleClose("peer EOF");
    }
}
void TcpConnection::shutdown() {
    auto self = shared_from_this();
    m_loop.runInLoop([self] {
        if (self->m_state == State::Connected) {
            self->m_state = State::Disconnecting;
            self->shutdownInLoop();
        }
    });
}
void TcpConnection::shutdownInLoop() {
    // SHUT_WR 只关闭发送方向。对端不回应的等待上限由 HTTP/信令业务定时器控制。
    if (m_output.readableBytes() == 0)
        ::shutdown(m_socket.get(), SHUT_WR);
}
void TcpConnection::forceClose() {
    auto self = shared_from_this();
    m_loop.runInLoop([self] { self->handleClose("force closed"); });
}
void TcpConnection::handleClose(const std::string &reason) {
    // 关闭入口统一且幂等。先更新状态并移除事件，再通知业务，允许回调释放 Server。
    if (m_state == State::Disconnected)
        return;
    const auto guard = shared_from_this();
    m_state = State::Disconnected;
    m_channel->disableAll();
    m_socket.reset();
    m_input.retrieveAll();
    m_output.retrieveAll();
    const auto callback = m_closeCallback;
    if (callback)
        callback(guard, reason);
}
} // namespace obs::net
