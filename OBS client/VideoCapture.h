#pragma once
#include "CaptureSource.h"
#include <QImage>
#include <QMutex>
#include <QThread>
#include <d3d11.h>
#include <wrl/client.h>

namespace csn {
// texture 是独立 BGRA GPU 快照，不引用系统会复用的采集缓冲。
// 复制此结构可延长纹理寿命；timestamp100ns 使用 Windows QPC 时间域。
struct VideoFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    QImage preview;
    qint64 previewTimestamp100ns = 0; // 预览读回频率较低，可能早于当前 GPU 帧。
    qint64 timestamp100ns = 0;
    quint64 sequence = 0;
};

class VideoCapture final : public QThread {
    Q_OBJECT
public:
    explicit VideoCapture(QObject *parent = nullptr);
    ~VideoCapture() override;
    static QList<CaptureSource> sources(QString *warning = nullptr); // 放到 MTA 枚举线程。
    bool begin(const CaptureSource &source);
    void stop();                           // 只请求退出，正常启停不在 GUI 上 wait。
    VideoFrame latestFrame() const;         // 单帧邮箱；慢消费者取最新帧，不堆积 Qt 信号。
signals:
    void opened();
    void failed(const QString &message);
protected:
    void run() override;
private:
    void captureCamera();
    void captureDesktop();
    void publishTexture(ID3D11Texture2D *source, int width, int height, qint64 timestamp);
    CaptureSource m_source;
    mutable QMutex m_mutex;
    VideoFrame m_latest;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    qint64 m_lastPreviewTime = 0;
};
} // namespace csn
Q_DECLARE_METATYPE(csn::VideoFrame)
