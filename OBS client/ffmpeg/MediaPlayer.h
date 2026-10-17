#pragma once
#include "MediaDecoder.h"
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <deque>

namespace csn {
struct PlaybackStats {
    quint64 decodedVideo = 0;          // 解码交付的视频帧数，不等同显示器扫描次数。
    quint64 audioSamples = 0;          // 解码/重采样实际PCM样本帧数。
    quint64 displayedVideo = 0;        // GUI取走并交给Renderer的帧数。
    quint64 droppedVideo = 0;          // 跳过的到期旧帧，以及直播按时间/数量裁掉的解码纹理。
    quint64 audioUnderruns = 0;        // 固定模式补零/自适应模式断粮暂停次数，实际补零样本另记。
    qint64 positionUs = 0;             // 当前媒体时钟，GUI用来显示进度。
    quint64 gpuVideo = 0;              // D3D11VA解码并在GPU转换交付的帧数。
    quint64 softwareVideo = 0;         // 软件解码/CPU转换后上传的帧数。
    quint64 readbackVideo = 0;         // 硬解成功但转换回读的帧数，单独列出性能回退。
    QString decoderNote;              // 当前路径的回退原因，显示在已有状态标签。
    qint64 openUs = 0;                 // begin到媒体探测/解码初始化就绪的墙钟耗时。
    qint64 firstDecodedUs = 0;         // begin到首张GPU/软件转换完成的视频。
    qint64 firstDeliveryUs = 0;        // begin到GUI第一次takeVideo；不包含显示器扫描。
    quint64 queuedVideo = 0;          // 当前未交付的解码纹理数。
    qint64 videoQueueSpanUs = 0;      // 视频队列媒体时间跨度，不是采集到显示延迟。
    qint64 audioQueuedUs = 0;         // PCM队列待播时长。
    qint64 audioPaddingUs = 0;        // WASAPI中最近观测到的待播时长。
    qint64 audioBufferUs = 0;         // WASAPI实际容量，驱动可能大于请求预算。
    quint64 droppedAudioSamples = 0; // 直播PCM队列裁掉的采样帧数。
    quint64 audioCatchups = 0;        // 声音线程实际跳过主动裁剪/较大缺口的次数。
    quint64 renderedAudioSamples = 0; // 解码PCM实际提交给端点，辅助识别持续补零但时钟仍走的情况。
    quint64 insertedSilenceSamples = 0; // 播放端因缺口/欠载补零，不含源素材本身的静音。
    bool adaptiveBuffer = false, buffering = false; // 是否启用自适应、是否等待恢复；纯视频同样支持。
    qint64 bufferTargetUs = 0, deliveryJitterUs = 0; // 目标缓冲与应用交付迟到平滑值。
    quint64 bufferIncreases = 0, bufferDecreases = 0, rebuffers = 0; // 本轮目标升/降及重新缓冲次数。
    qint64 rebufferUs = 0;              // 已完成的重新缓冲时长，首播不计入。
};
// Controller拥有的播放QThread；持有一个WASAPI输出线程，普通MediaDecoder只在run中存在。
class MediaPlayer final : public QThread {
    Q_OBJECT
public:
    explicit MediaPlayer(QObject *parent = nullptr); // 构造只绑定音频线程错误，不打开媒体。
    ~MediaPlayer() override;               // stop/wait，保证解码和音频线程先结束。
    bool begin(const QString &source, bool audioOutput = true, bool live = false,
               VideoDecodeMode mode = VideoDecodeMode::Automatic, bool lowLatency = true,
               bool adaptiveBuffer = true); // 低延迟直播默认自适应；末参数false保留固定预算对照。
    void stop();                            // GUI关闭交付入口、唤醒背压并停止声音，常规停止不wait。
    void setVolume(float volume);           // GUI快速转交输出线程的原子音量。
    PlaybackVideo takeVideo();              // GUI按主时钟取一张到期帧，空纹理表示继续等，不清画面。
    PlaybackStats stats() const;            // GUI取统计副本及当前时钟。
    qint64 clockUs() const;                  // 音频实际进度为主，静音输出关闭/视频尾部用QPC。
signals:
    void opened(const csn::MediaInfo &info); // 解码器与本轮输出入口已初始化，排队回GUI。
    void failed(const QString &message);     // 读包/解码/音频失效，资源回收后通知GUI。
    void completed(bool endOfFile);          // true自然结束，false用户停止；之后QThread发finished。
protected:
    void run() override;                    // open → 读包/解码有界交付 → EOF排空 → 等声音/视频结束 → 释放。
private:
    void pushVideo(const PlaybackVideo &frame); // 点播满16帧背压；直播按时间/数量裁剪，保留音频时钟余量。
    void pushAudio(const PlaybackAudio &packet); // 播放线程统计并向音频线程交付，关闭声音时只解码。
    void checkAudioError() const;            // 播放线程读锁存的音频错误，转成统一退出流程。
    bool scheduleVideo(qint64 nowUs, qint64 &clockUs); // GUI持队列锁调用；无音频输出时预缓冲/冻结/追赶视频时钟。
    mutable QMutex m_mutex;                  // 保护视频队列、本轮参数及统计。
    QWaitCondition m_changed;                // GUI取帧/stop/音频失败唤醒背压等待。
    std::deque<PlaybackVideo> m_video;        // 最多16张未到期纹理，点播背压保留未来画面。
    WasapiPlayback m_audio;                  // 本实例拥有的独立输出QThread；端点不在解码线程使用。
    PlaybackBuffer m_videoBuffer;            // 无音频输出时使用的同一策略；在视频队列锁内串行访问。
    QString m_source;                        // 本轮本地路径或HTTP(S)地址，运行中不可修改。
    QString m_audioError;                    // 音频线程直连回调只锁存文本，不碰GUI。
    PlaybackStats m_stats;                   // 解码/GUI按锁更新，时钟实时计算。
    qint64 m_wallOriginUs = 0;               // 纯视频/不输出音频时的QPC起点。
    qint64 m_mediaEndUs = 0;                 // 解码音频/视频的最晚结束点，关闭声音也保持完整媒体时长。
    qint64 m_videoEndUs = 0;                 // 单独记录最后视频结束点，纯视频调度不能被忽略的音轨拖着前进。
    qint64 m_audioEndSamples = -1;           // 已知音轨尾部到达即关闭输出入口，不等较长视频EOF。
    bool m_audioRequested = true;            // 用户本轮是否输出声音。
    bool m_audioActive = false;              // 媒体存在音轨且本轮实际启用WASAPI。
    bool m_accepting = false;                // stop后不再交付任何迟到画面。
    bool m_decodeEof = false;                // 全部解码尾部已交付，主时钟最多走到实际媒体结束点。
    bool m_live = false;                     // 直播有界丢旧画面，点播继续背压；运行中不改模式。
    bool m_lowLatency = false;               // 仅直播短探测；自适应视频窗口500ms/16帧，固定对照300ms。
    bool m_adaptiveBuffer = false;           // 仅直播低延迟；有声用WASAPI时钟，无声用视频策略。
    bool m_videoBuffering = false, m_videoStarted = false; // 无声路径等待状态，不通过新线程/定时器驱动。
    qint64 m_videoHeldUs = 0, m_videoRebufferSinceUs = -1; // 无声暂停时冻结媒体时间，恢复重新锚定QPC。
    qint64 m_startedUs = 0;                 // begin的QPC时间，用于本轮打开/首帧测量，与媒体时钟分开。
    VideoDecodeMode m_decodeMode = VideoDecodeMode::Automatic; // 本轮硬件优先/软件/严格硬件，不跨轮复用设备。
    qint64 m_liveClockBaseUs = -1;           // 关闭声音时首帧建立直播时钟，GUI不追随无限积压。
};
}
