#pragma once
#include "FaceDetection.h"
#include <functional>

namespace csn {
// 普通本地美颜处理类，无线程；GpuFaceDetector在线程内持有它，图像不读回CPU。
class GpuBeauty final {
public:
    explicit GpuBeauty(ID3D11Device *device); // 创建私有上下文/Shader，共用检测帧的设备。
    VideoFrame apply(const FaceFrame &frame, const BeautySettings &settings,
                     const std::function<bool()> &cancelled); // 人脸区域磨皮/提亮，返回独立BGRA或原帧。
private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;             // 源与输出在同一个适配器。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_immediate;   // 提交命令并检查完成，恢复共享状态。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deferred;    // 独占录制状态，避免影响采集/显示/编码。
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertex;       // 全屏三角形，不需要顶点缓冲。
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixel;         // 区域蒙版、保边平滑、柔和提亮。
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;     // 线性采样，边界钳制。
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_parameters;        // 原图尺寸、强度、最多16个人脸框及五点。
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;   // 全图二维绘制状态。
    Microsoft::WRL::ComPtr<ID3D11Query> m_done;               // 交付前确认输出完成，可取消且有超时。
};
}
