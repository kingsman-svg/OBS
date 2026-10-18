#include "RtmpSource.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}
#include <stdexcept>

namespace obs::media {
RtmpSource::RtmpSource(std::string baseUrl, Sink sink)
    : m_baseUrl(std::move(baseUrl)), m_sink(std::move(sink)) {
    if (m_baseUrl.rfind("rtmp://", 0) != 0 || !m_sink)
        throw std::invalid_argument("invalid RTMP source configuration");
    avformat_network_init();
}
RtmpSource::~RtmpSource() {
    stop();
    join();
}
void RtmpSource::start() {
    // 1. 构造不启动线程；网关 start 后才进入读取循环。
    if (m_thread.joinable())
        throw std::logic_error("source already started");
    m_stopping = false;
    m_thread = std::thread([this] { run(); });
}
void RtmpSource::select(const std::string &name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = SourceStatus{};
    m_status.stream = name;
    m_status.phase = "connecting";
    ++m_generation;
    m_wakeup.notify_all();
}
void RtmpSource::stop() {
    m_stopping = true;
    m_wakeup.notify_all();
}
void RtmpSource::join() {
    if (m_thread.joinable())
        m_thread.join();
}
SourceStatus RtmpSource::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}
int RtmpSource::interrupt(void *opaque) {
    const auto &self = *static_cast<RtmpSource *>(opaque);
    return self.m_stopping || self.m_generation != self.m_readGeneration ||
           av_gettime_relative() >= self.m_deadlineUs;
}
void RtmpSource::run() {
    std::uint64_t epoch = 0;
    while (!m_stopping) {
        std::string stream;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wakeup.wait(lock, [this] { return m_stopping || !m_status.stream.empty(); });
            if (m_stopping)
                break;
            stream = m_status.stream;
            m_readGeneration = m_generation;
            m_status.phase = "connecting";
            m_status.epoch = ++epoch;
        }
        bool unsupported = false;
        auto *format = avformat_alloc_context();
        auto *packet = av_packet_alloc();
        try {
            if (!format || !packet)
                throw std::bad_alloc();
            // 2. 三秒期限+interrupt_callback，使 stop/换流不被 RTMP 握手/读包长期阻塞。
            format->interrupt_callback = {interrupt, this};
            m_deadlineUs = av_gettime_relative() + 3000000;
            AVDictionary *options = nullptr;
            av_dict_set(&options, "rtmp_live", "live", 0);
            av_dict_set(&options, "rtmp_buffer", "50", 0);
            av_dict_set(&options, "tcp_nodelay", "1", 0);
            av_dict_set(&options, "probesize", "131072", 0);
            av_dict_set(&options, "analyzeduration", "100000", 0);
            const auto url = m_baseUrl + "/" + stream;
            const int opened = avformat_open_input(&format, url.c_str(), nullptr, &options);
            av_dict_free(&options);
            if (opened < 0 || avformat_find_stream_info(format, nullptr) < 0)
                throw std::runtime_error("upstream unavailable");
            const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (index < 0)
                throw std::runtime_error("upstream has no video");
            const auto &video = *format->streams[index];
            H264Adapter adapter(*video.codecpar, video.time_base);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_generation == m_readGeneration) {
                    m_status.phase = "ready";
                    m_status.note.clear();
                    m_status.profile = adapter.profile();
                    m_status.width = video.codecpar->width;
                    m_status.height = video.codecpar->height;
                }
            }
            std::int64_t lastPts = AV_NOPTS_VALUE;
            while (!interrupt(this)) {
                // 3. 上游 RTMP 已按直播节奏读入；只转视频码流，AAC 本阶段明确忽略。
                m_deadlineUs = av_gettime_relative() + 3000000;
                if (av_read_frame(format, packet) < 0)
                    break;
                if (packet->stream_index == index) {
                    auto unit = adapter.convert(*packet, epoch);
                    if (lastPts != AV_NOPTS_VALUE && unit.ptsUs <= lastPts)
                        throw std::runtime_error("upstream timestamp discontinuity");
                    lastPts = unit.ptsUs;
                    if (!interrupt(this)) {
                        m_sink(std::move(unit));
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_generation == m_readGeneration)
                            ++m_status.frames;
                    }
                }
                av_packet_unref(packet);
            }
        } catch (const std::invalid_argument &) {
            unsupported = true;
        } catch (const std::exception &) {
            // 不打印上游 URL/包/SDP，错误只反映到可读状态；允许主播稍后开始推流。
        }
        // 4. 先销毁 adapter 再释放输入；中断后也只由读取线程收尾。
        av_packet_free(&packet);
        avformat_close_input(&format);
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_generation == m_readGeneration && !m_stopping) {
            m_status.phase = unsupported ? "unsupported" : "retrying";
            m_status.note = unsupported ? "首版要求带 avcC 和有效 PTS/DTS 的无 B 帧 H.264 视频"
                                        : "等待上游直播或重试连接";
            ++m_status.reconnects;
            m_wakeup.wait_for(lock, std::chrono::seconds(1),
                              [this] { return m_stopping || m_generation != m_readGeneration; });
        }
    }
}
} // namespace obs::media
