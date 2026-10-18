#pragma once
#include "MediaUnit.h"
extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
}
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace obs::media {
// 普通类，无线程；仅由 RtmpSource 的读取线程构造、转换和销毁。
class H264Adapter final {
  public:
    H264Adapter(const AVCodecParameters &parameters, AVRational timeBase);
    ~H264Adapter();
    H264Adapter(const H264Adapter &) = delete;
    H264Adapter &operator=(const H264Adapter &) = delete;
    MediaUnit convert(const AVPacket &packet, std::uint64_t epoch);
    const std::string &profile() const noexcept {
        return m_profile;
    }
    // 用于验证 bitstream filter 输出完整 NAL 边界；超大/缺少 start code 的帧直接拒绝。
    static bool hasIdr(const std::vector<std::byte> &annexB);

  private:
    AVBSFContext *m_filter = nullptr; // h264_mp4toannexb，持有并按需插入 SPS/PPS。
    AVRational m_timeBase{};          // 输入流时间基；首版拒绝重排序视频。
    std::string m_profile; // avcC 中的 profile/constraints/level，用于 SDP 兼容校验。
};
} // namespace obs::media
