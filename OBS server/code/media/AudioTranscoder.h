#pragma once
#include "MediaUnit.h"
extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
}
#include <memory>

namespace obs::media {
struct AudioTranscodeStats {
    std::uint64_t decodedSamples = 0, resampledSamples = 0, packets = 0;
    std::uint64_t resets = 0; // 格式/时间段重建次数，不把正常重采样滤波延迟算作跳变。
    int inputRate = 0;           // 最近一个解码帧的实际采样率。
    int encoderDelaySamples = 0; // libopus initial_padding，已反映在输出 packet PTS 中。
    int tailInputSamples = 0; // finish 时不足 960 的有效输入，RTP 不携带文件尾裁剪元数据。
};

// 普通类，无线程；RtmpSource 在同一读取线程持有 AAC 解码、SWR、FIFO 和 Opus 编码器。
class AudioTranscoder final {
  public:
    AudioTranscoder(const AVCodecParameters &parameters, AVRational timeBase);
    ~AudioTranscoder();
    AudioTranscoder(const AudioTranscoder &) = delete;
    AudioTranscoder &operator=(const AudioTranscoder &) = delete;
    std::vector<MediaUnit> push(const AVPacket &packet, std::uint64_t epoch);
    std::vector<MediaUnit>
    finish(std::uint64_t epoch); // 正常 EOF 排尾；stop/换流直接析构丢弃旧段。
    AudioTranscodeStats stats() const;

  private:
    struct Resources;
    std::unique_ptr<Resources> m_resources; // 仅归读取线程，析构统一释放全部 FFmpeg 资源。
};
} // namespace obs::media
