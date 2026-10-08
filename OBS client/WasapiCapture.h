#pragma once
#include "CaptureSource.h"
#include <QByteArray>
#include <QMutex>
#include <QThread>
#include <windows.h>
#include <mmreg.h>

namespace csn {
struct AudioPacket {
    QByteArray pcm;                    // 交错 Float32，小端，采样率/声道保留设备原始值。
    int sampleRate = 0;                // 设备每秒采样帧数，当前尚未重采样。
    int channels = 0;                  // 每个采样帧的声道数，当前尚未混音。
    qint64 timestamp100ns = 0;          // 首个采样帧的 QPC 时间；与视频使用同一时间域。
    bool discontinuity = false;        // 设备中断或应用队列丢包，消费者须重置连续性假设。
    bool timestampEstimated = false;   // 设备时间戳无效，已用读取时的 QPC 估计。
};

// 麦克风与系统回环共用一个类，区别只有枚举方向及 LOOPBACK 标志。
class WasapiCapture final : public QThread {
    Q_OBJECT
public:
    explicit WasapiCapture(QObject *parent = nullptr); // 仅创建线程对象，不打开端点。
    ~WasapiCapture() override;                         // 请求退出并等待，归还所有 COM 资源。
    static QList<CaptureSource> devices(bool loopback); // MTA 线程枚举输入端点/输出回环端点。
    // 复制设备缓冲并转成交错 Float32；支持 PCM/IEEE Float，不执行重采样。
    static AudioPacket decode(const uchar *data, quint32 frames, const WAVEFORMATEX &format,
                              quint32 flags, quint64 timestamp100ns);
    bool begin(const CaptureSource &source); // 校验音频类型，清空队列后启动线程。
    void stop();                            // 只发中断请求，GUI 正常停止不阻塞等待。
    QList<AudioPacket> takePackets();    // 最多保留 50 包，消费过慢时标记 discontinuity。
    float level() const;                    // 最近一包的 RMS，超过 500ms 没数据则返回 0。
signals:
    void opened();                          // IAudioClient 已启动，排队通知 GUI。
    void failed(const QString &message);     // 非主动停止造成的设备/格式错误。
protected:
    void run() override;                    // 在线程内初始化 COM、打开端点并循环取包。
private:
    CaptureSource m_source;                 // begin 前设定，线程运行期间只读。
    mutable QMutex m_mutex;                 // 保护数据队列、电平和最后收包时间。
    QList<AudioPacket> m_packets;           // 最多 50 包，takePackets 一次取走并清空。
    float m_level = 0;                      // 当前归一化 RMS 电平，范围 0～1。
    qint64 m_lastPacketTime = 0;             // 最近取包的 QPC 时间，100ns，用于电平超时。
};
} // namespace csn
Q_DECLARE_METATYPE(csn::AudioPacket)
