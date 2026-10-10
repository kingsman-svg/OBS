#include "GpuVideoOutput.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace csn {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char Shader[] = R"(
Texture2D picture : register(t0);
SamplerState sampling : register(s0);
struct Face { float4 box; float4 p01; float4 p23; float4 p4; };
cbuffer Overlay : register(b0) { float4 image; Face faces[16]; };
struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    Vertex v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1); return v;
}
float4 ps(Vertex v) : SV_TARGET {
    float3 color = picture.Sample(sampling, v.uv).rgb;
    float2 pixel = v.uv * image.xy;
    for (int i = 0; i < (int)image.z; ++i) {
        float4 box = faces[i].box;
        bool vertical = min(abs(pixel.x - box.x), abs(pixel.x - box.z)) <= image.w && pixel.y >= box.y && pixel.y <= box.w;
        bool horizontal = min(abs(pixel.y - box.y), abs(pixel.y - box.w)) <= image.w && pixel.x >= box.x && pixel.x <= box.z;
        if (vertical || horizontal) color = float3(0.1, 1, 0.3);
        float radius = image.w * 1.75;
        if (distance(pixel, faces[i].p01.xy) <= radius || distance(pixel, faces[i].p01.zw) <= radius ||
            distance(pixel, faces[i].p23.xy) <= radius || distance(pixel, faces[i].p23.zw) <= radius ||
            distance(pixel, faces[i].p4.xy) <= radius) color = float3(1, 0.2, 0.15);
    }
    return float4(color, 1);
}
)";
struct Overlay {
    float image[4]{};       // 源宽高、框数量、源图线宽。
    float faces[16][16]{};  // LTRB、五点坐标，按 HLSL float4 对齐。
};
static_assert(sizeof(Overlay) % 16 == 0);
ComPtr<ID3DBlob> compile(const char *entry, const char *profile)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(Shader, std::strlen(Shader), "GpuVideoOutput", nullptr, nullptr,
        entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result) && errors) throw std::runtime_error(static_cast<const char *>(errors->GetBufferPointer()));
    winrt::check_hresult(result); return code;
}
}

GpuVideoOutput::GpuVideoOutput(ID3D11Device *device) : m_device(device)
{
    // 1. 资源在编码工作线程建立；共享立即上下文受 D3D11 多线程保护。
    if (!device) throw std::invalid_argument("GPU输出缺少D3D11设备");
    device->GetImmediateContext(&m_immediate);
    ComPtr<ID3D11Multithread> protection;
    winrt::check_hresult(m_immediate.As(&protection)); protection->SetMultithreadProtected(TRUE);
    winrt::check_hresult(device->CreateDeferredContext(0, &m_deferred));
    const auto vertex = compile("vs", "vs_5_0"), pixel = compile("ps", "ps_5_0");
    winrt::check_hresult(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &m_vertex));
    winrt::check_hresult(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &m_pixel));
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(device->CreateSamplerState(&sampler, &m_sampler));
    D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = sizeof(Overlay); buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(device->CreateBuffer(&buffer, nullptr, &m_overlay));
    D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    winrt::check_hresult(device->CreateRasterizerState(&raster, &m_raster));
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    winrt::check_hresult(device->CreateQuery(&query, &m_done));
}

