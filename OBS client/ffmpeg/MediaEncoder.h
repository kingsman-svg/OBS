#pragma once
#include "MediaTimeline.h"
#include <QString>
#include <functional>
#include <memory>
struct AVCodecContext;
struct AVPacket;

namespace csn {
struct RecordingSettings {
    QString path;             // 本步仅本地 .mp4；不把信令开播误当作媒体推流。
    bool video = true;        // 来自本轮视频源选择。
    bool audio = true;        // 来自本轮麦克风/回环选择。
    int width = 1280;         // 编码尺寸固定，源尺寸变化只影响GPU缩放。
    int height = 720;         // 偶数尺寸，默认720p。
    int videoBitrate = 4000000; // H.264目标码率，bit/s。
    int audioBitrate = 128000;  // AAC目标码率，bit/s。
};
struct EncodingStats {
    quint64 videoFrames = 0;   // 已送入视频编码器的同步帧数。
    quint64 audioSamples = 0;  // 已接收的混音采样帧数，每帧包含左右声道，不把两声道重复计数。
    quint64 silentSamples = 0; // 时间缺口内补入的静音采样帧数。
    quint64 packets = 0;       // 编码器输出并由封装层接受的包数。
    quint64 bytes = 0;         // 编码包字节，不含MP4容器开销。
    quint64 droppedVideo = 0;  // 线程输入队列溢出丢掉的视频帧。
    quint64 droppedAudio = 0;  // 线程输入队列溢出丢掉的音频包。
};
// 普通编码类，无线程；由 MediaPublisher::run 独占，图像保持GPU输入，音频使用CPU PCM。
class MediaEncoder final {
public:
    using PacketSink = std::function<void(AVPacket *, const AVCodecContext *)>; // 包只在回调期间有效。
    MediaEncoder();                       // 不在构造时访问设备或打开编码器。
    ~MediaEncoder();                      // 释放上下文/显存；正常尾部由finish显式处理。
    void open(const RecordingSettings &settings, ID3D11Device *device, bool globalHeader, PacketSink sink); // 工作线程初始化。
    void pushVideo(const TimedVideoFrame &frame); // GPU绘制到硬件AVFrame，保留1/30的PTS。
    void pushAudio(const TimedAudioPacket &packet); // 10ms交错PCM转平面FIFO，按AAC帧长聚合。
    void finish();                        // FIFO尾部 → send(nullptr) → receive至EOF；幂等。
    const AVCodecContext *videoContext() const; // 封装层读取参数，不拥有上下文。
    const AVCodecContext *audioContext() const; // 未启用时返回nullptr。
    const EncodingStats &stats() const;   // 仅工作线程读取；外部统计由Publisher复制。
private:
    struct Resources;                    // FFmpeg/GPU资源与辅助方法隐藏在cpp，统一RAII释放。
    std::unique_ptr<Resources> m_resources; // 本实例独占资源，禁止并行编码调用。
};
}
Q_DECLARE_METATYPE(csn::EncodingStats)
