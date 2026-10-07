#pragma once
#include <cstdint>
#include <string>

namespace obs::services {
// 登录节点与信令节点共享签名密钥，凭据保持不透明；本阶段只提供 root 开发账号。
class AuthService final {
  public:
    AuthService(std::string secret, std::string password, int ttlSeconds = 3600);
    bool checkPassword(const std::string &account, const std::string &password) const;
    std::string issue(const std::string &userId) const;
    bool verify(const std::string &token, std::string &userId, std::int64_t &expiresAt) const;
    int ttlSeconds() const noexcept;
    static std::string randomId();
    static std::int64_t unixSeconds();

  private:
    std::string sign(const std::string &payload) const;
    std::string m_secret, m_passwordDigest;
    int m_ttlSeconds;
};
} // namespace obs::services
