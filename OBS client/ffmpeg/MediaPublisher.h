#pragma once
#include "MediaEncoder.h"
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <deque>

namespace csn {
// Controller拥有的编码/封装线程；GUI只调用begin/push/stop，FFmpeg资源只在run存在。
class MediaPublisher final : public QThread {
    Q_OBJECT
public:
    explicit MediaPublisher(QObject *parent = nullptr); // 构造只建立状态，不访问GPU/磁盘。
    ~MediaPublisher() override;                         // 请求停止并等待资源归还。
    bool begin(const RecordingSettings &settings);      // GUI保存本轮参数、清空队列，再start线程。
    void pushVideo(const TimedVideoFrame &frame);        // 快速复制纹理引用，溢出丢最旧帧。
    void pushAudio(const TimedAudioPacket &packet);      // 快速复制PCM，溢出后的时间缺口由编码器补零。
    void stop();                                        // 关闭入口，排空已接收数据并封存MP4；不阻塞调用者。
    void abort();                                       // 放弃本轮文件，唤醒线程；用于明确取消/测试。
    EncodingStats stats() const;                        // 加锁复制统计，不暴露工作线程上下文。
signals:
    void opened();                                      // 编码器和MP4头初始化成功，排队回GUI。
    void failed(const QString &message);                 // 失败删除临时文件，采集/预览可继续。
    void completed(const QString &path, const csn::EncodingStats &stats); // 空路径表示没有可保存媒体。
protected:
    void run() override;                                // 等首帧 → 初始化 → 消费队列 → 排空 → trailer → 改名。
private:
    void publishStats(const EncodingStats &stats);        // 合并工作线程编码统计与入口丢弃计数。
    mutable QMutex m_mutex;                              // 只保护参数/队列/状态，不包围编码和写文件。
    QWaitCondition m_ready;                              // 无数据时等待，push/stop/abort均唤醒。
    RecordingSettings m_settings;                        // begin到finished之间不变。
    std::deque<TimedVideoFrame> m_video;                  // 最多16帧，纹理不被过早释放。
    std::deque<TimedAudioPacket> m_audio;                 // 最多50个10ms包，避免延迟无限增长。
    EncodingStats m_stats;                               // GUI可读快照，包括队列溢出计数。
    bool m_accepting = false;                            // stop后迟到信号不再入队。
    bool m_finishing = false;                            // 正常停止，现有队列继续消费。
    bool m_aborted = false;                              // 明确放弃，队列和临时文件一起丢弃。
};
}
