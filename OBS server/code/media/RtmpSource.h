#pragma once
#include "H264Adapter.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace obs::media {
struct SourceStatus {
    std::string stream;               // 固定上游基址下的流名称，不接收任意 URL。
    std::string phase = "idle", note; // 可读业务阶段/原因，不包含 FFmpeg 异常或认证数据。
    std::string profile;              // 当前 avcC profile-level-id。
    int width = 0, height = 0; // 输入视频尺寸，尚未表示浏览器已解码。
    std::uint64_t epoch = 0; // 本进程输入重开序号；同一 RTMP 连接内重推未必改变它。
    std::uint64_t frames = 0, reconnects = 0; // 本次选择的输入计数。
};

// 网关独占一个读取线程；HTTP loop 只发选择命令、读取状态，不等待上游网络。
class RtmpSource final {
  public:
    using Sink = std::function<void(VideoUnit)>;
    RtmpSource(std::string baseUrl, Sink sink);
    ~RtmpSource();
    void start();                         // loop 线程调用，创建读取线程。
    void select(const std::string &name); // loop 线程调用，唤醒/打断旧流，不 join。
    void stop();                          // 原子停止+唤醒，可跨线程。
    void join();                          // main 在 loop 退出后调用，等待资源清理。
    SourceStatus status() const;          // 复制快照，不返回借用引用。

  private:
    void run(); // FFmpeg 上下文、H264Adapter、AVPacket 全部只归此线程。
    static int interrupt(void *opaque);
    std::string m_baseUrl; // 固定服务器基址；外部只允许选择安全流名。
    Sink m_sink;           // 读取线程调用，网关只做有界入队。
    std::thread m_thread;  // 唯一自建媒体读取线程。
    std::atomic<bool> m_stopping{false};
    std::atomic<std::uint64_t> m_generation{0}; // select 立即使旧 IO 取消。
    std::uint64_t m_readGeneration = 0;         // 只在读取线程使用。
    std::int64_t m_deadlineUs = 0; // 每次 open/read 的单调时钟期限，仅读取线程访问。
    mutable std::mutex m_mutex; // 保护命令和快照，不持锁执行 FFmpeg 或业务回调。
    std::condition_variable m_wakeup; // 空闲/重试等待可立即被 select/stop 打断。
    SourceStatus m_status;
};
} // namespace obs::media
