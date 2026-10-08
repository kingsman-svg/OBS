#pragma once
#include "CaptureSource.h"
#include <QMutex>
#include <QThread>
#include <d3d11.h>
#include <wrl/client.h>

namespace csn {
// texture 是独立 BGRA GPU 快照，不引用系统会复用的采集缓冲。
// 复制此结构可延长纹理寿命；timestamp100ns 使用 Windows QPC 时间域。
struct VideoFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; // 独立单层 BGRA8 ShaderResource 纹理。
    qint64 timestamp100ns = 0;                     // 采集时刻，QPC 时间域，单位 100ns。
    quint64 sequence = 0;                         // 本轮从 1 递增，消费者用于过滤重复帧。
};

class VideoCapture final : public QThread {
    Q_OBJECT
public:
    explicit VideoCapture(QObject *parent = nullptr); // 仅创建线程对象，不打开设备。
    ~VideoCapture() override;                         // 请求停止并等线程退出，确保资源已归还。
    static QList<CaptureSource> sources(QString *warning = nullptr); // MTA 线程枚举桌面目标/摄像头。
    bool begin(const CaptureSource &source); // 校验源、清空邮箱，再启动工作线程。
    void stop();                           // 只请求退出，正常启停不在 GUI 上 wait。
    VideoFrame latestFrame() const;         // 单帧邮箱；慢消费者取最新帧，不堆积 Qt 信号。
signals:
    void opened();                        // 系统采集会话已启动，排队通知 GUI。
    void failed(const QString &message);   // 非主动停止引起的初始化/采集错误。
protected:
    void run() override;                   // 工作线程 MTA 初始化、分派采集及统一错误清理。
private:
    void captureCamera();                  // MediaCapture + MediaFrameReader，最新彩色帧转 BGRA。
    void captureDesktop();                 // WGC 捕获 HWND/HMONITOR，处理缩放及目标关闭。
    void publishTexture(ID3D11Texture2D *source, int width, int height, qint64 timestamp); // GPU 复制快照到邮箱。
    CaptureSource m_source;                // 启动前设定，线程运行期间只读。
    mutable QMutex m_mutex;                // 保护 GUI 和工作线程之间的单帧邮箱。
    VideoFrame m_latest;                   // 只保留最新快照，慢消费者不会堆积旧帧。
};
} // namespace csn
Q_DECLARE_METATYPE(csn::VideoFrame)
