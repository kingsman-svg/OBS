#include "RtcSession.h"
#include <algorithm>
#include <random>
#include <stdexcept>

namespace obs::media {
namespace {
// RFC 6184 的常见 profile 模式；首版只做兼容透传，不偷偷改写 SPS 或转码。
int profileType(const std::string &value) {
    if (value.size() != 6 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return -1;
    const int id = std::stoi(value.substr(0, 2), nullptr, 16);
    const int flags = std::stoi(value.substr(2, 2), nullptr, 16);
    if ((id == 0x42 && (flags & 0x4f) == 0x40) || (id == 0x4d && (flags & 0x8f) == 0x80) ||
        (id == 0x58 && (flags & 0xcf) == 0xc0))
        return 0;
    if ((id == 0x42 && (flags & 0x4f) == 0) || (id == 0x58 && (flags & 0xcf) == 0x80))
        return 1;
    if (id == 0x4d && (flags & 0xaf) == 0)
        return 2;
    if (id == 0x64 && flags == 0x0c)
        return 3;
    if (id == 0x64 && flags == 0)
        return 4;
    return -1;
}
std::string parameter(const std::vector<std::string> &fmtps, const std::string &key,
                      std::string fallback) {
    for (const auto &fmtp : fmtps) {
        const auto start = fmtp.find(key + "=");
        if (start != std::string::npos) {
            const auto first = start + key.size() + 1;
            return fmtp.substr(first, fmtp.find(';', first) - first);
        }
    }
    return fallback;
}
} // namespace
RtcSession::RtcSession(const std::string &sdp, const std::string &profile, std::uint16_t firstPort,
                       std::uint16_t lastPort) {
    // 1. 浏览器只申请 recvonly 视频；服务端选择与真实 SPS 兼容的 H.264 payload type。
    rtc::Description offer(sdp, rtc::Description::Type::Offer);
    if (offer.mediaCount() != 1 || offer.hasApplication())
        throw std::invalid_argument("one recvonly video track required");
    const auto entry = offer.media(0);
    const auto media = std::get_if<rtc::Description::Media *>(&entry);
    if (!media || (*media)->type() != "video" ||
        (*media)->direction() != rtc::Description::Direction::RecvOnly)
        throw std::invalid_argument("recvonly video required");
    int payload = -1;
    for (int pt : (*media)->payloadTypes()) {
        const auto *map = (*media)->rtpMap(pt);
        if (!map || (map->format != "H264" && map->format != "h264") || map->clockRate != 90000)
            continue;
        const auto remote = parameter(map->fmtps, "profile-level-id", "42e01f");
        if (profileType(profile) < 0 || profileType(remote) != profileType(profile) ||
            parameter(map->fmtps, "packetization-mode", "0") != "1")
            continue;
        if (std::stoi(profile.substr(4), nullptr, 16) > std::stoi(remote.substr(4), nullptr, 16))
            continue;
        payload = pt;
        break;
    }
    if (payload < 0)
        throw std::invalid_argument("browser has no compatible H264 profile/level");

    // 2. 使用固定端口范围适配 Docker 映射；首版本机使用 host candidate，不依赖公网 STUN。
    rtc::Configuration config;
    config.portRangeBegin = firstPort;
    config.portRangeEnd = lastPort;
    config.disableAutoNegotiation = true;
    config.mtu = 1200;
    m_peer = std::make_shared<rtc::PeerConnection>(config);
    const auto signals = m_signals;
    m_peer->onStateChange([signals](auto state) { signals->state = state; });
    m_peer->onGatheringStateChange([signals](auto state) {
        if (state == rtc::PeerConnection::GatheringState::Complete)
            signals->gathered = true;
    });
    rtc::Description::Video video((*media)->mid(), rtc::Description::Direction::SendOnly);
    video.addH264Codec(payload, "profile-level-id=" + profile +
                                    ";packetization-mode=1;level-asymmetry-allowed=1");
    const auto ssrc = std::random_device{}() | 1U;
    video.addSSRC(ssrc, "obs-gateway", "obs-gateway", "video");
    m_video = m_peer->addTrack(video);
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "obs-gateway", payload, 90000);
    auto packetizer =
        std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp, 1100);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
    // 3. 先复用库的有界 512 包 NACK 缓存；自定义重传期限/PLI/GCC 在后续阶段处理。
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
    m_video->setMediaHandler(packetizer);
    m_peer->setRemoteDescription(std::move(offer));
    m_peer->setLocalDescription(rtc::Description::Type::Answer);
}
RtcSession::~RtcSession() {
    // 6. 回调不访问网关或本对象；先撤销回调并关闭，库内部任务用自己的引用收尾。
    if (m_peer) {
        m_peer->resetCallbacks();
        m_peer->close();
    }
}
bool RtcSession::gathered() const noexcept {
    return m_signals->gathered;
}
bool RtcSession::connected() const noexcept {
    return m_signals->state == rtc::PeerConnection::State::Connected;
}
bool RtcSession::failed() const noexcept {
    const auto state = m_signals->state.load();
    return state == rtc::PeerConnection::State::Failed ||
           state == rtc::PeerConnection::State::Closed ||
           state == rtc::PeerConnection::State::Disconnected;
}
std::string RtcSession::answer(const std::string &publicAddress) const {
    // 4. 保留 ICE 凭据/指纹/真实端口，只把容器 host 地址映射为宿主机可达地址。
    auto description = m_peer->localDescription();
    if (!gathered() || !description)
        throw std::logic_error("ICE gathering incomplete");
    auto candidates = description->extractCandidates();
    for (auto &candidate : candidates) {
        if (candidate.transportType() != rtc::Candidate::TransportType::Udp)
            continue;
        candidate.changeAddress(publicAddress);
        description->addCandidate(candidate);
    }
    description->endCandidates();
    return description->generateSdp();
}
void RtcSession::push(const VideoUnit &unit) {
    // 5. 入场/换段/队列裁剪都等下一个 IDR；首版没有回放旧 GOP，避免拉高首屏延迟。
    if (!m_video->isOpen())
        return;
    if (unit.epoch != m_epoch) {
        m_epoch = unit.epoch;
        m_waitIdr = true;
    }
    if (m_waitIdr) {
        if (!unit.idr) {
            ++m_skipped;
            return;
        }
        m_waitIdr = false;
        m_originUs = unit.ptsUs;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - m_started)
                                 .count();
        m_baseUs = std::max(m_lastUs + 33333, elapsed);
    }
    const auto pts = m_baseUs + unit.ptsUs - m_originUs;
    if (pts <= m_lastUs) {
        m_waitIdr = true;
        ++m_skipped;
        return;
    }
    try {
        m_video->sendFrame(unit.bytes->data(), unit.bytes->size(),
                           std::chrono::duration<double, std::micro>(pts));
        m_lastUs = pts;
        ++m_submitted; // 交给 RTC 栈的帧数，不冒充浏览器解码帧数。
    } catch (const std::exception &) {
        m_waitIdr = true;
        ++m_skipped;
    }
}
} // namespace obs::media
