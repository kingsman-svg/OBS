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

// 异步接口也可返回 HTML/SDP；原有业务继续使用 HttpResponse，不改变 JSON 协议。
struct HttpReply {
    int status = 200;
    std::string body;
    std::string contentType = "application/json; charset=utf-8";
    static HttpReply json(int status, const Json &body);
};

// 负责请求边界、超时与响应发送；路由和身份校验交给业务 Handler。
class HttpServer final {
  public:
    using Handler = std::function<HttpResponse(const HttpRequest &)>;
    // Reply 必须在本 EventLoop 线程调用；过期/断开/stop 后的回复自动忽略。
    using Reply = std::function<void(HttpReply)>;
    using AsyncHandler = std::function<void(const HttpRequest &, Reply)>;
    HttpServer(net::EventLoop &loop, std::string address, std::uint16_t port, Handler handler,
               std::chrono::milliseconds timeout = std::chrono::seconds(5));
    HttpServer(net::EventLoop &loop, std::string address, std::uint16_t port, AsyncHandler handler,
               std::chrono::milliseconds timeout = std::chrono::seconds(5));
    ~HttpServer();
    void start();
    void stop();
    std::uint16_t port() const;
    std::size_t connectionCount() const;

  private:
    void receive(const net::TcpConnection::Ptr &connection, net::Buffer &input);
    void respond(const net::TcpConnection::Ptr &connection, HttpReply response);
    net::EventLoop &m_loop;
    net::TcpServer m_server;
    AsyncHandler m_handler;
    std::chrono::milliseconds m_timeout;
    // 每连接一个绝对请求期限；收到零碎字节不会续期，防止慢速请求长期占用资源。
    struct RequestState {
        net::TimerId timer;
        bool dispatched = false; // 完整请求只交给业务一次，异步等待时不再解析后续字节。
        bool responded = false; // 第一次回复生效，重复回复不得写第二个 HTTP 响应。
    };
    std::unordered_map<net::TcpConnection *, RequestState> m_requests;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0); // 仅 loop 线程访问的回复存活标记。
};
} // namespace obs::protocol
