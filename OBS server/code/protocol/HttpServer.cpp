#include "HttpServer.h"
#include <stdexcept>

namespace obs::protocol {
namespace {
HttpServer::AsyncHandler adapt(HttpServer::Handler handler) {
    if (!handler)
        throw std::invalid_argument("invalid HTTP handler");
    return [handler = std::move(handler)](const HttpRequest &request, HttpServer::Reply reply) {
        const auto response = handler(request);
        reply(HttpReply::json(response.first, response.second));
    };
}
} // namespace
Json errorBody(const std::string &code, const std::string &message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}
HttpReply HttpReply::json(int status, const Json &body) {
    return {status, body.dump(), "application/json; charset=utf-8"};
}
HttpServer::HttpServer(net::EventLoop &loop, std::string address, std::uint16_t port,
                       Handler handler, std::chrono::milliseconds timeout)
    : HttpServer(loop, std::move(address), port, adapt(std::move(handler)), timeout) {
}
HttpServer::HttpServer(net::EventLoop &loop, std::string address, std::uint16_t port,
                       AsyncHandler handler, std::chrono::milliseconds timeout)
    : m_loop(loop), m_server(loop, std::move(address), port), m_handler(std::move(handler)),
      m_timeout(timeout) {
    if (!m_handler || timeout.count() <= 0)
        throw std::invalid_argument("invalid HTTP handler or timeout");
    m_server.setConnectionCallback([this](const auto &connection) {
        if (m_requests.size() >= 1024) {
            connection->forceClose();
            return;
        }
        const std::weak_ptr<net::TcpConnection> weak = connection;
        auto timer = m_loop.runAfter(m_timeout, [weak] {
            if (auto active = weak.lock())
                active->forceClose();
        });
        m_requests.emplace(connection.get(), RequestState{timer, false, false});
    });
    m_server.setMessageCallback(
        [this](const auto &connection, auto &input) { receive(connection, input); });
    m_server.setDisconnectCallback([this](const auto &connection, const auto &) {
        const auto it = m_requests.find(connection.get());
        if (it != m_requests.end()) {
            m_loop.cancelTimer(it->second.timer);
            m_requests.erase(it);
        }
    });
    // 接入错误由启动器终止并报告，避免服务显示存活却已停止监听。
    m_server.setErrorCallback([](int) { throw std::runtime_error("HTTP accept failed"); });
}
HttpServer::~HttpServer() {
    stop();
}
void HttpServer::start() {
    if (!m_alive)
        m_alive = std::make_shared<int>(0);
    m_server.start();
}
void HttpServer::stop() {
    m_alive.reset();
    for (auto &[connection, request] : m_requests) {
        (void)connection;
        m_loop.cancelTimer(request.timer);
    }
    m_requests.clear();
    m_server.stop();
}
std::uint16_t HttpServer::port() const {
    return m_server.port();
}
std::size_t HttpServer::connectionCount() const {
    return m_server.connectionCount();
}
void HttpServer::receive(const net::TcpConnection::Ptr &connection, net::Buffer &input) {
    const auto found = m_requests.find(connection.get());
    if (found == m_requests.end() || found->second.dispatched || found->second.responded) {
        input.retrieveAll();
        return;
    }
    HttpRequest request;
    const int result = HttpParser::parse(input, request);
    if (result == 0)
        return;
    if (result != 200) {
        respond(connection,
                HttpReply::json(result, errorBody("INVALID_HTTP", "请求格式或大小不符合要求")));
        return;
    }
    // 1. 分发即占用此请求；ICE 等异步任务不能阻塞 epoll，也不能重复分发。
    found->second.dispatched = true;
    const std::weak_ptr<int> alive = m_alive;
    const std::weak_ptr<net::TcpConnection> weak = connection;
    Reply reply = [this, alive, weak](HttpReply response) {
        if (alive.expired())
            return;
        m_loop.assertInLoopThread();
        if (auto active = weak.lock())
            respond(active, std::move(response));
    };
    try {
        m_handler(request, std::move(reply));
    } catch (const Json::exception &) {
        respond(connection,
                HttpReply::json(400, errorBody("INVALID_JSON", "JSON 字段类型或格式错误")));
    } catch (const std::exception &) {
        // 不输出请求体和异常携带的数据，避免把密码或令牌写入日志。
        respond(connection, HttpReply::json(500, errorBody("INTERNAL_ERROR", "服务处理失败")));
    }
}
void HttpServer::respond(const net::TcpConnection::Ptr &connection, HttpReply response) {
    auto it = m_requests.find(connection.get());
    if (it == m_requests.end() || it->second.responded)
        return;
    const auto &body = response.body;
    const auto status = response.status;
    const std::string reason = status == 200 ? "OK" : "Error";
    const auto wire = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                      "\r\nContent-Type: " + response.contentType +
                      "\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
    it->second.responded = true;
    m_loop.cancelTimer(it->second.timer);
    const std::weak_ptr<net::TcpConnection> weak = connection;
    it->second.timer = m_loop.runAfter(std::chrono::seconds(1), [weak] {
        if (auto active = weak.lock())
            active->forceClose();
    });
    connection->send(wire);
    connection->shutdown(); // 排空后 SHUT_WR；兜底定时器释放不主动断开的对端。
}
} // namespace obs::protocol
