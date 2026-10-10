#pragma once
#include "FaceDetection.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace csn {
// 编码前 GPU 输出：等比缩放、补边和同帧框点，直接绘制到编码器拥有的 BGRA 纹理。
// 普通类，无线程；由 MediaPublisher 的工作线程创建、调用、销毁。
class GpuVideoOutput final {
public:
    explicit GpuVideoOutput(ID3D11Device *device); // 复用采集设备，创建私有延迟上下文和 Shader。
    void render(const FaceFrame &source, ID3D11Texture2D *target); // 等 GPU 绘制完成后返回，不读回像素。
private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;             // 与采集、编码同一 D3D11 设备。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_immediate;   // 只提交录制的命令，恢复共享状态。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deferred;    // 本类独占，避免修改其它线程的绘制状态。
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertex;       // 全屏三角形顶点 Shader。
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixel;         // BGRA 采样与框点叠加。
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;     // 线性缩放，边界截断。
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_overlay;           // 同一源帧的人脸框点常量。
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;   // 无裁剪/剔除的视口状态。
    Microsoft::WRL::ComPtr<ID3D11Query> m_done;               // GPU 完成标记，提交给 NVENC 前完成资源交接。
};
}
