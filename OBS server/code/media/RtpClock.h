#pragma once
#include "MediaUnit.h"
#include <chrono>
#include <optional>
#include <rtc/rtc.hpp>

namespace obs::media {
// 每个观众一个共同媒体钟；公开接口和 SR outgoing 都只在网关 loop 执行，无自建线程。
class RtpClock final {
  public:
    using Steady = std::chrono::steady_clock;
    RtpClock(Steady::time_point steady = Steady::now(),
             std::chrono::system_clock::time_point wall = std::chrono::system_clock::now());
    std::optional<std::int64_t> map(const MediaUnit &unit, Steady::time_point now = Steady::now());
    void resync() noexcept {
        m_waitIdr = true;
    } // 音视频共同等待下一 IDR，不分别归零。
    std::uint64_t ntp(Steady::time_point now) const;
    std::uint32_t rtpNow(std::uint32_t rate, std::uint32_t offset, Steady::time_point now) const;
    static std::uint32_t rtp(std::int64_t microseconds, std::uint32_t rate, std::uint32_t offset);
    std::uint64_t reanchors() const noexcept {
        return m_reanchors;
    }
    std::int64_t lastAudioUs() const noexcept {
        return m_lastAudio;
    }
    std::int64_t lastVideoUs() const noexcept {
        return m_lastVideo;
    }

  private:
    Steady::time_point m_steady; // 会话单调钟原点，系统校时不改变媒体推进速度。
    std::int64_t m_wallUs;       // 原点对应 UTC，只读取一次供 NTP 配对。
    bool m_waitIdr = true;
    std::uint64_t m_epoch = 0, m_reanchors = 0;
    std::int64_t m_sourceOrigin = 0, m_outputOrigin = 0;
    std::int64_t m_lastAudio = -1, m_lastVideo = -1; // 分轨单调检查，共享同一映射。
};

// 自定义 SR：NTP 与“同一时刻的理论 RTP 钟”配对，不能使用最后一个包的 PTS 代替现在。
class ClockSenderReport final : public rtc::MediaHandler {
  public:
    ClockSenderReport(std::shared_ptr<rtc::RtpPacketizationConfig> config,
                      std::shared_ptr<RtpClock> clock);
    void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override;
    std::uint64_t reports() const noexcept {
        return m_reports;
    }

  private:
    std::shared_ptr<rtc::RtpPacketizationConfig> m_config; // 本轨随机 RTP 原点/SSRC，跨轨各自独立。
    std::shared_ptr<RtpClock> m_clock; // 音视频共享，报告参考时间相同。
    RtpClock::Steady::time_point m_lastReport{};
    std::uint32_t m_packets = 0, m_octets = 0; // RFC3550 模 2^32 累积计数。
    std::uint64_t m_reports = 0;
};
} // namespace obs::media
