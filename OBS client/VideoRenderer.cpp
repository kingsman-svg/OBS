#include "VideoRenderer.h"
#include <QThread>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
// 采样坐标以左上角为原点；超出视口的三角形由光栅器裁剪。
constexpr char shader[] = R"(
struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex vsMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    Vertex v; v.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1); v.uv = uv;
    return v;
}
Texture2D video : register(t0);
SamplerState linearClamp : register(s0);
float4 psMain(Vertex v) : SV_TARGET { return float4(video.Sample(linearClamp, v.uv).rgb, 1); }
)";

// Shader 编译失败时保留 HLSL 诊断，交给统一错误出口。
ComPtr<ID3DBlob> compile(const char *entry, const char *profile)
{
    ComPtr<ID3DBlob> code, diagnostics;
    const HRESULT result = D3DCompile(shader, std::strlen(shader), "VideoRenderer", nullptr, nullptr,
        entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &diagnostics);
    if (FAILED(result) && diagnostics)
        throw std::runtime_error(static_cast<const char *>(diagnostics->GetBufferPointer()));
    winrt::check_hresult(result);
    return code;
}
}

VideoRenderer::VideoRenderer(QWidget *parent) : QWidget(parent)
{
    // 1. 让 D3D11 接管原生子窗口像素，Qt 只管理布局和事件。
    setObjectName(QStringLiteral("videoRenderer"));
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

VideoRenderer::~VideoRenderer() { releaseResources(); }
QPaintEngine *VideoRenderer::paintEngine() const { return nullptr; }

void VideoRenderer::setFrame(const csn::VideoFrame &frame)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!frame.texture) { clear(); return; }
    m_frame = frame;
    if (!m_failed) update(); // 隐藏时仍换缓存，Qt 不会为隐藏画布执行 Present。
}

void VideoRenderer::clear()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_frame = {}; m_failed = false;
    releaseResources();
}

void VideoRenderer::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); update(); }
void VideoRenderer::showEvent(QShowEvent *event) { QWidget::showEvent(event); update(); }

void VideoRenderer::paintEvent(QPaintEvent *)
{
    if (!m_frame.texture || m_failed || !isVisible()) return;
    QString message;
    try { render(); }
    catch (const winrt::hresult_error &error) {
        message = tr("GPU 预览失败（0x%1）：%2").arg(quint32(error.code().value), 8, 16, QLatin1Char('0'))
            .arg(QString::fromStdWString(error.message().c_str()));
    } catch (const std::exception &error) { message = tr("GPU 预览失败：%1").arg(QString::fromUtf8(error.what())); }
    if (!message.isEmpty()) {
        m_failed = true; releaseResources();
        emit failed(message + tr("；可停止采集后重试。"));
    }
}

void VideoRenderer::initialize(ID3D11Device *device, HWND window)
{
    // 1. 换设备/原生句柄时先释放旧链，复用输入纹理所属设备。
    releaseResources();
    m_device = device; m_window = window;
    m_device->GetImmediateContext(&m_immediate);
    ComPtr<ID3D11Multithread> protection;
    winrt::check_hresult(m_immediate.As(&protection));
    protection->SetMultithreadProtected(TRUE);
    // 2. 独立录制渲染状态，避免修改 WinRT/采集使用的立即上下文状态。
    winrt::check_hresult(m_device->CreateDeferredContext(0, &m_deferred));
    const auto vs = compile("vsMain", "vs_4_0");
    const auto ps = compile("psMain", "ps_4_0");
    winrt::check_hresult(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vertex));
    winrt::check_hresult(m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_pixel));
    // 3. 固定二维绘制状态：线性采样、边缘钳制、不剔除、不做深度测试。
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(m_device->CreateSamplerState(&sampler, &m_sampler));
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID; rasterizer.CullMode = D3D11_CULL_NONE; rasterizer.DepthClipEnable = TRUE;
    winrt::check_hresult(m_device->CreateRasterizerState(&rasterizer, &m_rasterizer));
    D3D11_DEPTH_STENCIL_DESC depth{};
    winrt::check_hresult(m_device->CreateDepthStencilState(&depth, &m_depth));
    // 4. 沿设备找到 DXGI 工厂，为此 HWND 创建双缓冲 flip 交换链。
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    winrt::check_hresult(m_device.As(&dxgi));
    winrt::check_hresult(dxgi->GetAdapter(&adapter));
    winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; description.Scaling = DXGI_SCALING_STRETCH;
    winrt::check_hresult(factory->CreateSwapChainForHwnd(m_device.Get(), window, &description, nullptr, nullptr, &m_swapChain));
    winrt::check_hresult(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
}

