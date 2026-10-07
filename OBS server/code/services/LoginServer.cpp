#include "LoginServer.h"

namespace obs::services {
using protocol::errorBody;
using protocol::Json;
LoginServer::LoginServer(net::EventLoop &loop, const std::string &address, std::uint16_t port,
                         AuthService &auth)
    : m_auth(auth),
      m_http(loop, address, port, [this](const auto &request) { return handle(request); }) {
}
void LoginServer::start() {
    m_http.start();
}
protocol::HttpResponse LoginServer::handle(const protocol::HttpRequest &request) {
    // health 不走登录限流：错误密码洪峰不应让调度器把仍可工作的节点判为离线。
    if (request.method == "GET" && request.target == "/health")
        return {200,
                {{"service", "login"}, {"ready", true}, {"connections", m_http.connectionCount()}}};
    if (request.target != "/auth/login")
        return {404, errorBody("NOT_FOUND", "接口不存在")};
    if (request.method != "POST")
        return {405, errorBody("METHOD_NOT_ALLOWED", "登录需要 POST 请求")};
    const auto contentType = request.headers.find("content-type");
    if (contentType == request.headers.end() ||
        (contentType->second != "application/json" &&
         contentType->second.rfind("application/json;", 0) != 0))
        return {415, errorBody("CONTENT_TYPE", "需要 application/json")};
    const auto now = std::chrono::steady_clock::now();
    if (now - m_window >= std::chrono::seconds(1)) {
        m_window = now;
        m_attempts = 0;
    }
    // 开发阶段采用全节点固定窗口上限；后续引入真实账号存储时再加来源/账号维度。
    if (++m_attempts > 100)
        return {429, errorBody("RATE_LIMITED", "请求过于频繁")};
    const auto body = Json::parse(request.body);
    const auto account = body.at("account").get<std::string>();
    const auto password = body.at("password").get<std::string>();
    if (account.empty() || account.size() > 64 || password.empty() || password.size() > 128)
        return {400, errorBody("INVALID_FIELDS", "账号或密码长度不合法")};
    if (!m_auth.checkPassword(account, password))
        return {401, errorBody("INVALID_CREDENTIALS", "账号或密码错误")};
    return {200,
            {{"accessToken", m_auth.issue("root")},
             {"expiresIn", m_auth.ttlSeconds()},
             {"user", {{"id", "root"}, {"displayName", "root"}}}}};
}
} // namespace obs::services
