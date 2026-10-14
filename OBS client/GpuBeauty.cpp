#include "GpuBeauty.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace csn {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char Shader[] = R"(
Texture2D picture : register(t0);
SamplerState sampling : register(s0);
struct Face { float4 box; float4 p01; float4 p23; float4 p4; };
cbuffer Parameters : register(b0) { float4 image; float4 strength; Face faces[16]; };
struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    Vertex v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1); return v;
}
float protect(float2 pixel, float2 center, float2 radius) {
    return smoothstep(0.65, 1.3, length((pixel - center) / max(radius, 1)));
}
float4 ps(Vertex v) : SV_TARGET {
    float4 original = picture.Sample(sampling, v.uv);
    float2 pixel = v.uv * image.xy;
    float mask = 0;
    for (int i = 0; i < (int)image.z; ++i) {
        float4 box = faces[i].box;
        float2 size = box.zw - box.xy, center = (box.xy + box.zw) * 0.5;
        float region = 1 - smoothstep(0.72, 1.0, length((pixel - center) / (size * float2(0.46, 0.48))));
        // 双眼、鼻尖及嘴部保留细节；蒙版只依据同一帧的SCRFD框和五点。
        region *= protect(pixel, faces[i].p01.xy, size * float2(0.13, 0.08));
        region *= protect(pixel, faces[i].p01.zw, size * float2(0.13, 0.08));
        region *= protect(pixel, faces[i].p23.xy, size * float2(0.07, 0.07));
        region *= protect(pixel, (faces[i].p23.zw + faces[i].p4.xy) * 0.5, size * float2(0.22, 0.10));
        mask = max(mask, region);
    }
    // 宽松的肤色门控减少眼镜/背景渗入，属于启发式蒙版，并非模型皮肤分割。
    float y = dot(original.rgb, float3(0.299, 0.587, 0.114));
    float cb = (original.b - y) * 0.564 + 0.5, cr = (original.r - y) * 0.713 + 0.5;
    mask *= smoothstep(0.43, 0.52, cr) * (1 - smoothstep(0.57, 0.65, cr));
    mask *= smoothstep(0.24, 0.30, cb) * (1 - smoothstep(0.54, 0.60, cb));
    if (mask < 0.0001) return original;
    float3 sum = 0; float weightSum = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            float3 sample = picture.Sample(sampling, v.uv + float2(dx, dy) * image.w / image.xy).rgb;
            float3 delta = sample - original.rgb;
            float weight = exp(-float(dx * dx + dy * dy) / 5.0 - dot(delta, delta) / 0.035);
            sum += sample * weight; weightSum += weight;
        }
    }
    float3 color = lerp(original.rgb, sum / max(weightSum, 0.00001), strength.x * mask);
    color += (1 - color) * strength.y * mask * 0.18;
    return float4(saturate(color), original.a);
}
)";
struct Parameters {
    float image[4]{};       // 源宽高、人脸数、采样步长。
    float strength[4]{};    // 归一化磨皮、提亮，剩余按HLSL对齐。
    float faces[16][16]{};  // 框LTRB、五点原图坐标，不将结果应用到另一张采集帧。
};
static_assert(sizeof(Parameters) % 16 == 0);
ComPtr<ID3DBlob> compile(const char *entry, const char *profile)
{
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(Shader, std::strlen(Shader), "GpuBeauty", nullptr, nullptr,
        entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors) throw std::runtime_error(static_cast<const char *>(errors->GetBufferPointer()));
    winrt::check_hresult(hr); return code;
}
float unit(float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0; }
}
GpuBeauty::GpuBeauty(ID3D11Device *device) : m_device(device)
{
    // 1. 同一检测线程建立资源；延迟上下文隔离渲染状态，共享立即上下文启用线程保护。
    if (!device) throw std::invalid_argument("美颜缺少D3D11设备");
    device->GetImmediateContext(&m_immediate);
    ComPtr<ID3D11Multithread> protection; winrt::check_hresult(m_immediate.As(&protection)); protection->SetMultithreadProtected(TRUE);
    winrt::check_hresult(device->CreateDeferredContext(0, &m_deferred));
    const auto vertex = compile("vs", "vs_5_0"), pixel = compile("ps", "ps_5_0");
    winrt::check_hresult(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &m_vertex));
    winrt::check_hresult(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &m_pixel));
    D3D11_SAMPLER_DESC sampler{}; sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(device->CreateSamplerState(&sampler, &m_sampler));
    D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = sizeof(Parameters); buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(device->CreateBuffer(&buffer, nullptr, &m_parameters));
    D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    winrt::check_hresult(device->CreateRasterizerState(&raster, &m_raster));
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0}; winrt::check_hresult(device->CreateQuery(&query, &m_done));
}
VideoFrame GpuBeauty::apply(const FaceFrame &frame, const BeautySettings &settings, const std::function<bool()> &cancelled)
{
    // 2. 无脸/关闭/零强度直接透传；模型结果仍保留，背景不做全图平滑。
    if (!frame.video.texture || !cancelled) throw std::invalid_argument("美颜缺少视频帧或取消回调");
    if (cancelled()) throw std::runtime_error("美颜已停止");
    const float smooth = unit(settings.smoothing), bright = unit(settings.brightness);
    if (!settings.enabled || frame.faces.isEmpty() || (smooth == 0 && bright == 0)) return frame.video;
    ComPtr<ID3D11Device> device; frame.video.texture->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; frame.video.texture->GetDesc(&desc);
    if (device.Get() != m_device.Get() || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.ArraySize != 1
        || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        || desc.Width > 3840 || desc.Height > 2160) throw std::runtime_error("美颜要求同设备、单层BGRA8纹理，最大3840×2160");
    Parameters parameters{}; parameters.image[0] = float(desc.Width); parameters.image[1] = float(desc.Height);
    parameters.image[3] = std::clamp(float(desc.Width) / 640, 1.0f, 3.0f);
    parameters.strength[0] = smooth; parameters.strength[1] = bright;
    // 3. 限制16个人脸，拒绝非有限坐标；避免异常张量把效果扩散到背景。
    for (const auto &face : frame.faces) {
        if (parameters.image[2] >= 16) break;
        bool valid = std::isfinite(face.confidence) && face.confidence >= 0.5f
            && std::isfinite(face.box.x()) && std::isfinite(face.box.y()) && std::isfinite(face.box.width()) && std::isfinite(face.box.height())
            && face.box.width() >= 4 && face.box.height() >= 4;
        for (const auto &point : face.landmarks) valid = valid && std::isfinite(point.x()) && std::isfinite(point.y());
        if (!valid) continue;
        const auto box = face.box.intersected(QRectF(0, 0, desc.Width, desc.Height)); if (box.width() < 4 || box.height() < 4) continue;
        auto *out = parameters.faces[int(parameters.image[2]++)];
        out[0] = float(box.left()); out[1] = float(box.top()); out[2] = float(box.right()); out[3] = float(box.bottom());
        for (int i = 0; i < 5; ++i) { out[4 + i * 2] = float(face.landmarks[i].x()); out[5 + i * 2] = float(face.landmarks[i].y()); }
    }
    if (parameters.image[2] == 0) return frame.video;
    // 4. 独立输出快照保持原序号/QPC；同一结果供预览、时间线和编码，绝不回写采集纹理。
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    VideoFrame output = frame.video; winrt::check_hresult(m_device->CreateTexture2D(&desc, nullptr, output.texture.ReleaseAndGetAddressOf()));
    ComPtr<ID3D11ShaderResourceView> source; ComPtr<ID3D11RenderTargetView> target;
    winrt::check_hresult(m_device->CreateShaderResourceView(frame.video.texture.Get(), nullptr, &source));
    winrt::check_hresult(m_device->CreateRenderTargetView(output.texture.Get(), nullptr, &target));
    const D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    auto *rtv = target.Get(); auto *srv = source.Get(); auto *sampler = m_sampler.Get(); auto *constants = m_parameters.Get();
    m_deferred->OMSetRenderTargets(1, &rtv, nullptr); m_deferred->OMSetDepthStencilState(nullptr, 0);
    m_deferred->RSSetState(m_raster.Get()); m_deferred->RSSetViewports(1, &viewport);
    m_deferred->IASetInputLayout(nullptr); m_deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deferred->VSSetShader(m_vertex.Get(), nullptr, 0); m_deferred->PSSetShader(m_pixel.Get(), nullptr, 0);
    m_deferred->PSSetShaderResources(0, 1, &srv); m_deferred->PSSetSamplers(0, 1, &sampler);
    m_deferred->UpdateSubresource(m_parameters.Get(), 0, nullptr, &parameters, 0, 0);
    m_deferred->PSSetConstantBuffers(0, 1, &constants); m_deferred->Draw(3, 0); m_deferred->End(m_done.Get());
    ComPtr<ID3D11CommandList> commands; winrt::check_hresult(m_deferred->FinishCommandList(FALSE, &commands));
    m_immediate->ExecuteCommandList(commands.Get(), TRUE); m_immediate->Flush();
    // 5. 完成查询不下载图像；停止/2秒超时不再发布结果，资源仍按COM引用归还。
    QElapsedTimer timer; timer.start();
    for (;;) {
        BOOL complete = FALSE; const auto hr = m_immediate->GetData(m_done.Get(), &complete, sizeof(complete), 0);
        if (hr == S_OK && complete) break; winrt::check_hresult(hr);
        if (cancelled()) throw std::runtime_error("美颜已停止");
        if (timer.elapsed() > 2000) throw std::runtime_error("美颜GPU等待超时");
        QThread::usleep(250);
    }
    return output;
}
}
