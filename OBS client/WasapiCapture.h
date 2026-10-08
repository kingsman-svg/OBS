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
    int sampleRate = 0;
    int channels = 0;
    qint64 timestamp100ns = 0;          // 首个采样帧的 QPC 时间；与视频使用同一时间域。
    bool discontinuity = false;
    bool timestampEstimated = false;
};

// 麦克风与系统回环共用一个类，区别只有枚举方向及 LOOPBACK 标志。
class WasapiCapture final : public QThread {
    Q_OBJECT
public:
    explicit WasapiCapture(QObject *parent = nullptr);
    ~WasapiCapture() override;
    static QList<CaptureSource> devices(bool loopback);
    static AudioPacket decode(const uchar *data, quint32 frames, const WAVEFORMATEX &format,
                              quint32 flags, quint64 timestamp100ns);
    bool begin(const CaptureSource &source);
    void stop();
    QList<AudioPacket> takePackets();    // 最多保留 50 包，消费过慢时标记 discontinuity。
    float level() const;
signals:
    void opened();
    void failed(const QString &message);
protected:
    void run() override;
private:
    CaptureSource m_source;
    mutable QMutex m_mutex;
    QList<AudioPacket> m_packets;
    float m_level = 0;
    qint64 m_lastPacketTime = 0;
};
} // namespace csn
Q_DECLARE_METATYPE(csn::AudioPacket)
