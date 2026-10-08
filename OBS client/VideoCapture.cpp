#include "VideoCapture.h"
#include <QMutexLocker>
#include <QSemaphore>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <memory>
#include <d3d11_4.h>
#include <MemoryBuffer.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.MediaProperties.h>

using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Capture::Frames;
using namespace winrt::Windows::Graphics::Imaging;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace DX = winrt::Windows::Graphics::DirectX;

namespace {
qint64 qpcTime()
{
    LARGE_INTEGER value{}, frequency{};
    QueryPerformanceCounter(&value);
    QueryPerformanceFrequency(&frequency);
    return value.QuadPart / frequency.QuadPart * 10000000
        + value.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
}
ComPtr<ID3D11Device> createDevice()
{
    ComPtr<ID3D11Device> device;
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    return device;
}
ComPtr<ID3D11Texture2D> surfaceTexture(const DX::Direct3D11::IDirect3DSurface &surface)
{
    ComPtr<ID3D11Texture2D> texture;
    check_hresult(surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()
        ->GetInterface(IID_PPV_ARGS(&texture)));
    return texture;
}
// .get() 只出现在 MTA 工作线程。初始化期间也定期检查停止请求。
template<class Operation> auto awaitOperation(const Operation &operation, QThread *thread)
{
    while (operation.wait_for(std::chrono::milliseconds(50)) == winrt::Windows::Foundation::AsyncStatus::Started) {
        if (thread->isInterruptionRequested()) {
            operation.Cancel();
            throw hresult_canceled();
        }
    }
    return operation.get();
}
}

namespace csn {
VideoCapture::VideoCapture(QObject *parent) : QThread(parent) {}
VideoCapture::~VideoCapture() { stop(); wait(); }

QList<CaptureSource> VideoCapture::sources(QString *warning)
{
    QList<CaptureSource> result;
    // 桌面枚举不依赖摄像头；摄像头权限失败仍可保留窗口和显示器。
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto &items = *reinterpret_cast<QList<CaptureSource> *>(parameter);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            items.append({CaptureSource::Kind::Monitor, QString::number(quintptr(monitor), 16),
                QStringLiteral("屏幕 %1 · %2×%3").arg(QString::fromWCharArray(info.szDevice))
                    .arg(info.rcMonitor.right - info.rcMonitor.left).arg(info.rcMonitor.bottom - info.rcMonitor.top),
                quintptr(monitor)});
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER)) return TRUE;
        const int length = GetWindowTextLengthW(window);
        if (length <= 0) return TRUE;
        std::wstring title(size_t(length) + 1, L'\0');
        GetWindowTextW(window, title.data(), length + 1);
        auto &items = *reinterpret_cast<QList<CaptureSource> *>(parameter);
        items.append({CaptureSource::Kind::Window, QString::number(quintptr(window), 16),
                      QStringLiteral("窗口 · ") + QString::fromWCharArray(title.c_str()), quintptr(window)});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    try {
        auto devices = winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(
            winrt::Windows::Devices::Enumeration::DeviceClass::VideoCapture).get();
        for (const auto &device : devices)
            result.append({CaptureSource::Kind::Camera, QString::fromStdWString(device.Id().c_str()),
                           QStringLiteral("摄像头 · ") + QString::fromStdWString(device.Name().c_str()), 0});
    } catch (const hresult_error &error) {
        if (warning) *warning = QString::fromStdWString(error.message().c_str());
        else throw;
    }
    return result;
}

bool VideoCapture::begin(const CaptureSource &source)
{
    if (isRunning() || (source.kind != CaptureSource::Kind::Camera
        && source.kind != CaptureSource::Kind::Window && source.kind != CaptureSource::Kind::Monitor)) return false;
    m_source = source;
    { QMutexLocker lock(&m_mutex); m_latest = {}; }
    m_lastPreviewTime = 0;
    start();
    return true;
}
void VideoCapture::stop() { requestInterruption(); }
VideoFrame VideoCapture::latestFrame() const { QMutexLocker lock(&m_mutex); return m_latest; }

