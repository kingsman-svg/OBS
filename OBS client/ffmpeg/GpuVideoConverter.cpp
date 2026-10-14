#include "GpuVideoConverter.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include <QElapsedTimer>
#include <QThread>
#include <winrt/base.h>
#include <stdexcept>

namespace csn {
using Microsoft::WRL::ComPtr;
GpuVideoConverter::GpuVideoConverter(ID3D11Device *device) : m_device(device)
{
    // 1. Decoder打开阶段取得设备能力；失败由自动模式撤回硬件候选，保留软件解码。
    if (!device) throw std::invalid_argument("GPU视频转换缺少D3D11设备");
    device->GetImmediateContext(&m_immediate);
    winrt::check_hresult(m_device.As(&m_videoDevice));
    winrt::check_hresult(m_immediate.As(&m_videoContext));
    ComPtr<ID3D11Multithread> protection;
    winrt::check_hresult(m_immediate.As(&protection)); protection->SetMultithreadProtected(TRUE);
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    winrt::check_hresult(device->CreateQuery(&query, &m_done));
}
void GpuVideoConverter::configure(UINT width, UINT height, DXGI_FORMAT input)
{
    if (m_processor && width == m_width && height == m_height && input == m_input) return;
    // 2. 视频处理器负责GPU颜色转换，禁用自动增强/去隔行，保留渐进视频原有颜色。
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = content.OutputWidth = width; content.InputHeight = content.OutputHeight = height;
    content.InputFrameRate = content.OutputFrameRate = {30, 1}; content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    winrt::check_hresult(m_videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator));
    UINT support = 0;
    winrt::check_hresult(enumerator->CheckVideoProcessorFormat(input, &support));
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) throw std::runtime_error("GPU视频处理器不支持输入像素格式");
    winrt::check_hresult(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support));
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) throw std::runtime_error("GPU视频处理器不支持BGRA输出");
    ComPtr<ID3D11VideoProcessor> processor;
    winrt::check_hresult(m_videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor));
    m_enumerator = std::move(enumerator); m_processor = std::move(processor);
    m_width = width; m_height = height; m_input = input; m_frameIndex = 0;
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor.Get(), 0, FALSE);
}
ComPtr<ID3D11Texture2D> GpuVideoConverter::convert(const AVFrame &frame, const std::function<bool()> &cancelled)
{
    // 3. AVFrame.data[0]是解码数组纹理，data[1]是层索引；不把数组纹理直接交给预览窗口。
    if (frame.format != AV_PIX_FMT_D3D11 || !frame.data[0] || !cancelled || frame.width <= 0 || frame.height <= 0
        || frame.width > 3840 || frame.height > 2160) throw std::invalid_argument("GPU转换帧格式/尺寸无效");
    if (cancelled()) throw std::runtime_error("播放已停止");
    auto *source = reinterpret_cast<ID3D11Texture2D *>(frame.data[0]);
    const auto slice = reinterpret_cast<uintptr_t>(frame.data[1]);
    D3D11_TEXTURE2D_DESC input{}; source->GetDesc(&input);
    ComPtr<ID3D11Device> device; source->GetDevice(&device);
    if (device.Get() != m_device.Get() || slice >= input.ArraySize || input.SampleDesc.Count != 1
        || UINT(frame.width) > input.Width || UINT(frame.height) > input.Height
        || (input.Format != DXGI_FORMAT_NV12 && input.Format != DXGI_FORMAT_P010))
        throw std::runtime_error("GPU转换需要同设备NV12/P010解码纹理及有效数组层");
    if (frame.color_trc == AVCOL_TRC_SMPTE2084 || frame.color_trc == AVCOL_TRC_ARIB_STD_B67
        || frame.colorspace == AVCOL_SPC_BT2020_NCL || frame.colorspace == AVCOL_SPC_BT2020_CL)
        throw std::runtime_error("当前GPU转换支持SDR BT601/709，HDR/BT2020需另接色彩管理");
    configure(UINT(frame.width), UINT(frame.height), input.Format);
    const bool full = frame.color_range == AVCOL_RANGE_JPEG;
    const bool bt709 = frame.colorspace == AVCOL_SPC_BT709;
    const auto color = bt709 ? (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709)
        : (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
    m_videoContext->VideoProcessorSetStreamColorSpace1(m_processor.Get(), 0, color);
    m_videoContext->VideoProcessorSetOutputColorSpace1(m_processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    const RECT rect{0, 0, frame.width, frame.height};
    m_videoContext->VideoProcessorSetStreamSourceRect(m_processor.Get(), 0, TRUE, &rect);
    m_videoContext->VideoProcessorSetStreamDestRect(m_processor.Get(), 0, TRUE, &rect);
    m_videoContext->VideoProcessorSetOutputTargetRect(m_processor.Get(), TRUE, &rect);
    m_videoContext->VideoProcessorSetStreamAlpha(m_processor.Get(), 0, TRUE, 1.0f);
    m_videoContext->VideoProcessorSetOutputAlphaFillMode(m_processor.Get(), D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
    // 4. 每次分配独立BGRA快照，GUI持有时不复用；输入借用结束后仍不改变已交付画面。
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = m_width; desc.Height = m_height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> result; winrt::check_hresult(m_device->CreateTexture2D(&desc, nullptr, &result));
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputView{};
    inputView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; inputView.Texture2D.ArraySlice = UINT(slice);
    ComPtr<ID3D11VideoProcessorInputView> view;
    winrt::check_hresult(m_videoDevice->CreateVideoProcessorInputView(source, m_enumerator.Get(), &inputView, &view));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputView{};
    outputView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> output;
    winrt::check_hresult(m_videoDevice->CreateVideoProcessorOutputView(result.Get(), m_enumerator.Get(), &outputView, &output));
    D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = view.Get();
    winrt::check_hresult(m_videoContext->VideoProcessorBlt(m_processor.Get(), output.Get(), m_frameIndex++, 1, &stream));
    m_immediate->End(m_done.Get()); m_immediate->Flush();
    // 5. 完成查询只检查GPU状态，不下载像素；等待可取消/超时，帧引用随后交还解码池。
    QElapsedTimer timer; timer.start();
    for (;;) {
        BOOL complete = FALSE;
        const HRESULT done = m_immediate->GetData(m_done.Get(), &complete, sizeof(complete), 0);
        if (done == S_OK && complete) break;
        winrt::check_hresult(done);
        if (cancelled()) throw std::runtime_error("播放已停止");
        if (timer.elapsed() > 2000) throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"GPU视频转换等待超时");
        QThread::usleep(250);
    }
    return result;
}
}
