#pragma once
#include "HttpProbe.h"
#include <vector>

namespace obs::services {
// 内部探测地址与返回给客户端的 publicUrl 分开，避免把容器私网地址返回 Windows。
struct LoginNode {
    std::string id, address, publicUrl;
    std::uint16_t port = 0;
    bool healthy = false;
    std::shared_ptr<protocol::HttpProbe> probe;
};
class SchedulerServer final {
  public:
    SchedulerServer(net::EventLoop &loop, const std::string &address, std::uint16_t port,
                    std::vector<LoginNode> nodes);
    ~SchedulerServer();
    void start();

  private:
    void probeNodes();
    protocol::HttpResponse handle(const protocol::HttpRequest &request);
    net::EventLoop &m_loop;
    protocol::HttpServer m_http;
    std::vector<LoginNode> m_nodes;
    net::TimerId m_probeTimer;
    std::size_t m_next = 0;
};
} // namespace obs::services
