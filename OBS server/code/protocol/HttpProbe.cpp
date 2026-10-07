#include "HttpProbe.h"
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <sys/socket.h>

namespace obs::protocol {
HttpProbe::HttpProbe(net::EventLoop &loop, std::string address, std::uint16_t port,
                     Callback callback)
    : m_loop(loop), m_address(std::move(address)), m_port(port), m_callback(std::move(callback)) {
    loop.assertInLoopThread();
}
HttpProbe::~HttpProbe() {
    stop();
}
void HttpProbe::start() {
    m_loop.assertInLoopThread();
    if (m_channel || m_finished)
        return;
    m_socket.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(m_port);
    if (m_socket.get() < 0 || ::inet_pton(AF_INET, m_address.c_str(), &peer.sin_addr) != 1) {
        finish(false);
        return;
    }
    const int result = ::connect(m_socket.get(), reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
    if (result < 0 && errno != EINPROGRESS) {
        finish(false);
        return;
    }
    m_output = "GET /health HTTP/1.1\r\nHost: " + m_address + ":" + std::to_string(m_port) +
               "\r\nConnection: close\r\n\r\n";
    m_channel = std::make_shared<net::Channel>(m_loop, m_socket.get());
    m_channel->tie(shared_from_this());
    m_channel->setWriteCallback([this] { write(); });
    m_channel->setReadCallback([this] { read(); });
    m_channel->setCloseCallback([this] { finish(false); });
    m_channel->enableReading();
    m_channel->enableWriting();
    const std::weak_ptr<HttpProbe> weak = shared_from_this();
    m_timeout = m_loop.runAfter(std::chrono::milliseconds(800), [weak] {
        if (auto probe = weak.lock())
            probe->finish(false);
    });
}
void HttpProbe::write() {
    // EPOLLOUT 也会通知连接失败，必须先检查 SO_ERROR，不能把“可写”当作连接成功。
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(m_socket.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
        finish(false);
        return;
    }
    while (m_written < m_output.size()) {
        const auto count = ::send(m_socket.get(), m_output.data() + m_written,
                                  m_output.size() - m_written, MSG_NOSIGNAL);
        if (count > 0)
            m_written += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        else {
            finish(false);
            return;
        }
    }
    m_channel->disableWriting();
}
void HttpProbe::read() {
    char bytes[4096];
    for (;;) {
        const auto count = ::recv(m_socket.get(), bytes, sizeof(bytes), 0);
        if (count > 0) {
            m_input.append(bytes, static_cast<std::size_t>(count));
            if (m_input.size() > 16 * 1024) {
                finish(false);
                return;
            }
        } else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        else {
            finish(false);
            return;
        }
        // 只接受本工程健康接口的明确长度响应，既不等待 EOF，也不解析任意网页。
        const auto end = m_input.find("\r\n\r\n");
        if (end == std::string::npos)
            continue;
        if (m_input.rfind("HTTP/1.1 200 ", 0) != 0) {
            finish(false);
            return;
        }
        const auto at = m_input.find("\r\nContent-Length: ");
        if (at == std::string::npos || at >= end) {
            finish(false);
            return;
        }
        const auto begin = at + std::string_view("\r\nContent-Length: ").size();
        const auto lineEnd = m_input.find("\r\n", begin);
        std::size_t bodyLength = 0;
        const auto parsed =
            std::from_chars(m_input.data() + begin, m_input.data() + lineEnd, bodyLength);
        if (parsed.ec != std::errc{} || parsed.ptr != m_input.data() + lineEnd ||
            bodyLength > 8192) {
            finish(false);
            return;
        }
        if (m_input.size() < end + 4 + bodyLength)
            continue;
        try {
            const auto body = Json::parse(m_input.substr(end + 4, bodyLength));
            finish(body.at("service") == "login" && body.at("ready") == true);
        } catch (const Json::exception &) {
            finish(false);
        }
        return;
    }
}
void HttpProbe::stop() {
    m_loop.assertInLoopThread();
    m_finished = true;
    m_loop.cancelTimer(m_timeout);
    if (m_channel)
        m_channel->disableAll();
    m_socket.reset();
    m_callback = {}; // 主动停止（如析构）不再通知已经失效的 Scheduler。
}
void HttpProbe::finish(bool healthy) {
    if (m_finished)
        return;
    const auto callback = m_callback;
    stop();
    if (callback)
        callback(healthy);
}
} // namespace obs::protocol
