#pragma once
#include "H264Adapter.h"
#include <atomic>
#include <chrono>
#include <rtc/rtc.hpp>

namespace obs::media {
// 一个观众一条 PeerConnection；普通类不建线程，所有公开接口由网关 loop 调用。
// libdatachannel 自己持有 ICE/DTLS/媒体内部线程，不借用我们的 FFmpeg 上下文。
class RtcSession final {
  public:
    RtcSession(const std::string &offer, const std::string &profile, std::uint16_t firstPort,
               std::uint16_t lastPort);
    ~RtcSession();
    bool gathered() const noexcept;
    bool failed() const noexcept;
    bool connected() const noexcept;
    std::string answer(const std::string &publicAddress) const;
    void push(const VideoUnit &unit); // 完整压缩帧 → 按协商 PT/SSRC 封装 RTP。
    void resync() noexcept {
        m_waitIdr = true;
    }
    std::uint64_t submitted() const noexcept {
        return m_submitted;
    }
    std::uint64_t skipped() const noexcept {
        return m_skipped;
    }
    std::size_t bytesSent() const {
        return m_peer->bytesSent();
    }

  private:
    struct Signals {
        std::atomic<bool> gathered{false};
        std::atomic<rtc::PeerConnection::State> state{rtc::PeerConnection::State::New};
    };
    std::shared_ptr<Signals> m_signals =
        std::make_shared<Signals>();             // 库回调只捕获它，不捕获 this。
    std::shared_ptr<rtc::PeerConnection> m_peer; // 负责 SDP/ICE/DTLS/SRTP 及库内部生命周期。
    std::shared_ptr<rtc::Track> m_video; // 独立观众的 RTP 序号、时间戳、SSRC 和 RTCP 链。
    bool m_waitIdr = true;               // 入场、断流和裁剪后不从 P 帧开始。
    std::uint64_t m_epoch = 0, m_submitted = 0, m_skipped = 0;
    std::int64_t m_originUs = 0, m_baseUs = 0, m_lastUs = -1;
    std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
};
} // namespace obs::media
