#include "SchedulerServer.h"
#include <stdexcept>

namespace obs::services {
SchedulerServer::SchedulerServer(net::EventLoop &loop, const std::string &address,
                                 std::uint16_t port, std::vector<LoginNode> nodes)
    : m_loop(loop),
      m_http(loop, address, port, [this](const auto &request) { return handle(request); }),
      m_nodes(std::move(nodes)) {
    if (m_nodes.empty())
        throw std::invalid_argument("scheduler requires login nodes");
}
SchedulerServer::~SchedulerServer() {
    m_loop.cancelTimer(m_probeTimer);
    for (auto &node : m_nodes)
        if (node.probe)
            node.probe->stop();
}
void SchedulerServer::start() {
    m_http.start();
    probeNodes(); // 启动时即探测，首次探测完成前拒绝分配未经验证的节点。
    m_probeTimer = m_loop.runEvery(std::chrono::seconds(2), [this] { probeNodes(); });
}
void SchedulerServer::probeNodes() {
    for (std::size_t i = 0; i < m_nodes.size(); ++i) {
        auto &node = m_nodes[i];
        node.probe = std::make_shared<protocol::HttpProbe>(
            m_loop, node.address, node.port,
            [this, i](bool healthy) { m_nodes[i].healthy = healthy; });
        node.probe->start();
    }
}
protocol::HttpResponse SchedulerServer::handle(const protocol::HttpRequest &request) {
    if (request.method == "GET" && request.target == "/health") {
        std::size_t count = 0;
        for (const auto &node : m_nodes)
            count += node.healthy ? 1 : 0;
        return {count ? 200 : 503,
                {{"service", "scheduler"}, {"ready", count > 0}, {"healthyNodes", count}}};
    }
    if (request.target != "/login/server")
        return {404, protocol::errorBody("NOT_FOUND", "接口不存在")};
    if (request.method != "GET")
        return {405, protocol::errorBody("METHOD_NOT_ALLOWED", "调度需要 GET 请求")};
    // 健康节点之间轮询，只返回地址，不代理账号密码，也不替客户端执行登录。
    for (std::size_t checked = 0; checked < m_nodes.size(); ++checked) {
        auto &node = m_nodes[m_next];
        m_next = (m_next + 1) % m_nodes.size();
        if (node.healthy)
            return {200, {{"nodeId", node.id}, {"loginUrl", node.publicUrl}}};
    }
    return {503, protocol::errorBody("NO_LOGIN_NODE", "当前没有可用登录节点")};
}
} // namespace obs::services
