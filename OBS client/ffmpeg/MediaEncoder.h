#pragma once
#include "MediaTimeline.h"
#include "AdaptiveBitrate.h"
#include <QString>
#include <functional>
#include <memory>
struct AVCodecContext;
struct AVPacket;

namespace csn {
struct RecordingSettings {
    QString path;             // 本地.mp4路径或RTMP地址；协议由live明确选择。
    bool live = false;        // true用FLV/RTMP，false保留临时MP4安全封存。
    bool lowLatency = true;   // 仅直播生效：200ms VBV/1秒GOP/40ms交织；false保留旧预算供对照或回退。
    bool video = true;        // 来自本轮视频源选择。
    bool audio = true;        // 来自本轮麦克风/回环选择。
    int width = 1280;         // 编码尺寸固定，源尺寸变化只影响GPU缩放。
    int height = 720;         // 偶数尺寸，默认720p。
    int videoBitrate = 4000000; // H.264目标码率，bit/s。
    bool adaptiveBitrate = false; // 仅直播视频支持；开启后videoBitrate作为初始值与上限。
    int minimumVideoBitrate = 400000; // 自动下限，手动模式不使用。
    int audioBitrate = 128000;  // AAC目标码率，bit/s。
    int liveQueueLimitMs = 300; // 直播队列高水位：媒体跨度或入队等待超过它时追赶；不代表端到端延迟。
    int liveQueueTargetMs = 100; // 触发后保留的时间窗口，必须小于高水位；本地录制不使用。
};
struct EncodingStats {
    quint64 videoFrames = 0;   // 已送入视频编码器的同步帧数。
    quint64 audioSamples = 0;  // 已接收的混音采样帧数，每帧包含左右声道，不把两声道重复计数。
    quint64 silentSamples = 0; // 时间缺口内补入的静音采样帧数。
    quint64 packets = 0;       // 编码器输出并由封装层接受的包数。
    quint64 bytes = 0;         // 编码包字节，不含MP4/FLV容器或网络开销。
    quint64 droppedVideo = 0;  // 线程输入队列容量/过期丢掉的视频帧总数。
    quint64 droppedAudio = 0;  // 线程输入队列容量/过期丢掉的音频包总数。
    quint64 expiredVideo = 0, expiredAudio = 0; // 上述丢弃中过期/落后于追赶截止点的数量。
    quint64 liveCatchups = 0;  // 高水位触发的队列裁剪次数，一次可同时处理音视频。
    quint64 audioResyncs = 0;  // 编码线程实际执行的主动音频追赶次数，普通缺口仍补零。
    quint64 skippedAudioSamples = 0; // 主动跳过的音频时间缺口及未编码FIFO样本，不计为补静音。
    int requestedVideoBitrate = 0; // 用户最新请求，等待工作线程提交时可与编码器目标不同。
    int targetVideoBitrate = 0;    // 工作线程策略/手动生成的计划目标，下一视频帧才提交。
    bool automaticBitrate = false; // 本轮已接收的模式；GUI待处理请求另有pending标志。
    bool bitrateControlPending = false; // 最新整份配置仍在参数邮箱中，IO阻塞时不冒充已接收。
    quint64 bitrateReductions = 0, bitrateIncreases = 0; // 本轮自动下降/回升次数。
    BitrateState bitrateState = BitrateState::Manual; // 策略原因，界面解释保持或调整。
    double bitrateWriteLoad = 0;   // EWMA写出占时，非网络带宽利用率。
    int encoderVideoBitrate = 0;   // 已随视频帧提交的编码器目标，不冒充驱动确认或实际带宽。
    quint64 videoPayloadBitrate = 0; // 最近最多1秒媒体PTS内的视频压缩包码率，不含音频/封装。
    qint64 videoRateWindowMs = 0;  // 视频码率的媒体统计窗口，首帧不足1秒时明确显示。
    quint64 outputBitrate = 0;     // 最近1秒墙钟内RTMP交给底层IO的码率，不等于接收端吞吐或可用带宽。
    quint64 outputBytes = 0;       // RTMP媒体写出字节（含封装），不含连接/文件头。
    quint64 writeCalls = 0;        // 媒体包交织写出+显式flush的调用次数。
    quint64 writeFailures = 0;     // 上述调用抛错次数；失败前也保留耗时与部分字节。
    qint64 lastWriteUs = 0;        // 最近一次完整写出耗时，微秒。
    qint64 maxWriteUs = 0;         // 本轮最长写出耗时。
    qint64 totalWriteUs = 0;       // 本轮写出耗时累计，可计算平均值。
    qint64 currentWriteUs = 0;     // 当前未返回写出已经等待多久，stats读取时即时计算。
    bool writing = false;         // 工作线程处于媒体写出调用中。
    qint64 lastVideoEncodeUs = 0;  // 最近视频GPU绘制/编码调用耗时，已扣除本次封装/IO写出。
    qint64 totalVideoEncodeUs = 0; // 本轮视频GPU绘制/编码累计耗时，不包含网络阻塞。
    quint64 queuedVideo = 0, queuedAudio = 0; // 当前待编码/发送的原始媒体队列长度。
    quint64 peakQueuedVideo = 0, peakQueuedAudio = 0; // 本轮队列峰值。
    qint64 videoQueueSpanMs = 0, audioQueueSpanMs = 0; // 队头到队尾的媒体时间跨度，包含最后一帧/包。
    qint64 oldestVideoWaitMs = 0, oldestAudioWaitMs = 0; // 队头在应用队列等待的墙钟时间，不含上游处理。
    int encoderVbvMs = 0;         // 当前编码目标对应的VBV容量预算，不是实测延迟。
    int encoderGopFrames = 0;     // 配置的关键帧间隔上限；追赶/重配置仍可提前插入IDR。
    int muxInterleaveMs = 0;      // 交织队列时间戳跨度阈值，不是墙钟超时。
    qint64 connectUs = 0;         // avio_open2的TCP/RTMP建连耗时，不含编码器初始化。
    qint64 readyUs = 0;           // run入口到编码/封装头就绪，包含等待首帧。
    qint64 firstMediaUs = 0;      // run入口到首次媒体IO写出；0表示尚未发生。
};
// 普通编码类，无线程；由 MediaPublisher::run 独占，图像保持GPU输入，音频使用CPU PCM。
class MediaEncoder final {
public:
    using PacketSink = std::function<void(AVPacket *, const AVCodecContext *)>; // 包只在回调期间有效。
    MediaEncoder();                       // 不在构造时访问设备或打开编码器。
    ~MediaEncoder();                      // 释放上下文/显存；正常尾部由finish显式处理。
    void open(const RecordingSettings &settings, ID3D11Device *device, bool globalHeader, PacketSink sink); // 工作线程初始化。
    void pushVideo(const TimedVideoFrame &frame); // GPU绘制到硬件AVFrame，保留1/30的PTS。
    void pushAudio(const TimedAudioPacket &packet, bool liveCatchup = false); // 主动直播追赶跳过缺口/FIFO；普通10ms包仍补零。
    void setVideoBitrate(int bitsPerSecond); // 仅编码线程调用；下张视频帧触发FFmpeg/NVENC动态重配置。
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
