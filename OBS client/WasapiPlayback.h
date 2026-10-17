#pragma once
#include "PlaybackBuffer.h"
#include <QByteArray>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <deque>

struct IAudioClient;
struct IAudioRenderClient;
struct IAudioClock;

namespace csn {
struct PlaybackAudio {
    QByteArray pcm;               // 48kHz、立体声、交错Float32；拥有解码/重采样后的字节。
    qint64 pts = 0;               // 文件时间线中的采样帧位置，单位1/48000秒，不使用采集QPC。
};
struct PlaybackClock {
    qint64 timeUs = 0;            // WASAPI已播放的位置，换算到媒体微秒。
    qint64 endedQpcUs = 0;        // 音频自然结束时的QPC，供较长的视频继续走单调时钟。
    quint64 underruns = 0;        // 固定模式补零次数；自适应模式实际断粮进入重新缓冲的次数。
    bool started = false;        // IAudioClient::Start已成功，音频主时钟可使用。
    bool ended = false;          // 尾部padding已排空，设备已停止。
    qint64 queuedUs = 0;         // 应用PCM队列剩余时长，不含设备padding。
    qint64 deviceBufferUs = 0;   // GetBufferSize返回的实际端点容量，不把请求值当实测值。
    qint64 devicePaddingUs = 0;  // 最近一次GetCurrentPadding，供观察已提交但尚未播放的数据。
    qint64 prebufferUs = 0;      // 本轮应用预缓冲预算。
    quint64 droppedSamples = 0; // 直播队列裁掉的未播放采样帧数。
    quint64 catchups = 0;       // 输出线程实际跳过主动裁剪/较大PTS缺口的次数。
    quint64 renderedSamples = 0; // 实际PCM交给端点的采样帧数，音量为0也计入，不是设备确认。
    quint64 silenceSamples = 0;  // 应用因缺口/欠载补的采样帧数，不把源素材静音计算在内。
    bool adaptive = false, buffering = false; // 本轮是否自适应、当前是否等待足量媒体；视频跟随冻结时钟。
    qint64 targetUs = 0, deliveryJitterUs = 0; // 当前目标与交付迟到平滑值，单位微秒。
    quint64 bufferIncreases = 0, bufferDecreases = 0; // 目标调整次数，区别于丢弃次数。
    quint64 rebuffers = 0;       // 运行中断粮/目标提高后的重新缓冲，启动预缓冲不计入。
    qint64 rebufferUs = 0;       // 已完成的重新缓冲等待时长，不含启动及正常播放。
};
// MediaPlayer持有的音频输出QThread；端点接口只在run所在的MTA线程存在。
class WasapiPlayback final : public QThread {
    Q_OBJECT
public:
    WasapiPlayback();                         // 构造不打开默认扬声器。
    ~WasapiPlayback() override;               // stop/wait，线程归还COM后再销毁。
    bool begin(qint64 startSample = 0, bool live = false, bool waitForVideo = false, bool lowLatency = true,
               bool adaptiveBuffer = true); // 低延迟直播默认自适应；false保留固定预算用于对照。
    void videoReady();                        // 首张转换后画面就绪；点播200ms，低延迟直播50ms预缓冲。
    bool push(const PlaybackAudio &packet);   // 点播满1秒背压；直播按本轮预算裁旧PCM，stop可唤醒。
    void forceStart();                        // 视频队列先满时允许先播放静音，防止等待后置音轨死锁。
    void finish();                            // 解码EOF后关闭入口，排空PCM及设备padding。
    void stop();                              // 用户停止立即丢队列、请求退出，不在GUI等待。
    void setVolume(float volume);             // 0～1，GUI线程可实时调整，输出线程应用到PCM。
    PlaybackClock clock() const;              // 快速复制时钟快照，不跨线程调用WASAPI。
signals:
    void failed(const QString &message);       // 端点失效/初始化/写出失败，MediaPlayer停止本轮。
protected:
    void run() override;                       // COM → 默认输出 → Initialize → 预填/Start → 写出/时钟 → Stop。
private:
    void renderAdaptive(IAudioClient *client, IAudioRenderClient *render, IAudioClock *clock,
                        void *event, quint32 capacity, quint64 frequency); // run内调用；暂停/恢复/追赶只在输出线程访问端点。
    void dropFrames(qint64 count);           // 调用方持锁；按采样裁掉尚未提交PCM，保留下一段原PTS。
    mutable QMutex m_mutex;                    // 保护队列、停止状态与时钟快照。
    QWaitCondition m_changed;                  // 唤醒预缓冲或有界入队等待，不使用逐包Qt事件。
    std::deque<PlaybackAudio> m_packets;       // 最多48000采样帧，已消费的包及时释放。
    qint64 m_queuedFrames = 0;                 // 包中尚未播放的PCM帧数，限制内存。
    qint64 m_offset = 0;                       // 队首包已消费的帧数。
    qint64 m_startSample = 0;                  // 容器声明的音轨起点，前面的空队列属于正常静音。
    PlaybackClock m_clock;                     // 输出线程发布，GUI/解码线程只取副本。
    PlaybackBuffer m_buffer;                  // 在队列锁内更新；push记录交付，run决定目标与消费动作。
    std::atomic<float> m_volume{0.5f};         // 音量不经过文件解码队列，静音也继续推进时钟。
    bool m_accepting = false;                 // finish/stop后拒绝迟到音频。
    bool m_finishing = false;                 // 正常EOF；队列空后等待端点padding清零。
    bool m_force = false;                     // 任一队列先满时允许建立时钟，避免媒体读包互相等待。
    bool m_live = false;                      // 直播溢出丢旧PCM，长时间缺口重新映射媒体时钟。
    bool m_lowLatency = false;                // 本轮固定；端点待播30ms，自适应50～150ms目标/300ms上限。
    bool m_adaptive = false;                  // 仅低延迟直播启用，固定模式和点播仍走原输出流程。
    bool m_audioGap = false;                  // 主动丢PCM必须通知输出线程跳过缺口，不能补零后再追逐旧时钟。
    bool m_videoReady = true;                 // 有视频时先等解码/转换初始化完成，避免启动阶段补静音。
    qint64 m_prebufferFrames = 4800;          // 普通音频100ms、视频点播200ms、低延迟直播50ms。
};
}
