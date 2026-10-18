#include "H264Adapter.h"
extern "C" {
#include <libavutil/mathematics.h>
}
#include <cstdio>
#include <stdexcept>

namespace obs::media {
namespace {
void checked(int result) {
    if (result < 0)
        throw std::runtime_error("H264 bitstream conversion failed");
}
} // namespace
H264Adapter::H264Adapter(const AVCodecParameters &parameters, AVRational timeBase)
    : m_timeBase(timeBase) {
    // 1. RTMP/FLV 的 AVC sequence header 提供 avcC；不支持的输入明确拒绝。
    if (parameters.codec_id != AV_CODEC_ID_H264 || parameters.video_delay > 0 ||
        timeBase.num <= 0 || timeBase.den <= 0 || !parameters.extradata ||
        parameters.extradata_size < 7 || parameters.extradata[0] != 1)
        throw std::invalid_argument("H264 AVCC without B frames required");
    char profile[7]{};
    std::snprintf(profile, sizeof(profile), "%02x%02x%02x", parameters.extradata[1],
                  parameters.extradata[2], parameters.extradata[3]);
    m_profile = profile;
    // 2. FFmpeg 处理长度前缀到 Annex B 的转换，业务只处理完整 access unit。
    checked(av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"), &m_filter));
    try {
        checked(avcodec_parameters_copy(m_filter->par_in, &parameters));
        m_filter->time_base_in = timeBase;
        checked(av_bsf_init(m_filter));
    } catch (...) {
        av_bsf_free(&m_filter);
        throw;
    }
}
H264Adapter::~H264Adapter() {
    // 4. 读取线程结束后才释放，不能由 HTTP/RTC 回调跨线程操作 filter。
    av_bsf_free(&m_filter);
}
bool H264Adapter::hasIdr(const std::vector<std::byte> &bytes) {
    if (bytes.empty() || bytes.size() > 4 * 1024 * 1024)
        throw std::invalid_argument("invalid H264 frame size");
    bool found = false, idr = false;
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        const auto u = [&](std::size_t n) { return std::to_integer<unsigned>(bytes[n]); };
        if (u(i) || u(i + 1))
            continue;
        std::size_t header = 0;
        if (u(i + 2) == 1)
            header = i + 3;
        else if (u(i + 2) == 0 && i + 4 < bytes.size() && u(i + 3) == 1)
            header = i + 4;
        if (!header)
            continue;
        if (!found && i != 0)
            throw std::invalid_argument("H264 leading bytes");
        found = true;
        const auto type = u(header) & 31U;
        if ((u(header) & 128U) || type == 0 || type > 23)
            throw std::invalid_argument("invalid H264 NAL");
        idr |= type == 5;
        i = header;
    }
    if (!found)
        throw std::invalid_argument("H264 Annex B required");
    return idr;
}
VideoUnit H264Adapter::convert(const AVPacket &packet, std::uint64_t epoch) {
    // 3. 透传首版限定 PTS=DTS；B 帧转码、无时间戳推算后续单独实现。
    if (packet.pts == AV_NOPTS_VALUE || packet.dts == AV_NOPTS_VALUE || packet.pts != packet.dts ||
        packet.size <= 0 || packet.size > 4 * 1024 * 1024)
        throw std::invalid_argument("H264 timestamps or frame size unsupported");
    auto *input = av_packet_clone(&packet);
    auto *output = av_packet_alloc();
    if (!input || !output) {
        av_packet_free(&input);
        av_packet_free(&output);
        throw std::bad_alloc();
    }
    try {
        checked(av_bsf_send_packet(m_filter, input));
        checked(av_bsf_receive_packet(m_filter, output));
        auto bytes = std::make_shared<std::vector<std::byte>>(
            reinterpret_cast<std::byte *>(output->data),
            reinterpret_cast<std::byte *>(output->data + output->size));
        VideoUnit result{bytes, av_rescale_q(output->pts, m_timeBase, AVRational{1, 1000000}),
                         epoch, hasIdr(*bytes)};
        av_packet_free(&input);
        av_packet_free(&output);
        return result;
    } catch (...) {
        av_packet_free(&input);
        av_packet_free(&output);
        throw;
    }
}
} // namespace obs::media
