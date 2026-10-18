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
RtcSession::RtcSession(const std::string &sdp, const std::string &profile, bool hasAudio,
                       std::uint16_t firstPort, std::uint16_t lastPort) {
    // 1. 按真实输入协商视频及可选音频；每轨必须 recvonly，不能依赖固定 m-line 顺序。
    rtc::Description offer(sdp, rtc::Description::Type::Offer);
    if (offer.mediaCount() != (hasAudio ? 2 : 1) || offer.hasApplication())
        throw std::invalid_argument("recvonly media tracks required");
    rtc::Description::Media *videoOffer = nullptr, *audioOffer = nullptr;
    for (int i = 0; i < offer.mediaCount(); ++i) {
        const auto entry = offer.media(i);
        const auto media = std::get_if<rtc::Description::Media *>(&entry);
        if (!media || (*media)->direction() != rtc::Description::Direction::RecvOnly)
            throw std::invalid_argument("recvonly required");
        if ((*media)->type() == "video" && !videoOffer)
            videoOffer = *media;
        else if ((*media)->type() == "audio" && hasAudio && !audioOffer)
            audioOffer = *media;
        else
            throw std::invalid_argument("unexpected media track");
    }
    if (!videoOffer || (hasAudio && !audioOffer))
        throw std::invalid_argument("missing media track");
    int payload = -1;
    for (int pt : videoOffer->payloadTypes()) {
        const auto *map = videoOffer->rtpMap(pt);
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
    int audioPayload = -1;
    if (audioOffer) {
        for (int pt : audioOffer->payloadTypes()) {
            const auto *map = audioOffer->rtpMap(pt);
            if (map && (map->format == "opus" || map->format == "OPUS") &&
                map->clockRate == 48000 && map->encParams == "2") {
                audioPayload = pt;
                break;
            }
        }
        if (audioPayload < 0)
            throw std::invalid_argument("Opus 48000/2 required");
    }

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
    rtc::Description::Video video(videoOffer->mid(), rtc::Description::Direction::SendOnly);
    video.addH264Codec(payload, "profile-level-id=" + profile +
                                    ";packetization-mode=1;level-asymmetry-allowed=1");
    const auto ssrc = std::random_device{}() | 1U;
    video.addSSRC(ssrc, "obs-gateway", "obs-gateway", "video");
    m_video = m_peer->addTrack(video);
    auto rtp = m_videoConfig =
        std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "obs-gateway", payload, 90000);
    auto packetizer =
        std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp, 1100);
    m_videoReport = std::make_shared<ClockSenderReport>(rtp, m_clock);
    packetizer->addToChain(m_videoReport);
    // 3. 先复用库的有界 512 包 NACK 缓存；自定义重传期限/PLI/GCC 在后续阶段处理。
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
    m_video->setMediaHandler(packetizer);
    if (audioOffer) {
        rtc::Description::Audio audio(audioOffer->mid(), rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(audioPayload, "minptime=10;stereo=1;sprop-stereo=1;useinbandfec=0");
        const auto audioSsrc = ssrc + 2U;
        audio.addSSRC(audioSsrc, "obs-gateway", "obs-gateway", "audio");
        m_audio = m_peer->addTrack(audio);
        m_audioConfig = std::make_shared<rtc::RtpPacketizationConfig>(audioSsrc, "obs-gateway",
                                                                      audioPayload, 48000);
        auto opus = std::make_shared<rtc::OpusRtpPacketizer>(m_audioConfig);
        m_audioReport = std::make_shared<ClockSenderReport>(m_audioConfig, m_clock);
        opus->addToChain(m_audioReport);
        opus->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
        m_audio->setMediaHandler(opus);
    }
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
void RtcSession::push(const MediaUnit &unit) {
    // 5. 两轨就绪后共同从 IDR 开始。媒体 PTS 映射为各轨随机起点上的 RTP 时间戳；
    //    SR 用相同会话理论钟，不把音频转码等待或出队时刻写成新的采集时间。
    const bool audio = unit.kind == MediaKind::Audio;
    if (!m_video->isOpen() || (m_audio && !m_audio->isOpen()) || (audio && !m_audio))
        return;
    auto &skipped = audio ? m_audioSkipped : m_skipped;
    const auto pts = m_clock->map(unit);
    if (!pts) {
        ++skipped;
        return;
    }
    const auto &config = audio ? m_audioConfig : m_videoConfig;
    const auto &track = audio ? m_audio : m_video;
    try {
        track->sendFrame(
            unit.bytes->data(), unit.bytes->size(),
            rtc::FrameInfo(RtpClock::rtp(*pts, config->clockRate, config->startTimestamp)));
        ++(audio ? m_audioSubmitted : m_submitted); // 仅表示交给 RTC 栈，不代表已播放。
    } catch (const std::exception &) {
        m_clock->resync();
        ++skipped;
    }
}
} // namespace obs::media
