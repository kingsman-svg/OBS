#pragma once
#include "EventLoop.h"
#include "HttpParser.h"
#include "TcpServer.h"
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace obs::protocol {
using Json = nlohmann::json;
using HttpResponse = std::pair<int, Json>;
Json errorBody(const std::string &code, const std::string &message);

// 负责请求边界、超时与响应发送；路由和身份校验交给业务 Handler。
class HttpServer final {
  public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;
    HttpServer(net::EventLoop &loop, std::string address, std::uint16_t port, Handler handler,
               std::chrono::milliseconds timeout = std::chrono::seconds(5));
    ~HttpServer();
    void start();
    void stop();
    std::uint16_t port() const;
    std::size_t connectionCount() const;

  private:
    void receive(const net::TcpConnection::Ptr &connection, net::Buffer &input);
    void respond(const net::TcpConnection::Ptr &connection, HttpResponse response);
    net::EventLoop &m_loop;
    net::TcpServer m_server;
    Handler m_handler;
    std::chrono::milliseconds m_timeout;
    // 每连接一个绝对请求期限；收到零碎字节不会续期，防止慢速请求长期占用资源。
    std::unordered_map<net::TcpConnection *, std::pair<net::TimerId, bool>> m_requests;
};
} // namespace obs::protocol