void VideoRenderer::resizeBuffers(const QSize &size)
{
    if (size == m_bufferSize && m_target) return;
    // 1. 撤销旧后缓冲引用；FinishCommandList(FALSE) 已重置录制状态。
    m_target.Reset();
    // 2. 根据实际物理像素重建后缓冲，适配缩放和高 DPI。
    winrt::check_hresult(m_swapChain->ResizeBuffers(0, UINT(size.width()), UINT(size.height()), DXGI_FORMAT_UNKNOWN, 0));
    ComPtr<ID3D11Texture2D> buffer;
    winrt::check_hresult(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)));
    // 3. 为新后缓冲建立 RTV，后面的 Draw 输出到这里。
    winrt::check_hresult(m_device->CreateRenderTargetView(buffer.Get(), nullptr, &m_target));
    m_bufferSize = size;
}

void VideoRenderer::render()
{
    // 1. 取得原生窗口物理尺寸；最小化时不提交无用帧。
    const auto window = reinterpret_cast<HWND>(winId());
    RECT rectangle{};
    winrt::check_bool(GetClientRect(window, &rectangle));
    const QSize size(rectangle.right, rectangle.bottom);
    if (size.isEmpty() || IsIconic(GetAncestor(window, GA_ROOT))) return;
    ComPtr<ID3D11Device> device;
    m_frame.texture->GetDevice(&device);
    if (device.Get() != m_device.Get() || window != m_window) initialize(device.Get(), window);
    resizeBuffers(size);
    // 2. 输入纹理建立 SRV；只接收采集模块约定的单层 BGRA8 纹理。
    D3D11_TEXTURE2D_DESC description{};
    m_frame.texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count != 1
        || description.ArraySize != 1 || !(description.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        throw std::runtime_error("预览需要单层、非多重采样的 BGRA8 ShaderResource 纹理");
    m_source.Reset();
    winrt::check_hresult(m_device->CreateShaderResourceView(m_frame.texture.Get(), nullptr, &m_source));
    // 3. 计算等比居中的视口，先清空整个后缓冲，剩余区域保留深色背景。
    const float scale = std::min(float(size.width()) / description.Width, float(size.height()) / description.Height);
    const float width = description.Width * scale, height = description.Height * scale;
    const D3D11_VIEWPORT viewport{(size.width() - width) / 2, (size.height() - height) / 2, width, height, 0, 1};
    constexpr float background[]{16.0f / 255, 24.0f / 255, 36.0f / 255, 1};
    auto *target = m_target.Get(); auto *source = m_source.Get(); auto *sampler = m_sampler.Get();
    m_deferred->ClearRenderTargetView(target, background);
    m_deferred->OMSetRenderTargets(1, &target, nullptr);
    m_deferred->OMSetDepthStencilState(m_depth.Get(), 0);
    m_deferred->RSSetState(m_rasterizer.Get()); m_deferred->RSSetViewports(1, &viewport);
    m_deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_deferred->VSSetShader(m_vertex.Get(), nullptr, 0); m_deferred->PSSetShader(m_pixel.Get(), nullptr, 0);
    m_deferred->PSSetShaderResources(0, 1, &source); m_deferred->PSSetSamplers(0, 1, &sampler);
    // 4. 录制 Draw，再用 TRUE 恢复共享上下文状态；线程保护串行化整次提交。
    m_deferred->Draw(3, 0);
    ComPtr<ID3D11CommandList> commands;
    winrt::check_hresult(m_deferred->FinishCommandList(FALSE, &commands));
    m_immediate->ExecuteCommandList(commands.Get(), TRUE);
    // 5. 非阻塞 Present；队列繁忙/遮挡时跳过，下一帧再尝试。
    const HRESULT result = m_swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        // 静止源可能不再产生新帧；延迟重试一次，避免首帧忙碌后永久黑屏。
        if (!m_retryScheduled) {
            m_retryScheduled = true;
            QTimer::singleShot(16, this, [this] { m_retryScheduled = false; update(); });
        }
        return;
    }
    if (result == DXGI_STATUS_OCCLUDED) return;
    winrt::check_hresult(result);
    emit framePresented(m_frame.sequence);
}

void VideoRenderer::releaseResources()
{
    // 1. 撤销独立上下文和视图引用；不能 ClearState 共享立即上下文。
    if (m_deferred) m_deferred->ClearState();
    m_source.Reset(); m_target.Reset(); m_swapChain.Reset();
    m_vertex.Reset(); m_pixel.Reset(); m_sampler.Reset(); m_rasterizer.Reset(); m_depth.Reset();
    m_deferred.Reset();
    // 2. Flush 促使旧 flip 链销毁，同一个 HWND 才能绑定下一条交换链。
    if (m_immediate) m_immediate->Flush();
    m_immediate.Reset(); m_device.Reset();
    m_window = nullptr; m_bufferSize = {};
}
