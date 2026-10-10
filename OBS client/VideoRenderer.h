#pragma once
#include "VideoCapture.h"
#include "FaceDetection.h"
#include <QWidget>
#include <dxgi1_2.h>

// 原生 D3D11 画布：只消费 GPU 纹理，不采集、不读回 CPU、不处理业务状态。
class VideoRenderer final : public QWidget {
    Q_OBJECT
public:
    explicit VideoRenderer(QWidget *parent = nullptr); // 配置原生子窗口，首帧才创建 GPU 资源。
    ~VideoRenderer() override;                        // 在 HWND 销毁前释放交换链。
    void setFrame(const csn::VideoFrame &frame, const QVector<csn::FaceDetection> &faces = {}); // 原帧和同帧检测一起更新。
    void clear();                                    // 清除画面、失败状态和本轮 GPU 引用。
    QPaintEngine *paintEngine() const override;       // 禁用该画布的 Qt backing store 绘制。
signals:
    void failed(const QString &message);              // 失败锁存到 clear，避免逐帧报错。
    void framePresented(quint64 sequence);            // Present 接受此帧，非显示器扫描完成通知。
protected:
    void paintEvent(QPaintEvent *event) override;     // 记录绘制命令并提交给同一个采集设备。
    void resizeEvent(QResizeEvent *event) override;   // 请求重绘，物理像素尺寸从 HWND 获取。
    void showEvent(QShowEvent *event) override;       // 恢复/重开时绘制缓存的最新帧。
private:
    void initialize(ID3D11Device *device, HWND window); // 创建独立录制上下文、Shader 和交换链。
    void resizeBuffers(const QSize &size);             // 先释放 RTV，再调整后缓冲。
    void render();                                    // 保持比例绘制，忙碌时丢弃一次 Present。
    void releaseResources();                          // 释放渲染资源，不清空共享上下文状态。
    csn::VideoFrame m_frame;                           // 最新纹理，COM 引用延长其寿命。
    QVector<csn::FaceDetection> m_faces;              // 同一帧的人脸框与五点，停止时清空。
    bool m_failed = false;                            // 本轮错误锁存标志。
    bool m_retryScheduled = false;                    // 忙碌 Present 最多保留一个延迟重试。
    HWND m_window = nullptr;                          // 当前交换链绑定的原生句柄。
    QSize m_bufferSize;                               // 后缓冲物理像素尺寸，非 Qt 逻辑尺寸。
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;      // 输入纹理所属的设备，不跨设备复制。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_immediate; // 共享提交入口，开启内部线程保护。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deferred; // 专用录制上下文，隔离绘制状态。
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain; // 双缓冲 flip 交换链。
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_target; // 后缓冲输出视图。
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_source; // 当前视频纹理采样视图。
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertex; // 无顶点缓冲的全屏三角形。
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixel;   // 纹理采样后输出不透明 RGB。
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_overlay;     // 原图尺寸、最多16个框/五点的常量缓冲。
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler; // 线性缩放及边缘钳制。
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer; // 禁用面剔除的二维光栅状态。
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depth; // 禁用深度与模板测试。
};
