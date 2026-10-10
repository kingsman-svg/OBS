#pragma once
#include "AudioPacket.h"
#include "FaceDetection.h"
#include <QList>
#include <deque>

namespace csn {
// 保留来源时间，PTS 另外描述这一帧应出现在输出时间线的哪个位置。
struct TimedVideoFrame {
    FaceFrame source;                // 同一采集纹理与检测结果，重复输出也不修改采集时间。
    qint64 pts = 0;                  // 时间基 1/30 秒，供后续视频编码器使用。
    bool repeated = false;           // 静止/缺帧时复用上一张图，PTS 仍继续推进。
    bool discontinuity = false;      // 积压跳过、输入溢出或迟到，后续编码须保留时间间隔。
};
struct TimedAudioPacket {
    AudioPacket source;              // 混音后的 48kHz 立体声 Float32，每包 480 个采样帧。
    qint64 pts = 0;                  // 时间基 1/48000 秒；包长为 480，而非视频帧长。
};
struct MediaBatch {
    QList<TimedVideoFrame> video;    // 本次到期视频帧；交付方只做快速入队。
    QList<TimedAudioPacket> audio;   // 本次到期音频包；封装时由时间戳交错写入。
};
struct MediaSyncStats {
    quint64 videoFrames = 0;         // 已调度的视频帧数，包含重复画面。
    quint64 repeatedVideo = 0;       // 复用旧画面的次数，便于检查静止 WGC 或处理速度。
    quint64 droppedVideo = 0;        // 迟到、乱序、输入溢出及调度跳过的计数。
    quint64 audioPackets = 0;        // 已交付的同步音频包数。
    quint64 droppedAudio = 0;        // 过时/重叠/溢出音频包的计数。
};

// 编码前同步：同一 QPC 原点、有界队列、固定视频节奏。调用方串行使用，无线程/MVC。
class MediaTimeline final {
public:
    static constexpr int VideoRate = 30;       // 首期固定 30fps，视频 PTS 单位。
    static constexpr int AudioRate = 48000;    // 与 AudioMixer 输出一致。
    static constexpr qint64 Delay100ns = 1200000; // 总等待 120ms，已包含混音的 80ms。
    void begin(qint64 origin100ns, bool video, bool audio); // 每轮记录共同起点并重置全部状态。
    void clear();                             // 退出/异常立即丢弃缓存和纹理，不输出尾部。
    bool pushVideo(const FaceFrame &frame);    // 接收原帧或同帧检测结果，保留来源时间。
    bool pushAudio(const AudioPacket &packet); // 仅接收混音输出，校验格式并换算 PTS。
    MediaBatch takeFrames(qint64 now100ns);    // 按 QPC 到期交付，常规最多追赶 200ms。
    MediaBatch finish();                      // 普通停止排空已接受数据，最多保留 500ms。
    void disableVideo();                      // 视频失败后撤掉画面路，音频仍按原时间轴运行。
    void disableAudio();                      // 所有音频源失败时撤掉声音路，视频不等待它。
    bool active() const { return m_active; }   // begin 至 finish/clear 的这一轮仍存在。
    qint64 origin100ns() const { return m_origin; } // 两路 PTS 共用的绝对 QPC 起点。
    const MediaSyncStats &stats() const { return m_stats; } // 本轮输出与丢弃统计。
private:
    MediaBatch takeUntil(qint64 relative100ns, bool draining); // 共用调度；停止时只排空有界尾部。
    qint64 m_origin = 0;                       // 本轮 QPC 原点；不使用 GPU 完成时间替代它。
    qint64 m_lastNow = 0;                      // 防止调用方时钟回退造成重复调度。
    qint64 m_nextVideo = 0;                    // 下一个 1/30 秒时间格，按绝对时钟计算。
    qint64 m_lastVideoTime = -1;               // 已交付视频时间格的绝对 QPC 时间。
    qint64 m_lastVideoInput = -1;              // 拒绝重复/倒序采集或检测结果。
    qint64 m_audioEnd = -1;                    // 最后接受音频包的结束 PTS，检测重叠及缺口。
    quint64 m_lastSequence = 0;                // 上次输出纹理序号，用于统计重复帧。
    bool m_active = false;                     // clear 后拒绝任何迟到输入。
    bool m_videoEnabled = false;              // 本轮存在的视频路，失败时可单独禁用。
    bool m_audioEnabled = false;              // 本轮存在的混音输出路。
    bool m_videoGap = false;                   // 跳过时间格后标记下一帧不连续。
    bool m_audioGap = false;                   // 丢弃包后标记下一包不连续。
    std::deque<FaceFrame> m_video;             // 最多 16 张待调度纹理，不持有无界 GPU 缓存。
    std::deque<TimedAudioPacket> m_audio;      // 最多 50 个 10ms 包，即 500ms PCM。
    FaceFrame m_current;                      // 最近可用于重复输出的同帧纹理/框点。
    MediaSyncStats m_stats;                    // 清轮时归零，正常 finish 后保留供展示。
};
} // namespace csn
Q_DECLARE_METATYPE(csn::TimedVideoFrame)
Q_DECLARE_METATYPE(csn::TimedAudioPacket)