void VideoCapture::run()
{
    bool initialized = false;
    try {
        init_apartment(apartment_type::multi_threaded);
        initialized = true;
        if (!isInterruptionRequested()) {
            if (m_source.kind == CaptureSource::Kind::Camera) captureCamera();
            else captureDesktop();
        }
    } catch (const hresult_error &error) {
        if (!isInterruptionRequested()) emit failed(QStringLiteral("视频采集失败（0x%1）：%2")
            .arg(quint32(error.code().value), 8, 16, QLatin1Char('0'))
            .arg(QString::fromStdWString(error.message().c_str())));
    } catch (const std::exception &error) {
        if (!isInterruptionRequested()) emit failed(QString::fromUtf8(error.what()));
    }
    m_staging.Reset();
    { QMutexLocker lock(&m_mutex); m_latest = {}; }
    if (initialized) uninit_apartment();
}

void VideoCapture::captureDesktop()
{
    // 在激活 WinRT/WGC 或创建设备前检查失效句柄，避免无效请求进入系统采集服务。
    if (m_source.kind == CaptureSource::Kind::Window) {
        const auto window = reinterpret_cast<HWND>(m_source.handle);
        if (!IsWindow(window) || IsIconic(window)) throw std::runtime_error("目标窗口已关闭或最小化，请重新选择");
    }
    if (!WGC::GraphicsCaptureSession::IsSupported()) throw std::runtime_error("当前系统不支持 WGC（需要 Windows 10 1903 或更高版本）");
    auto device = createDevice();
    ComPtr<IDXGIDevice> dxgi;
    check_hresult(device.As(&dxgi));
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
    auto direct3d = inspectable.as<DX::Direct3D11::IDirect3DDevice>();
    auto factory = get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{nullptr};
    if (m_source.kind == CaptureSource::Kind::Monitor) {
        check_hresult(factory->CreateForMonitor(reinterpret_cast<HMONITOR>(m_source.handle),
            guid_of<WGC::GraphicsCaptureItem>(), put_abi(item)));
    } else {
        const auto window = reinterpret_cast<HWND>(m_source.handle);
        check_hresult(factory->CreateForWindow(window, guid_of<WGC::GraphicsCaptureItem>(), put_abi(item)));
    }
    auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) throw std::runtime_error("采集目标尺寸无效");
    auto pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(direct3d,
        DX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    // 系统帧回调只通知工作线程，不持有 this 或执行纹理操作；退出时不存在悬空对象回调。
    auto notification = std::make_shared<QSemaphore>();
    const auto frameToken = pool.FrameArrived([notification](const auto &, const auto &) {
        if (!notification->available()) notification->release();
    });
    auto session = pool.CreateCaptureSession(item);
    auto closed = std::make_shared<std::atomic_bool>(false);
    const auto closedToken = item.Closed([closed](const auto &, const auto &) { closed->store(true); });
    // 每项独立清理，设备断开导致一个 Close 抛错时，仍然撤销其余回调并释放资源。
    const auto cleanup = [&]() noexcept {
        try { item.Closed(closedToken); } catch (...) {}
        try { pool.FrameArrived(frameToken); } catch (...) {}
        try { session.Close(); } catch (...) {}
        try { pool.Close(); } catch (...) {}
    };
    try {
        session.StartCapture();
        emit opened();
        while (!isInterruptionRequested()) {
            if (closed->load()) throw std::runtime_error("采集目标已关闭或断开");
            if (m_source.kind == CaptureSource::Kind::Window
                && !IsWindow(reinterpret_cast<HWND>(m_source.handle))) throw std::runtime_error("采集窗口已关闭");
            notification->tryAcquire(1, 33);
            auto frame = pool.TryGetNextFrame();
            if (!frame) continue;
            const auto content = frame.ContentSize();
            const bool resized = content.Width != size.Width || content.Height != size.Height;
            if (content.Width > 0 && content.Height > 0) {
                auto texture = surfaceTexture(frame.Surface());
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                // 尺寸变化的第一帧可能仍来自旧池，只复制实际有效的交集。
                publishTexture(texture.Get(), std::min(content.Width, int(description.Width)),
                    std::min(content.Height, int(description.Height)), frame.SystemRelativeTime().count());
            }
            frame.Close(); // 必须先归还旧帧，再按新尺寸重建池。
            if (resized && content.Width > 0 && content.Height > 0) {
                size = content;
                pool.Recreate(direct3d, DX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            }
            msleep(25); // 本阶段预览采集目标约 30fps，不在帧回调中占用 GUI。
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
}

void VideoCapture::captureCamera()
{
    MediaCapture capture;
    MediaFrameReader reader{nullptr};
    event_token frameToken{};
    auto failure = std::make_shared<std::atomic_uint32_t>(0);
    const auto failedToken = capture.Failed([failure](const MediaCapture &, const MediaCaptureFailedEventArgs &args) {
        failure->store(args.Code() ? args.Code() : quint32(E_FAIL));
    });
    const auto cleanup = [&]() noexcept {
        if (reader) {
            try { if (frameToken.value) reader.FrameArrived(frameToken); } catch (...) {}
            // 等待仅发生在工作线程；驱动不响应停止时，超时后仍关闭 Reader。
            try { reader.StopAsync().wait_for(std::chrono::seconds(1)); } catch (...) {}
            try { reader.Close(); } catch (...) {}
        }
        try { capture.Failed(failedToken); } catch (...) {}
        try { capture.Close(); } catch (...) {}
    };
    try {
        MediaCaptureInitializationSettings settings;
        settings.VideoDeviceId(m_source.id.toStdWString());
        settings.StreamingCaptureMode(StreamingCaptureMode::Video);
        settings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);
        settings.MemoryPreference(MediaCaptureMemoryPreference::Auto);
        awaitOperation(capture.InitializeAsync(settings), this);
        MediaFrameSource source{nullptr};
        for (const auto &entry : capture.FrameSources())
            if (entry.Value().Info().SourceKind() == MediaFrameSourceKind::Color) { source = entry.Value(); break; }
        if (!source) throw std::runtime_error("摄像头没有可用的彩色视频源");
        reader = awaitOperation(capture.CreateFrameReaderAsync(source,
            winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Bgra8()), this);
        reader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Realtime);
        auto notification = std::make_shared<QSemaphore>();
        frameToken = reader.FrameArrived([notification](const auto &, const auto &) {
            if (!notification->available()) notification->release();
        });
        if (awaitOperation(reader.StartAsync(), this) != MediaFrameReaderStartStatus::Success)
            throw std::runtime_error("摄像头启动失败，请检查隐私权限或设备占用");
        emit opened();
        ComPtr<ID3D11Device> uploadDevice; // 驱动交付 SoftwareBitmap 时才创建上传设备。
        while (!isInterruptionRequested()) {
            if (const auto code = failure->load()) check_hresult(HRESULT(code));
            notification->tryAcquire(1, 33);
            auto frame = reader.TryAcquireLatestFrame();
            if (!frame) continue;
            auto video = frame.VideoMediaFrame();
            const auto time = frame.SystemRelativeTime();
            const qint64 timestamp = time ? time.Value().count() : qpcTime();
            auto surface = video.Direct3DSurface();
            if (surface) {
                auto texture = surfaceTexture(surface);
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                publishTexture(texture.Get(), int(description.Width), int(description.Height), timestamp);
            } else if (auto bitmap = video.SoftwareBitmap()) {
                if (bitmap.BitmapPixelFormat() != BitmapPixelFormat::Bgra8)
                    bitmap = SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Bgra8);
                auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
                auto reference = buffer.CreateReference();
                uint8_t *bytes = nullptr;
                uint32_t capacity = 0;
                check_hresult(reference.as<::Windows::Foundation::IMemoryBufferByteAccess>()->GetBuffer(&bytes, &capacity));
                const auto plane = buffer.GetPlaneDescription(0);
                if (plane.Stride < bitmap.PixelWidth() * 4 || plane.StartIndex < 0 || bitmap.PixelWidth() <= 0 || bitmap.PixelHeight() <= 0
                    || quint64(plane.StartIndex) + quint64(plane.Stride) * (bitmap.PixelHeight() - 1)
                        + quint64(bitmap.PixelWidth()) * 4 > capacity)
                    throw std::runtime_error("摄像头位图缓冲布局无效");
                if (!uploadDevice) uploadDevice = createDevice();
                D3D11_TEXTURE2D_DESC description{};
                description.Width = UINT(bitmap.PixelWidth()); description.Height = UINT(bitmap.PixelHeight());
                description.MipLevels = 1; description.ArraySize = 1;
                description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data{bytes + plane.StartIndex, UINT(plane.Stride), 0};
                ComPtr<ID3D11Texture2D> texture;
                check_hresult(uploadDevice->CreateTexture2D(&description, &data, &texture));
                publishTexture(texture.Get(), bitmap.PixelWidth(), bitmap.PixelHeight(), timestamp);
                bitmap.Close();
            }
            frame.Close();
            msleep(25);
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
}

void VideoCapture::publishTexture(ID3D11Texture2D *source, int width, int height, qint64 timestamp)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_latest.sequence && timestamp <= m_latest.timestamp100ns) return; // 同一摄像头帧不重复交付。
    }
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    // WinRT 可能与我们共享设备；开启 D3D11 内部串行保护，未来消费者也须遵守设备线程规则。
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        throw std::runtime_error("采集源未返回 BGRA8 视频帧");
    description.Width = UINT(width); description.Height = UINT(height);
    description.MipLevels = 1; description.ArraySize = 1;
    description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = 0; description.MiscFlags = 0;
    VideoFrame output;
    check_hresult(device->CreateTexture2D(&description, nullptr, &output.texture));
    D3D11_BOX box{0, 0, 0, UINT(width), UINT(height), 1};
    context->CopySubresourceRegion(output.texture.Get(), 0, 0, 0, 0, source, 0, &box);
    // 此处没有 SwapChain::Present；必须提交复制命令，否则驱动可能一直占住采集池的两张纹理。
    // Flush 只提交 GPU 工作，不在 GUI 线程等待；后面的帧池才有机会继续交付新帧。
    context->Flush();
    output.timestamp100ns = timestamp;
    // GPU 帧始终保留；CPU 读回仅为现阶段 QLabel 预览，限制为 10fps。
    if (timestamp - m_lastPreviewTime >= 1000000) {
        D3D11_TEXTURE2D_DESC old{};
        ComPtr<ID3D11Device> oldDevice;
        if (m_staging) { m_staging->GetDesc(&old); m_staging->GetDevice(&oldDevice); }
        if (!m_staging || old.Width != description.Width || old.Height != description.Height || oldDevice != device) {
            description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            m_staging.Reset();
            check_hresult(device->CreateTexture2D(&description, nullptr, &m_staging));
        }
        context->CopyResource(m_staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        QImage image(static_cast<const uchar *>(mapped.pData), width, height, int(mapped.RowPitch), QImage::Format_RGB32);
        output.preview = image.scaled(960, 540, Qt::KeepAspectRatio, Qt::FastTransformation).copy();
        output.previewTimestamp100ns = timestamp;
        context->Unmap(m_staging.Get(), 0);
        m_lastPreviewTime = timestamp;
    }
    QMutexLocker lock(&m_mutex);
    output.sequence = m_latest.sequence + 1;
    if (output.preview.isNull()) {
        output.preview = m_latest.preview;
        output.previewTimestamp100ns = m_latest.previewTimestamp100ns;
    }
    m_latest = std::move(output);
}
} // namespace csn
