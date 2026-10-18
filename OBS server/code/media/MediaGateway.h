#pragma once
#include "AuthService.h"
#include "HttpServer.h"
#include "RtcSession.h"
#include "RtmpSource.h"
#include <deque>
#include <map>

namespace obs::media {
struct GatewayConfig {
    std::string bindAddress = "0.0.0.0", publicAddress = "127.0.0.1", rtmpBase, page;
    std::uint16_t httpPort = 8090, firstPort = 8100, lastPort = 8109;
    unsigned maxViewers = 8;
};

// 网关编排类：持有 HTTP 服务、一个上游读取线程及若干普通 RTC 会话。
// HTTP/会话表/定时器只归 main 的 EventLoop；读取线程仅向有界帧队列写数据。
class MediaGateway final {
  public:
    MediaGateway(net::EventLoop &loop, services::AuthService &auth, GatewayConfig config);
    ~MediaGateway();
    void start(); // main 在线程资源和信号通道就绪后调用。
    void stop();  // loop 线程发出停止，不在请求回调等待读线程。

  private:
    struct Viewer {
        std::unique_ptr<RtcSession> rtc;
        std::string owner;
        std::int64_t expires = 0;
        std::chrono::steady_clock::time_point created;
        protocol::HttpServer::Reply pending; // ICE 完成后在 loop 回复；未连接会话十秒收敛。
    };
    struct Queued {
        MediaUnit unit;
        std::chrono::steady_clock::time_point arrived;
    };
    void handle(const protocol::HttpRequest &request, protocol::HttpServer::Reply reply);
    void enqueue(MediaUnit unit); // 读取线程调用，音视频共用有界交接队列，溢出后共同等 IDR。
    void tick(); // loop 每 5ms 处理帧和异步握手，不等待 ICE 或上游 IO。
    protocol::Json status() const;
    net::EventLoop &m_loop;        // 借用，最后销毁。
    services::AuthService &m_auth; // 复用登录/信令的签名与账号逻辑。
    GatewayConfig m_config;
    protocol::HttpServer m_http; // 8090，仅 HTTP JSON 协商，本阶段尚非 WHEP。
    RtmpSource m_source;         // 唯一自建的媒体读取线程，先 stop 再 join。
    std::map<std::string, Viewer> m_viewers; // 每个观众独立 PeerConnection 与 RTP/RTCP 状态。
    net::TimerId m_timer;
    bool m_started = false;
    mutable std::mutex m_queueMutex; // 只保护下列队列，不持锁发送网络数据。
    std::deque<Queued> m_queue;
    std::size_t m_queueBytes = 0;
    std::int64_t m_queueMinPts = 0,
                 m_queueMaxPts = 0; // 两轨可能交错到达，用真正 min/max 检查跨度。
    bool m_resync = false;
    std::uint64_t m_trimmed = 0;  // 被应用队列主动裁剪的 access unit 数。
    unsigned m_loginAttempts = 0; // 本机开发入口的全局登录节流窗口。
    std::chrono::steady_clock::time_point m_loginWindow = std::chrono::steady_clock::now();
};
} // namespace obs::media
