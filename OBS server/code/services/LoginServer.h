#pragma once
#include "AuthService.h"
#include "HttpServer.h"

namespace obs::services {
class LoginServer final {
  public:
    LoginServer(net::EventLoop &loop, const std::string &address, std::uint16_t port,
                AuthService &auth);
    void start();

  private:
    protocol::HttpResponse handle(const protocol::HttpRequest &request);
    AuthService &m_auth;
    protocol::HttpServer m_http;
    std::chrono::steady_clock::time_point m_window = std::chrono::steady_clock::now();
    unsigned int m_attempts = 0;
};
} // namespace obs::services
