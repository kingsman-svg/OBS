#pragma once
#include <QByteArray>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <deque>

namespace csn {
struct PlaybackAudio {
    QByteArray pcm;               // 48kHz、立体声、交错Float32；拥有解码/重采样后的字节。
    qint64 pts = 0;               // 文件时间线中的采样帧位置，单位1/48000秒，不使用采集QPC。
};
struct PlaybackClock {
    qint64 timeUs = 0;            // WASAPI已播放的位置，换算到媒体微秒。
    qint64 endedQpcUs = 0;        // 音频自然结束时的QPC，供较长的视频继续走单调时钟。
    quint64 underruns = 0;        // 运行中队列为空且尚未finish的补静音次数。
    bool started = false;        // IAudioClient::Start已成功，音频主时钟可使用。
    bool ended = false;          // 尾部padding已排空，设备已停止。
};
// MediaPlayer持有的音频输出QThread；端点接口只在run所在的MTA线程存在。
class WasapiPlayback final : public QThread {
    Q_OBJECT
public:
    WasapiPlayback();                         // 构造不打开默认扬声器。
    ~WasapiPlayback() override;               // stop/wait，线程归还COM后再销毁。
    bool begin(qint64 startSample = 0, bool live = false); // 清状态；直播使用200ms队列，点播保留1秒背压。
    bool push(const PlaybackAudio &packet);   // 解码线程有界入队；满1秒时等待消费，stop可唤醒。
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
    mutable QMutex m_mutex;                    // 保护队列、停止状态与时钟快照。
    QWaitCondition m_changed;                  // 唤醒预缓冲或有界入队等待，不使用逐包Qt事件。
    std::deque<PlaybackAudio> m_packets;       // 最多48000采样帧，已消费的包及时释放。
    qint64 m_queuedFrames = 0;                 // 包中尚未播放的PCM帧数，限制内存。
    qint64 m_offset = 0;                       // 队首包已消费的帧数。
    qint64 m_startSample = 0;                  // 容器声明的音轨起点，前面的空队列属于正常静音。
    PlaybackClock m_clock;                     // 输出线程发布，GUI/解码线程只取副本。
    std::atomic<float> m_volume{0.5f};         // 音量不经过文件解码队列，静音也继续推进时钟。
    bool m_accepting = false;                 // finish/stop后拒绝迟到音频。
    bool m_finishing = false;                 // 正常EOF；队列空后等待端点padding清零。
    bool m_force = false;                     // 后置音轨/纯静音区间允许建立时钟。
    bool m_live = false;                      // 直播溢出丢旧PCM，长时间缺口重新映射媒体时钟。
};
}
