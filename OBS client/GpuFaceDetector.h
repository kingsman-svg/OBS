#pragma once
#include "FaceDetection.h"
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <memory>

namespace csn {
// 一个工作类负责 D3D11/CUDA、TensorRT与同帧美颜；不增加采集Model/Controller或额外线程。
class GpuFaceDetector final : public QThread {
    Q_OBJECT
public:
    explicit GpuFaceDetector(QObject *parent = nullptr); // 构造只建立邮箱，不访问 GPU。
    ~GpuFaceDetector() override;                       // 停止并等待线程清理资源。
    bool begin(const QString &enginePath, BeautySettings beauty = {}, bool drawFaces = true); // 清邮箱/start，检测和美颜在同一线程。
    void setBeautyStrength(float smoothing, float brightness); // GUI快速更新强度，下一帧取得同一份参数快照。
    void submit(const VideoFrame &frame);              // 快速替换单帧输入，慢处理不积压。
    FaceFrame latestResult() const;                    // 返回结果与同帧原图/美颜纹理，GUI定时消费。
    void stop();                                      // 停止接收、清空邮箱、唤醒退出；不阻塞 GUI。
signals:
    void opened();                                    // 首个设备的 CUDA/引擎初始化完成。
    void failed(const QString &message);               // 本轮失败一次，控制器恢复原画面。
protected:
    void run() override;                              // 读引擎 → 等帧 → 互操作/推理 → 发布 → 清理。
private:
    struct Resources;                                 // 仅工作线程拥有的 GPU RAII 资源，定义藏在 cpp。
    mutable QMutex m_mutex;                            // 只保护有限邮箱和接受标志。
    QWaitCondition m_available;                        // 没有新帧时休眠，stop 能立即唤醒。
    QString m_enginePath;                              // begin 设置，运行期间只读。
    VideoFrame m_pending;                              // 最多一帧待处理；COM 引用保证纹理有效。
    FaceFrame m_latest;                                // 最多一个处理完成结果。
    bool m_accepting = false;                          // 防止 stop 后迟到 submit/结果重新填充邮箱。
    quint64 m_replaced = 0;                            // 被最新输入替换的待处理帧计数。
    BeautySettings m_beauty;                           // 本轮开关固定，强度按锁在帧间更新。
    bool m_drawFaces = true;                           // 诊断框点显示开关，与美颜分开。
};
}