void GpuVideoOutput::render(const FaceFrame &frame, ID3D11Texture2D *target)
{
    // 2. 输入快照只读；输出是 AVFrame 的独立纹理，编码释放引用前不能再次写它。
    if (!frame.video.texture || !target) throw std::invalid_argument("GPU输出缺少输入/目标纹理");
    ComPtr<ID3D11Device> sourceDevice, targetDevice;
    frame.video.texture->GetDevice(&sourceDevice); target->GetDevice(&targetDevice);
    if (sourceDevice.Get() != m_device.Get() || targetDevice.Get() != m_device.Get())
        throw std::runtime_error("录制不支持运行中跨D3D11设备切换");
    D3D11_TEXTURE2D_DESC source{}, output{};
    frame.video.texture->GetDesc(&source); target->GetDesc(&output);
    if (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM || source.ArraySize != 1 || source.SampleDesc.Count != 1
        || !(source.BindFlags & D3D11_BIND_SHADER_RESOURCE) || output.Format != source.Format
        || output.ArraySize != 1 || output.SampleDesc.Count != 1 || !(output.BindFlags & D3D11_BIND_RENDER_TARGET))
        throw std::runtime_error("GPU输出要求单层BGRA8采样纹理和渲染目标");
    ComPtr<ID3D11ShaderResourceView> input;
    ComPtr<ID3D11RenderTargetView> destination;
    winrt::check_hresult(m_device->CreateShaderResourceView(frame.video.texture.Get(), nullptr, &input));
    winrt::check_hresult(m_device->CreateRenderTargetView(target, nullptr, &destination));
    // 3. 固定编码尺寸，源窗口变化时等比缩放并补黑；框点仍使用源图坐标。
    const float scale = std::min(float(output.Width) / source.Width, float(output.Height) / source.Height);
    const float width = source.Width * scale, height = source.Height * scale;
    const D3D11_VIEWPORT viewport{(output.Width - width) / 2, (output.Height - height) / 2, width, height, 0, 1};
    Overlay overlay{};
    overlay.image[0] = float(source.Width); overlay.image[1] = float(source.Height);
    overlay.image[2] = float(std::min<qsizetype>(16, frame.faces.size())); overlay.image[3] = 2 / scale;
    for (int i = 0; i < int(overlay.image[2]); ++i) {
        const auto &face = frame.faces[i]; auto *values = overlay.faces[i];
        values[0] = float(face.box.left()); values[1] = float(face.box.top());
        values[2] = float(face.box.right()); values[3] = float(face.box.bottom());
        for (int p = 0; p < 5; ++p) { values[4 + 2 * p] = float(face.landmarks[p].x()); values[5 + 2 * p] = float(face.landmarks[p].y()); }
    }
    constexpr float black[]{0, 0, 0, 1};
    auto *rtv = destination.Get(); auto *srv = input.Get(); auto *sampler = m_sampler.Get(); auto *constants = m_overlay.Get();
    m_deferred->ClearRenderTargetView(rtv, black);
    m_deferred->OMSetRenderTargets(1, &rtv, nullptr); m_deferred->OMSetDepthStencilState(nullptr, 0);
    m_deferred->RSSetState(m_raster.Get()); m_deferred->RSSetViewports(1, &viewport);
    m_deferred->IASetInputLayout(nullptr); m_deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deferred->VSSetShader(m_vertex.Get(), nullptr, 0); m_deferred->PSSetShader(m_pixel.Get(), nullptr, 0);
    m_deferred->PSSetShaderResources(0, 1, &srv); m_deferred->PSSetSamplers(0, 1, &sampler);
    m_deferred->UpdateSubresource(m_overlay.Get(), 0, nullptr, &overlay, 0, 0);
    m_deferred->PSSetConstantBuffers(0, 1, &constants); m_deferred->Draw(3, 0);
    m_deferred->End(m_done.Get());
    ComPtr<ID3D11CommandList> commands;
    winrt::check_hresult(m_deferred->FinishCommandList(FALSE, &commands));
    m_immediate->ExecuteCommandList(commands.Get(), TRUE); m_immediate->Flush();
    // 4. 工作线程等待绘制完成再交给 NVENC；查询只检查 GPU 状态，不读回视频像素。
    QElapsedTimer elapsed; elapsed.start();
    for (;;) {
        const HRESULT result = m_immediate->GetData(m_done.Get(), nullptr, 0, 0);
        if (result == S_OK) break;
        winrt::check_hresult(result);
        if (elapsed.elapsed() > 2000) throw std::runtime_error("编码GPU输出等待超时");
        QThread::usleep(250);
    }
}
}
