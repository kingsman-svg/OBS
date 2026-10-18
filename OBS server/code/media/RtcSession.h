#pragma once
#include "H264Adapter.h"
#include "RtpClock.h"
#include <atomic>
#include <chrono>
#include <rtc/rtc.hpp>

namespace obs::media {
// 一个观众一条 PeerConnection；普通类不建线程，所有公开接口由网关 loop 调用。
// libdatachannel 自己持有 ICE/DTLS/媒体内部线程，不借用我们的 FFmpeg 上下文。
class RtcSession final {
  public:
    RtcSession(const std::string &offer, const std::string &profile, bool hasAudio,
               std::uint16_t firstPort, std::uint16_t lastPort);
    ~RtcSession();
    bool gathered() const noexcept;
    bool failed() const noexcept;
    bool connected() const noexcept;
    std::string answer(const std::string &publicAddress) const;
    void push(const MediaUnit &unit); // 两轨用共同 PTS 映射，再分别封装 90kHz/48kHz RTP。
    void resync() noexcept {
        m_clock->resync();
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
    std::uint64_t audioSubmitted() const noexcept {
        return m_audioSubmitted;
    }
    std::uint64_t audioSkipped() const noexcept {
        return m_audioSkipped;
    }
    std::uint64_t reanchors() const noexcept {
        return m_clock->reanchors();
    }
    std::uint64_t videoReports() const noexcept {
        return m_videoReport->reports();
    }
    std::uint64_t audioReports() const noexcept {
        return m_audioReport ? m_audioReport->reports() : 0;
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
    std::shared_ptr<rtc::Track> m_audio; // 可选 Opus 轨道，同一 PeerConnection/MSID/CNAME。
    std::shared_ptr<RtpClock> m_clock = std::make_shared<RtpClock>(); // 两轨唯一的源 PTS 映射。
    std::shared_ptr<rtc::RtpPacketizationConfig> m_videoConfig, m_audioConfig;
    std::shared_ptr<ClockSenderReport> m_videoReport, m_audioReport; // 理论时钟与 NTP 配对。
    std::uint64_t m_submitted = 0, m_skipped = 0, m_audioSubmitted = 0, m_audioSkipped = 0;
};
} // namespace obs::media
