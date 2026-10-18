#include "RtpClock.h"
#include <algorithm>
#include <stdexcept>

namespace obs::media {
RtpClock::RtpClock(Steady::time_point steady, std::chrono::system_clock::time_point wall)
    : m_steady(steady),
      m_wallUs(
          std::chrono::duration_cast<std::chrono::microseconds>(wall.time_since_epoch()).count()) {
}
std::optional<std::int64_t> RtpClock::map(const MediaUnit &unit, Steady::time_point now) {
    // 1. 新段/裁剪后共同等待视频 IDR；不把音频提前独立设为零而破坏原始偏移。
    if (unit.epoch != m_epoch) {
        m_epoch = unit.epoch;
        m_waitIdr = true;
    }
    if (unit.discontinuity)
        m_waitIdr = true;
    if (m_waitIdr) {
        if (unit.kind != MediaKind::Video || !unit.idr)
            return {};
        m_sourceOrigin = unit.ptsUs;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(now - m_steady).count();
        m_outputOrigin = std::max({elapsed, m_lastVideo + 33333, m_lastAudio + 20000});
        m_waitIdr = false;
        ++m_reanchors;
    }
    // 2. 共享偏移保留源头音画关系；入场前整包音频不再回放，编码预读不作第二次修正。
    if (unit.kind == MediaKind::Audio && unit.ptsUs + unit.durationUs <= m_sourceOrigin)
        return {};
    const auto mapped = m_outputOrigin + unit.ptsUs - m_sourceOrigin;
    auto &last = unit.kind == MediaKind::Video ? m_lastVideo : m_lastAudio;
    if (mapped < 0 || mapped <= last)
        return {};
    last = mapped;
    return mapped;
}
std::uint32_t RtpClock::rtp(std::int64_t us, std::uint32_t rate, std::uint32_t offset) {
    // 3. 用整数换算并自然处理 32 位回绕，避免长时间 double 累积误差。
    const auto ticks = (us / 1000000) * rate + (us % 1000000) * rate / 1000000;
    return offset + static_cast<std::uint32_t>(ticks);
}
std::uint32_t RtpClock::rtpNow(std::uint32_t rate, std::uint32_t offset,
                               Steady::time_point now) const {
    return rtp(std::chrono::duration_cast<std::chrono::microseconds>(now - m_steady).count(), rate,
               offset);
}
std::uint64_t RtpClock::ntp(Steady::time_point now) const {
    const auto us =
        m_wallUs + std::chrono::duration_cast<std::chrono::microseconds>(now - m_steady).count();
    return (static_cast<std::uint64_t>(us / 1000000 + 2208988800LL) << 32) |
           ((static_cast<std::uint64_t>(us % 1000000) << 32) / 1000000);
}
ClockSenderReport::ClockSenderReport(std::shared_ptr<rtc::RtpPacketizationConfig> config,
                                     std::shared_ptr<RtpClock> clock)
    : m_config(std::move(config)), m_clock(std::move(clock)) {
    if (!m_config || !m_clock)
        throw std::invalid_argument("SR clock required");
}
void ClockSenderReport::outgoing(rtc::message_vector &messages, const rtc::message_callback &send) {
    // 4. 在 RTP 打包后统计本轨包和有效载荷；NACK 直接重传不会重复计入新包。
    bool media = false;
    for (const auto &message : messages) {
        if (message->type == rtc::Message::Control || message->size() < sizeof(rtc::RtpHeader))
            continue;
        const auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
        if (header->ssrc() != m_config->ssrc || header->getSize() > message->size())
            continue;
        ++m_packets;
        m_octets += static_cast<std::uint32_t>(message->size() - header->getSize());
        media = true;
    }
    const auto now = RtpClock::Steady::now();
    if (!media || (m_reports && now - m_lastReport < std::chrono::seconds(1)))
        return;
    // 5. 一个复合 RTCP SR+SDES，音视频使用同一 CNAME、NTP 原点和单调钟。
    const auto srSize = rtc::RtcpSr::Size(0);
    auto message = rtc::make_message(
        srSize + rtc::RtcpSdes::Size({{static_cast<std::uint8_t>(m_config->cname.size())}}),
        rtc::Message::Control);
    auto *sr = reinterpret_cast<rtc::RtcpSr *>(message->data());
    sr->setNtpTimestamp(m_clock->ntp(now));
    sr->setRtpTimestamp(m_clock->rtpNow(m_config->clockRate, m_config->startTimestamp, now));
    sr->setPacketCount(m_packets);
    sr->setOctetCount(m_octets);
    sr->preparePacket(m_config->ssrc, 0);
    auto *sdes = reinterpret_cast<rtc::RtcpSdes *>(message->data() + srSize);
    auto *chunk = sdes->getChunk(0);
    chunk->setSSRC(m_config->ssrc);
    auto *item = chunk->getItem(0);
    item->type = 1;
    item->setText(m_config->cname);
    sdes->preparePacket(1);
    send(message);
    m_lastReport = now;
    ++m_reports;
}
} // namespace obs::media
