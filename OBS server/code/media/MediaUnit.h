#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace obs::media {
enum class MediaKind { Video, Audio };
// 完整 H.264 access unit 或一个 Opus 包；不借用 FFmpeg 缓冲，跨线程只读共享。
struct MediaUnit {
    std::shared_ptr<const std::vector<std::byte>> bytes;
    std::int64_t ptsUs = 0; // 源媒体时间；音频包含编码器实际报告的 lookahead 修正。
    std::uint64_t epoch = 0; // 输入重开序号，不能跨段保留参考帧/音频历史。
    bool idr = false;        // 仅视频使用，来自真实 NAL type 5。
    MediaKind kind = MediaKind::Video;
    std::int64_t durationUs = 0; // Opus 通常 20ms；尾包可能含少量编码填充。
    bool discontinuity = false; // 音频格式或时间线变化，接收会话重新等待共同 IDR 起点。
};
} // namespace obs::media
