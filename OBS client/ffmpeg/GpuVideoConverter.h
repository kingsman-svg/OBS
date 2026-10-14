#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <functional>
struct AVFrame;

namespace csn {
// 普通GPU转换类，无线程；MediaDecoder在播放线程持有它，输入只在convert期间借用。
class GpuVideoConverter final {
public:
    explicit GpuVideoConverter(ID3D11Device *device); // 取得VideoDevice/Context，创建完成查询，不创建窗口。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> convert(const AVFrame &frame, const std::function<bool()> &cancelled); // NV12/P010数组层→独立BGRA，GPU完成后返回。
private:
    void configure(UINT width, UINT height, DXGI_FORMAT input); // 尺寸/格式变化时重建处理器和内容描述。
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;              // 和FFmpeg硬件解码器共用同一设备。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_immediate;   // GPU命令与完成查询，只在convert的播放线程使用。
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;   // 创建转换处理器及输入/输出视频视图。
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> m_videoContext; // 颜色空间、裁剪、VideoProcessorBlt。
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator; // 本轮尺寸和格式支持信息。
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;  // 独占逐帧转换状态，不与GUI渲染器共用。
    Microsoft::WRL::ComPtr<ID3D11Query> m_done;                // 查询GPU完成，保证解码池不会提前复用输入。
    UINT m_width = 0, m_height = 0;                           // 可见尺寸；解码池纹理可能有额外对齐行。
    DXGI_FORMAT m_input = DXGI_FORMAT_UNKNOWN;                // 当前NV12或P010格式，变化时重新配置。
    UINT m_frameIndex = 0;                                   // 本处理器的输出帧编号，重配置从零开始。
};
}
