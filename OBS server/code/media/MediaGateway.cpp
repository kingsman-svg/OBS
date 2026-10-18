#include "MediaGateway.h"
#include <algorithm>
#include <fstream>
#include <regex>

namespace obs::media {
using namespace protocol;
using Clock = std::chrono::steady_clock;
MediaGateway::MediaGateway(net::EventLoop &loop, services::AuthService &auth, GatewayConfig config)
    : m_loop(loop), m_auth(auth), m_config(std::move(config)),
      m_http(loop, m_config.bindAddress, m_config.httpPort,
             HttpServer::AsyncHandler(
                 [this](const auto &request, auto reply) { handle(request, std::move(reply)); }),
             std::chrono::seconds(12)),
      m_source(m_config.rtmpBase, [this](VideoUnit unit) { enqueue(std::move(unit)); }) {
}
MediaGateway::~MediaGateway() {
    // 6. main 已退出 loop；stop 禁用入口/定时器并关闭 RTC，再 join FFmpeg 线程。
    stop();
    m_source.join();
}
void MediaGateway::start() {
    // 1. 构造只绑定回调；start 启动监听/读取线程，真正 epoll loop 由 main 调用。
    if (m_started)
        return;
    m_http.start();
    m_source.start();
    m_timer = m_loop.runEvery(std::chrono::milliseconds(5), [this] { tick(); });
    m_started = true;
}
void MediaGateway::stop() {
    if (!m_started)
        return;
    m_http.stop();
    m_loop.cancelTimer(m_timer);
    m_source.stop();
    m_viewers.clear();
    m_started = false;
}
void MediaGateway::handle(const HttpRequest &request, HttpServer::Reply reply) {
    // 2. 页面与健康检查公开，控制/媒体会话必须使用未过期 Bearer 登录令牌。
    if (request.method == "GET" && request.target == "/health") {
        reply(HttpReply::json(200,
                              {{"ready", true}, {"role", "media-gateway"}, {"videoOnly", true}}));
        return;
    }
    if (request.method == "GET" && request.target == "/") {
        reply({200, m_config.page, "text/html; charset=utf-8"});
        return;
    }
    if (request.method == "POST" && request.target == "/api/login") {
        const auto now = Clock::now();
        if (now - m_loginWindow >= std::chrono::seconds(10)) {
            m_loginWindow = now;
            m_loginAttempts = 0;
        }
        if (++m_loginAttempts > 8) {
            reply(HttpReply::json(429, errorBody("RATE_LIMIT", "登录请求过多，请稍后重试")));
            return;
        }
        const auto body = Json::parse(request.body);
        if (!m_auth.checkPassword(body.at("account").get<std::string>(),
                                  body.at("password").get<std::string>())) {
            reply(HttpReply::json(401, errorBody("LOGIN_FAILED", "账号或密码错误")));
            return;
        }
        reply(HttpReply::json(200, {{"token", m_auth.issue("root")}}));
        return;
    }
    std::string owner;
    std::int64_t expires = 0;
    const auto header = request.headers.find("authorization");
    if (header == request.headers.end() || header->second.rfind("Bearer ", 0) != 0 ||
        !m_auth.verify(header->second.substr(7), owner, expires)) {
        reply(HttpReply::json(401, errorBody("UNAUTHORIZED", "请先登录，或重新获取有效令牌")));
        return;
    }
    if (request.method == "GET" && request.target == "/api/status") {
        reply(HttpReply::json(200, status()));
        return;
    }
    if (request.method == "POST" && request.target == "/api/source") {
        const auto body = Json::parse(request.body);
        const auto name = body.at("stream").get<std::string>();
        if (!std::regex_match(name, std::regex("[A-Za-z0-9_-]{1,96}"))) {
            reply(
                HttpReply::json(400, errorBody("INVALID_STREAM", "仅填写直播地址最后一段流名称")));
            return;
        }
        const auto current = m_source.status().stream;
        if (current != name && !m_viewers.empty()) {
            reply(HttpReply::json(409,
                                  errorBody("SOURCE_BUSY", "已有观众，请先停止播放再切换上游流")));
            return;
        }
        if (current != name) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queue.clear();
            m_queueBytes = 0;
            m_resync = true;
            m_source.select(name); // 只提交命令；IO 和探测在 source 线程执行。
        }
        reply(HttpReply::json(202, {{"accepted", true}}));
        return;
    }
    if (request.method == "POST" && request.target == "/api/sessions") {
        const auto input = m_source.status();
        if (input.phase != "ready" || !input.frames) {
            reply(HttpReply::json(
                503, errorBody("SOURCE_NOT_READY", "上游尚未就绪，请先开始 RTMP 推流")));
            return;
        }
        if (m_viewers.size() >= m_config.maxViewers) {
            reply(HttpReply::json(429, errorBody("VIEWERS_FULL", "首版观众数量达到配置上限")));
            return;
        }
        const auto body = Json::parse(request.body);
        try {
            auto rtc =
                std::make_unique<RtcSession>(body.at("sdp").get<std::string>(), input.profile,
                                             m_config.firstPort, m_config.lastPort);
            const auto id = services::AuthService::randomId();
            m_viewers.emplace(
                id, Viewer{std::move(rtc), owner, expires, Clock::now(), std::move(reply)});
            // ICE 回调只改原子状态；tick 完成 HTTP 响应，epoll 不等待库线程。
        } catch (const std::invalid_argument &) {
            reply(HttpReply::json(
                406,
                errorBody("H264_INCOMPATIBLE",
                          "需要单条 recvonly 视频和兼容的 H.264 profile/level；首版不转码视频")));
        }
        return;
    }
    const std::string prefix = "/api/sessions/";
    if (request.method == "DELETE" && request.target.rfind(prefix, 0) == 0) {
        const auto it = m_viewers.find(request.target.substr(prefix.size()));
        if (it == m_viewers.end() || it->second.owner != owner) {
            reply(HttpReply::json(404, errorBody("NOT_FOUND", "会话不存在")));
            return;
        }
        m_viewers.erase(it);
        reply(HttpReply::json(200, {{"closed", true}}));
        return;
    }
    reply(HttpReply::json(404, errorBody("NOT_FOUND", "接口不存在")));
}
void MediaGateway::enqueue(VideoUnit unit) {
    // 3. 不为每帧向 EventLoop 投递任务；队列同时受 32 帧/8MiB/300ms 约束。
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (!m_queue.empty() &&
        (m_queue.size() >= 32 || m_queueBytes + unit.bytes->size() > 8 * 1024 * 1024 ||
         unit.epoch != m_queue.front().unit.epoch ||
         unit.ptsUs - m_queue.front().unit.ptsUs > 300000)) {
        m_trimmed += m_queue.size();
        m_queue.clear();
        m_queueBytes = 0;
        m_resync = true;
    }
    m_queueBytes += unit.bytes->size();
    m_queue.push_back({std::move(unit), Clock::now()});
}
void MediaGateway::tick() {
    // 4. 握手、令牌过期、未连接期限收敛在 loop；不会留下永远等待的 HTTP/RTC 会话。
    const auto now = Clock::now();
    for (auto it = m_viewers.begin(); it != m_viewers.end();) {
        auto &viewer = it->second;
        const bool expired = viewer.expires <= services::AuthService::unixSeconds();
        if (expired || viewer.rtc->failed() ||
            (!viewer.rtc->connected() && now - viewer.created > std::chrono::seconds(10))) {
            if (viewer.pending)
                viewer.pending(
                    HttpReply::json(504, errorBody("RTC_TIMEOUT", "会话已结束或协商超时")));
            it = m_viewers.erase(it);
            continue;
        }
        if (viewer.pending && viewer.rtc->gathered()) {
            try {
                viewer.pending(
                    HttpReply::json(201, {{"id", it->first},
                                          {"type", "answer"},
                                          {"sdp", viewer.rtc->answer(m_config.publicAddress)}}));
                viewer.pending = {};
            } catch (const std::exception &) {
                viewer.pending(HttpReply::json(500, errorBody("RTC_FAILED", "无法生成媒体会话")));
                it = m_viewers.erase(it);
                continue;
            }
        }
        ++it;
    }
    std::deque<Queued> frames;
    bool resync = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        frames.swap(m_queue);
        m_queueBytes = 0;
        resync = m_resync;
        m_resync = false;
    }
    if (resync)
        for (auto &[id, viewer] : m_viewers) {
            (void)id;
            viewer.rtc->resync();
        }
    const auto source = m_source.status();
    for (const auto &frame : frames) {
        if (frame.unit.epoch != source.epoch ||
            now - frame.arrived > std::chrono::milliseconds(300)) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ++m_trimmed;
            for (auto &[id, viewer] : m_viewers) {
                (void)id;
                viewer.rtc->resync();
            }
            continue;
        }
        // 5. 共享压缩数据，每个观众独立 RTP 打包/加密；不经过 Qt/GPU 解码/美颜链路。
        for (auto &[id, viewer] : m_viewers) {
            (void)id;
            viewer.rtc->push(frame.unit);
        }
    }
}
Json MediaGateway::status() const {
    const auto input = m_source.status();
    Json peers = Json::array();
    for (const auto &[id, viewer] : m_viewers)
        peers.push_back({{"id", id},
                         {"connected", viewer.rtc->connected()},
                         {"submittedFrames", viewer.rtc->submitted()},
                         {"skippedFrames", viewer.rtc->skipped()},
                         {"transportBytes", viewer.rtc->bytesSent()}});
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return {{"videoOnly", true},
            {"stream", input.stream},
            {"phase", input.phase},
            {"note", input.note},
            {"profile", input.profile},
            {"width", input.width},
            {"height", input.height},
            {"epoch", input.epoch},
            {"inputFrames", input.frames},
            {"reconnects", input.reconnects},
            {"viewers", peers},
            {"queuedFrames", m_queue.size()},
            {"queuedBytes", m_queueBytes},
            {"trimmedFrames", m_trimmed}};
}
} // namespace obs::media
