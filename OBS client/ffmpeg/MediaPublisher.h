#pragma once
#include "MediaEncoder.h"
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <deque>
#include <atomic>

namespace csn {
// Controller拥有的编码/封装线程；GUI只调用begin/push/stop，FFmpeg资源只在run存在。
class MediaPublisher final : public QThread {
    Q_OBJECT
public:
    explicit MediaPublisher(QObject *parent = nullptr); // 构造只建立状态，不访问GPU/磁盘。
    ~MediaPublisher() override;                         // 请求停止并等待资源归还。
    bool begin(const RecordingSettings &settings);      // GUI保存本轮参数、清空队列，再start线程。
    void pushVideo(const TimedVideoFrame &frame);        // 快速复制纹理引用，直播按数量/时间裁剪旧帧。
    void pushAudio(const TimedAudioPacket &packet);      // 快速复制PCM；直播过期跳过，录制缺口仍补零。
    bool requestVideoBitrate(int bitsPerSecond);         // GUI/策略只更新单值参数邮箱；未运行/停播/非法值返回false。
    void stop();                                        // MP4排空封存；直播取消网络IO并退出，不阻塞调用者。
    void abort();                                       // 放弃本轮文件，唤醒线程；用于明确取消/测试。
    EncodingStats stats() const;                        // 加锁复制统计，不暴露工作线程上下文。
signals:
    void opened();                                      // 编码器与MP4/FLV头初始化成功，排队回GUI。
    void transmitting();                                // RTMP头之后已写出媒体字节，Controller才通知开播。
    void failed(const QString &message);                 // 失败删除临时文件，采集/预览可继续。
    void completed(const QString &path, const csn::EncodingStats &stats); // 空路径表示没有可保存媒体。
protected:
    void run() override;                                // 等首帧 → 编码/封装 → 文件排空封存或网络取消退出。
private:
    void publishStats(const EncodingStats &stats);        // 合并工作线程编码统计与入口丢弃计数。
    void finishWrite(qint64 startedUs, quint64 bytes, bool failed); // 写出结束/抛错均记录耗时，IO期间不持锁。
    void trimLiveQueues(qint64 nowUs);                  // 调用方持锁；入队/出队前检查，音频追赶同步推进视频截止点。
    struct QueuedVideo { TimedVideoFrame value; qint64 enteredUs; }; // 一帧及进入应用队列的单调时间。
    struct QueuedAudio { TimedAudioPacket value; qint64 enteredUs; }; // 一包PCM及其入队时间。
    struct OutputSample { qint64 endedUs; quint64 bytes; };          // 最近1秒RTMP本地IO写出样本。
    mutable QMutex m_mutex;                              // 只保护参数/队列/状态，不包围编码和写文件。
    QWaitCondition m_ready;                              // 无数据时等待，push/stop/abort均唤醒。
    RecordingSettings m_settings;                        // begin到finished之间不变。
    std::deque<QueuedVideo> m_video;                      // 最多16帧，纹理不被过早释放；附带可观测入队时间。
    std::deque<QueuedAudio> m_audio;                      // 最多50个10ms包，附带可观测入队时间。
    std::deque<OutputSample> m_outputSamples;             // 写出时裁掉1秒之前的样本，GUI只复制聚合值。
    EncodingStats m_stats;                               // GUI可读快照，包括队列溢出计数。
    int m_pendingVideoBitrate = 0;                       // 最新未消费请求；0表示空，参数不形成无界队列。
    qint64 m_writeStartedUs = 0;                         // 写出开始时间；stats可在IO阻塞时观察已等待时间。
    qint64 m_videoCutoffUs = 0, m_audioCutoffUs = 0;      // 本轮单调前进的媒体截止点，拒绝迟到旧数据；单位微秒。
    bool m_videoGap = false, m_audioGap = false;         // 裁剪后交给下一张视频/下一包音频，不能在队列暂空时丢失。
    bool m_accepting = false;                            // stop后迟到信号不再入队。
    bool m_finishing = false;                            // 正常停止，现有队列继续消费。
    bool m_aborted = false;                              // 明确放弃，队列和临时文件一起丢弃。
    std::atomic<bool> m_cancelIo{false};                 // GUI停止可中断FFmpeg网络连接/写出，不持有队列锁。
};
}
