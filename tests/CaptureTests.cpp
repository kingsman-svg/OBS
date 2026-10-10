#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "VideoCapture.h"
#include "WasapiCapture.h"
#include "LoginModel.h"
#include "LoginController.h"
#include "HttpClient.h"
#include "mainwindow.h"
#include "PreviewWindow.h"
#include "VideoRenderer.h"
#include <QImage>
#include <d3d11_4.h>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QTest>
#include <QSignalSpy>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <ksmedia.h>
#include <winrt/base.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>

using namespace csn;
#define CHECK(condition) do { if (!(condition)) { qCritical("Check failed at line %d: %s", __LINE__, #condition); return false; } } while (false)
namespace {
bool until(const std::function<bool()> &predicate, int timeout = 10000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) QTest::qWait(5);
    return predicate();
}
float sample(const AudioPacket &packet, int index)
{
    float value;
    std::memcpy(&value, packet.pcm.constData() + index * sizeof(float), sizeof(float));
    return value;
}
WAVEFORMATEX format(WORD bits, WORD tag = WAVE_FORMAT_PCM)
{
    WAVEFORMATEX result{};
    result.wFormatTag = tag; result.wBitsPerSample = bits; result.nChannels = 2;
    result.nSamplesPerSec = 48000; result.nBlockAlign = result.nChannels * (bits / 8);
    result.nAvgBytesPerSec = result.nSamplesPerSec * result.nBlockAlign;
    return result;
}
// 测试专用 CPU → GPU 色块；生产预览没有此构造路径。
VideoFrame colorFrame(const QImage &image, quint64 sequence = 1, ID3D11Device *reuse = nullptr)
{
    Microsoft::WRL::ComPtr<ID3D11Device> device = reuse;
    if (!device) winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    D3D11_TEXTURE2D_DESC description{};
    description.Width = UINT(image.width()); description.Height = UINT(image.height());
    description.MipLevels = 1; description.ArraySize = 1; description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{image.constBits(), UINT(image.bytesPerLine()), 0};
    VideoFrame frame; frame.sequence = sequence; frame.timestamp100ns = qint64(sequence) * 333333;
    winrt::check_hresult(device->CreateTexture2D(&description, &data, &frame.texture));
    return frame;
}
// 仅测试/截图读回快照，不能放进采集或渲染生产循环。
QImage readBack(ID3D11Texture2D *texture)
{
    if (!texture) return {};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    texture->GetDevice(&device); device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ; description.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    winrt::check_hresult(device->CreateTexture2D(&description, nullptr, &staging));
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto image = QImage(static_cast<const uchar *>(mapped.pData), int(description.Width),
        int(description.Height), int(mapped.RowPitch), QImage::Format_RGB32).copy();
    context->Unmap(staging.Get(), 0);
    return image;
}
bool nearColor(const QColor &actual, const QColor &expected)
{
    return std::abs(actual.red() - expected.red()) < 8 && std::abs(actual.green() - expected.green()) < 8
        && std::abs(actual.blue() - expected.blue()) < 8;
}
bool audioFormats()
{
    qInfo("BEGIN PCM decode");
    const qint16 pcm16[]{-32768, 32767, 0, 16384};
    auto packet = WasapiCapture::decode(reinterpret_cast<const uchar *>(pcm16), 2, format(16), 0, 123456);
    CHECK(packet.channels == 2 && packet.sampleRate == 48000 && packet.timestamp100ns == 123456);
    CHECK(sample(packet, 0) == -1 && sample(packet, 2) == 0 && sample(packet, 3) == 0.5f);
    const uchar pcm24[]{0, 0, 0x80, 0xff, 0xff, 0x7f};
    packet = WasapiCapture::decode(pcm24, 1, format(24), AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, 234567);
    CHECK(sample(packet, 0) == -1 && sample(packet, 1) > 0.9999f && packet.discontinuity);
    WAVEFORMATEXTENSIBLE extended{};
    extended.Format = format(32, WAVE_FORMAT_EXTENSIBLE); extended.Format.cbSize = 22;
    extended.SubFormat = KSDATAFORMAT_SUBTYPE_PCM; extended.Samples.wValidBitsPerSample = 24;
    const qint32 pcm32[]{std::numeric_limits<qint32>::min(), 0x40000000};
    packet = WasapiCapture::decode(reinterpret_cast<const uchar *>(pcm32), 1, extended.Format, 0, 1);
    CHECK(sample(packet, 0) == -1 && sample(packet, 1) == 0.5f); // EXTENSIBLE 不一定是 Float32。
    extended.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const float floats[]{std::numeric_limits<float>::quiet_NaN(), 0.25f};
    packet = WasapiCapture::decode(reinterpret_cast<const uchar *>(floats), 1, extended.Format, 0, 1);
    CHECK(sample(packet, 0) == 0 && sample(packet, 1) == 0.25f);
    packet = WasapiCapture::decode(nullptr, 10, format(16), AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR, 0);
    CHECK(packet.pcm.size() == 80 && sample(packet, 0) == 0 && packet.timestampEstimated && packet.timestamp100ns > 0);
    bool rejected = false;
    extended.SubFormat = GUID{};
    try { WasapiCapture::decode(nullptr, 1, extended.Format, AUDCLNT_BUFFERFLAGS_SILENT, 0); }
    catch (const std::exception &) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    auto invalid = format(16); invalid.nBlockAlign = 1;
    try { WasapiCapture::decode(nullptr, 1, invalid, AUDCLNT_BUFFERFLAGS_SILENT, 0); }
    catch (const std::exception &) { rejected = true; }
    CHECK(rejected);
    return true;
}
bool failedWorkerRestart()
{
    qInfo("BEGIN failed worker restart");
    VideoCapture worker;
    int errors = 0;
    QObject::connect(&worker, &VideoCapture::failed, &worker, [&errors] { ++errors; });
    const CaptureSource invalid{CaptureSource::Kind::Window, "invalid", "closed window", 1};
    CHECK(worker.begin(invalid));
    CHECK(!worker.begin(invalid));
    CHECK(until([&] { return errors == 1 && !worker.isRunning(); }));
    CHECK(!worker.latestFrame().texture);
    worker.stop(); worker.stop();
    CHECK(worker.begin(invalid));
    CHECK(until([&] { return errors == 2 && !worker.isRunning(); }));
    CHECK(!worker.begin({CaptureSource::Kind::Microphone, {}, {}, 0}));
    return true;
}
bool captureMvc()
{
    qInfo("BEGIN capture MVC");
    MainWindow view(ClientRole::Publisher);
    MainWindow player(ClientRole::Player);
    CHECK(!player.findChild<QComboBox *>("videoSourceCombo"));
    LoginModel login;
    HttpClient http;
    LoginController auth(&view, &login, &http);
    SessionModel model;
    SignalClient signal;
    SessionController controller(&view, &login, &model, &signal);
    auto *start = view.findChild<QPushButton *>("startCaptureButton");
    CHECK(start && !start->isEnabled());
    // 枚举在工作线程，GUI 心跳必须能继续处理。
    int ticks = 0;
    QTimer pulse; pulse.setInterval(1);
    QObject::connect(&pulse, &QTimer::timeout, &view, [&ticks] { ++ticks; }); pulse.start();
    CHECK(until([&] { return view.findChild<QPushButton *>("refreshDevicesButton")->isEnabled(); }));
    CHECK(ticks > 0);
    login.completeLogin("test", "root", "root", 60);
    CHECK(start->isEnabled());
    QSignalSpy mixChanges(&view, &MainWindow::audioMixChanged);
    view.findChild<QSpinBox *>("microphoneGain")->setValue(150);
    view.findChild<QCheckBox *>("microphoneMute")->setChecked(true);
    CHECK(mixChanges.size() == 2 && mixChanges[0][0].value<CaptureSource::Kind>() == CaptureSource::Kind::Microphone);
    CHECK(mixChanges[0][1].toFloat() == 1.5f && mixChanges[1][2].toBool());
    CHECK(!player.findChild<QSpinBox *>("microphoneGain"));
    view.captureRequested({});
    CHECK(view.findChild<QLabel *>("captureStatusLabel")->text().contains(QStringLiteral("请至少选择")));
    view.captureRequested({{CaptureSource::Kind::Window, "invalid", "closed window", 1}});
    CHECK(!start->isEnabled());
    CHECK(until([&] { return start->isEnabled(); }));
    CHECK(view.findChild<QLabel *>("captureStatusLabel")->text().contains(QStringLiteral("目标窗口")));
    view.captureRequested({{CaptureSource::Kind::Window, "invalid", "closed window", 1}});
    login.logout();
    CHECK(until([&] { return !view.findChild<QPushButton *>("stopCaptureButton")->isEnabled(); }));
    CHECK(!start->isEnabled());
    return true;
}

bool previewUiLifecycle()
{
    qInfo("BEGIN independent preview and compact layout");
    MainWindow view(ClientRole::Publisher);
    view.show();
    const QSize initialSize = view.size();
    // 布局取证只构造界面状态；包含登录页及滚动区下方的 AI 控件。
    const auto directory = qEnvironmentVariable("OBS_UI_SCREENSHOTS");
    if (!directory.isEmpty()) {
        CHECK(QDir().mkpath(directory));
        QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/登录页面布局.png")));
    }
    view.applyLoginState(QStringLiteral("已登录：root"), false, true);
    auto *preview = view.findChild<PreviewWindow *>("previewWindow");
    auto *open = view.findChild<QPushButton *>("showPreviewButton");
    auto *scroll = view.findChild<QScrollArea *>("workspaceScroll");
    CHECK(preview && preview->isWindow() && preview->windowModality() == Qt::NonModal);
    CHECK(open && open->isEnabled() && scroll);
    CHECK(!view.findChild<QLabel *>("previewPlaceholder"));

    QImage image(320, 180, QImage::Format_RGB32);
    image.fill(QColor(32, 160, 96));
    auto frame = colorFrame(image);
    view.showCapturePreview(frame);
    CHECK(preview->isVisible() && view.isEnabled());
    auto *renderer = preview->findChild<VideoRenderer *>();
    int presented = 0, failures = 0;
    QObject::connect(renderer, &VideoRenderer::framePresented, &view, [&](quint64) { ++presented; });
    QObject::connect(renderer, &VideoRenderer::failed, &view, [&](const QString &message) { ++failures; qWarning().noquote() << message; });
    renderer->update();
    CHECK(until([&] { return presented > 0 || failures > 0; }));
    CHECK(failures == 0);
    // 设置一个生产者状态，后续 GPU 绘制必须恢复它。
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    frame.texture->GetDevice(&device); device->GetImmediateContext(&context);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    preview->close();
    CHECK(!preview->isVisible());
    // 后续帧仍更新缓存，但不能把用户刚关掉的窗口重新弹出。
    image.fill(QColor(64, 128, 192));
    frame = colorFrame(image, 2, device.Get());
    view.showCapturePreview(frame);
    CHECK(!preview->isVisible());
    open->click();
    CHECK(preview->isVisible());
    preview->resize(320, 320);
    const int beforeResize = presented;
    CHECK(until([&] { return presented > beforeResize || failures > 0; }));
    CHECK(failures == 0);
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    CHECK(topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    // 同 HWND 切换输入设备，旧 flip 链必须释放才能绑定新设备。
    const int beforeDevice = presented;
    frame = colorFrame(image, 3);
    view.showCapturePreview(frame);
    CHECK(until([&] { return presented > beforeDevice || failures > 0; }));
    CHECK(failures == 0);

    const QString longName = QStringLiteral("窗口 · ") + QString(180, QChar(0x6d4b));
    view.setCaptureSources({{CaptureSource::Kind::Window, "layout", longName, 1}});
    view.findChild<QComboBox *>("videoSourceCombo")->setCurrentIndex(1);
    view.applyCaptureState(false, true, false, QStringLiteral("已发现采集设备"),
        {QStringLiteral("采集中：") + longName, QStringLiteral("未启用"), QStringLiteral("未启用")});
    const QSize compactSize = QSize(640, 480).boundedTo(initialSize);
    view.resize(compactSize);
    view.showMixedAudio(0.3f, QStringLiteral("48kHz · 立体声 · 10ms\n已输出 100 包，削波 0 次"));
    view.showMediaSyncStatus(QStringLiteral("同步运行中 · 视频30fps / 音频48kHz · 总等待120ms\n视频 30 帧（复用 10，丢弃 0）· 音频 100 包（丢弃 0）"));
    QTest::qWait(30);
    CHECK(view.size() == compactSize);
    CHECK(scroll->widget()->width() <= scroll->viewport()->width());
    CHECK(scroll->verticalScrollBar()->maximum() > 0); // 小窗口可滚动访问底部房间操作。
    auto *mixStatus = view.findChild<QLabel *>("audioMixStatusLabel");
    CHECK(mixStatus && mixStatus->hasHeightForWidth());
    CHECK(mixStatus->height() >= mixStatus->heightForWidth(mixStatus->width())); // 统计第二行不裁切。
    auto *syncStatus = view.findChild<QLabel *>("mediaSyncStatusLabel");
    CHECK(syncStatus && syncStatus->hasHeightForWidth());
    CHECK(syncStatus->height() >= syncStatus->heightForWidth(syncStatus->width())); // 同步状态两行完整显示。
    view.showRecordingStatus(QStringLiteral("已保存：C:/录制/") + QString(100, QChar(0x6d4b)) + QStringLiteral(".mp4\n视频 90 帧 · 音频 3.00 秒 · 编码 800 KB · 队列丢弃 0 帧 / 0 包"));
    QTest::qWait(30);
    auto *recordStatus = view.findChild<QLabel *>("recordingStatusLabel");
    CHECK(recordStatus && recordStatus->height() >= recordStatus->heightForWidth(recordStatus->width()));
    CHECK(!view.findChild<QCheckBox *>("recordingCheck")->isEnabled());
    view.showMixedAudio(0, QStringLiteral("48kHz · 立体声 · 10ms\n音频处理失败：") + QString(100, QChar(0x6d4b)));
    QTest::qWait(30);
    CHECK(mixStatus->height() >= mixStatus->heightForWidth(mixStatus->width())); // 错误详情可增高并滚动。
    view.showMixedAudio(0.3f, QStringLiteral("48kHz · 立体声 · 10ms\n已输出 100 包，削波 0 次"));
    QTest::qWait(30);

    // 只保存自建色块及人工构造的界面状态，不访问摄像头、麦克风或屏幕。
    if (!directory.isEmpty()) {
        CHECK(view.grab().save(directory + QStringLiteral("/主窗口紧凑布局.png")));
        scroll->ensureWidgetVisible(syncStatus);
        QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/音视频同步状态布局.png")));
        scroll->ensureWidgetVisible(view.findChild<QLabel *>("captureStatusLabel"));
        QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/采集状态换行布局.png")));
        scroll->verticalScrollBar()->setValue(0);
        view.setCaptureSources({{CaptureSource::Kind::Window, "layout", QStringLiteral("窗口 · 采集验证色块"), 1}});
        view.applyCaptureState(false, true, false, QStringLiteral("已发现 3 个采集目标"),
            {QStringLiteral("视频采集中"), QStringLiteral("未启用"), QStringLiteral("未启用")});
        view.resize(initialSize); QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/主窗口采集设置.png")));
        CHECK(view.grab().save(directory + QStringLiteral("/音频重采样混音页面.png")));
        view.findChild<QCheckBox *>("faceDetectionCheck")->setChecked(true);
        view.showFaceStatus(QStringLiteral("1 张人脸 · GPU 2.30 ms · 处理 5 ms · 帧龄 38 ms\n替换待处理帧 0 次 · 纹理注册 1 次"));
        scroll->ensureWidgetVisible(view.findChild<QLabel *>("faceStatusLabel"));
        QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/推流端人脸检测布局.png")));
        view.resize(compactSize);
        view.showFaceStatus(QStringLiteral("人脸检测失败，已恢复原画面：") + QString(100, QChar(0x6d4b)));
        QTest::qWait(30);
        scroll->ensureWidgetVisible(view.findChild<QLabel *>("faceStatusLabel"));
        QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/人脸检测错误换行布局.png")));
        scroll->ensureWidgetVisible(recordStatus);
        QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/本地录制紧凑布局.png")));
        view.resize(initialSize);
        preview->resize(QSize(720, 480).boundedTo(preview->screen()->availableGeometry().size() - QSize(40, 80)));
        QTest::qWait(30);
        // 原生 SwapChain 不在 QWidget backing store 中；实际画面在 WGC 专项测试取证。
        MainWindow player(ClientRole::Player);
        player.applyLoginState(QStringLiteral("已登录：root"), false, true);
        player.applySessionState(true, false, false, QStringLiteral("信令已连接，可以操作房间。"), {},
            {QJsonObject{{"roomId", "room-1"}, {"title", QStringLiteral("我的直播间")}, {"viewers", 0}, {"streaming", false}}});
        player.show(); QTest::qWait(30);
        CHECK(player.grab().save(directory + QStringLiteral("/播放端直播布局.png")));
        player.findChild<QComboBox *>("modeCombo")->setCurrentIndex(1); QTest::qWait(30);
        CHECK(player.grab().save(directory + QStringLiteral("/播放端点播布局.png")));
    }

    view.showCapturePreview({});
    CHECK(!preview->isVisible());
    view.showCapturePreview(frame); // 新一轮采集首帧允许再次自动打开。
    CHECK(preview->isVisible());
    view.applyLoginState(QStringLiteral("已退出登录"), false, false);
    CHECK(!preview->isVisible() && !open->isEnabled());
    view.showCapturePreview(frame); // 迟到的预览不能在退出后弹窗。
    CHECK(!preview->isVisible());
    view.applyLoginState(QStringLiteral("已登录：root"), false, true);
    view.showCapturePreview(frame);
    CHECK(preview->isVisible());
    // 非契约格式必须显示错误；空帧清理后可恢复到正常 GPU 绘制。
    Microsoft::WRL::ComPtr<ID3D11Device> latestDevice;
    frame.texture->GetDevice(&latestDevice);
    D3D11_TEXTURE2D_DESC invalidDescription{};
    frame.texture->GetDesc(&invalidDescription);
    invalidDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    VideoFrame invalidFrame;
    winrt::check_hresult(latestDevice->CreateTexture2D(&invalidDescription, nullptr, &invalidFrame.texture));
    view.showCapturePreview(invalidFrame);
    CHECK(until([&] { return failures == 1; }));
    CHECK(preview->isVisible() && !renderer->isVisible());
    view.showCapturePreview({});
    const int beforeRecovery = presented;
    view.showCapturePreview(frame);
    CHECK(until([&] { return presented > beforeRecovery; }));
    CHECK(failures == 1 && renderer->isVisible());
    view.close();
    CHECK(!preview->isVisible());
    return true;
}

bool gpuOutputHardware()
{
    qInfo("BEGIN GPU output quadrants / letterbox / resize / device switch");
    QImage image(320, 180, QImage::Format_RGB32);
    const QColor colors[]{QColor(224,48,32), QColor(32,192,64), QColor(32,80,224), QColor(224,192,32)};
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) image.setPixelColor(x, y, colors[(y >= 90 ? 2 : 0) + (x >= 160 ? 1 : 0)]);
    PreviewWindow preview(QStringLiteral("OBS GPU 渲染验证"), "waiting", nullptr);
    preview.resize(640, 480);
    QString error;
    QObject::connect(&preview, &PreviewWindow::failed, &preview, [&](const QString &message) { error = message; });
    auto frame = colorFrame(image);
    preview.setFrame(frame);
    // 模拟持续视频输入。WGC 重建观察帧池后需一次新的 Present 才会交付完整新尺寸。
    QTimer presentation;
    presentation.setInterval(50);
    QObject::connect(&presentation, &QTimer::timeout, &preview, [&] {
        ++frame.sequence;
        preview.setFrame(frame);
    });
    presentation.start();
    CHECK(QTest::qWaitForWindowExposed(&preview));
    VideoCapture observer;
    QObject::connect(&observer, &VideoCapture::failed, &preview, [&](const QString &message) { error = message; });
    CHECK(observer.begin({CaptureSource::Kind::Window, "gpu-output", "GPU output", quintptr(preview.winId())}));
    QImage output;
    const auto matches = [&] {
        if (!error.isEmpty()) return true;
        auto observed = observer.latestFrame();
        if (!observed.texture) return false;
        output = readBack(observed.texture.Get());
        return nearColor(output.pixelColor(output.width()/4, output.height()/3), colors[0])
            && nearColor(output.pixelColor(output.width()*3/4, output.height()/3), colors[1])
            && nearColor(output.pixelColor(output.width()/4, output.height()*2/3), colors[2])
            && nearColor(output.pixelColor(output.width()*3/4, output.height()*2/3), colors[3])
            && nearColor(output.pixelColor(output.width()/2, output.height()-20), QColor("#101824"));
    };
    CHECK(until(matches));
    if (!error.isEmpty()) qWarning().noquote() << error;
    CHECK(error.isEmpty());
    const auto directory = qEnvironmentVariable("OBS_UI_SCREENSHOTS");
    if (!directory.isEmpty()) CHECK(output.save(directory + QStringLiteral("/GPU四象限渲染验证.png")));
    // 改变源设备与窗口尺寸，仍验证最终显示内容；这里保留 4:3 外框供颜色采样。
    const int beforeWidth = output.width();
    frame = colorFrame(image, 2);
    preview.setFrame(frame); preview.resize(720, 540);
    const bool resized = until([&] { return matches() && output.width() > beforeWidth; });
    if (!resized) {
        qWarning() << "GPU output resize:" << preview.size() << "before" << beforeWidth << "after" << output.size()
            << "sequence" << observer.latestFrame().sequence << "error" << error;
        qWarning() << "Samples:" << output.pixelColor(output.width()/4, output.height()/3)
            << output.pixelColor(output.width()*3/4, output.height()/3)
            << output.pixelColor(output.width()/4, output.height()*2/3)
            << output.pixelColor(output.width()*3/4, output.height()*2/3);
        if (!directory.isEmpty()) output.save(directory + QStringLiteral("/GPU缩放诊断.png"));
    }
    CHECK(resized);
    CHECK(error.isEmpty());
    preview.showMinimized(); QTest::qWait(50);
    preview.present(); QTest::qWait(100);
    CHECK(until(matches) && error.isEmpty());
    observer.stop(); CHECK(observer.wait(10000));
    preview.setFrame({});
    qInfo("PASS GPU actual output / RGBA direction / letterbox / ResizeBuffers / device switch / restore");
    return true;
}

bool desktopHardware()
{
    // 只捕获测试自建的色块窗口，避免把桌面内容写入测试图片。
    QWidget target;
    target.setWindowTitle(QStringLiteral("OBS 采集验证色块"));
    target.setStyleSheet("background:rgb(32,160,96)");
    target.resize(400, 260); target.show();
    CHECK(QTest::qWaitForWindowExposed(&target));
    VideoCapture worker;
    QString error;
    QObject::connect(&worker, &VideoCapture::failed, &worker, [&error](const QString &message) { error = message; });
    const CaptureSource source{CaptureSource::Kind::Window, "test", QStringLiteral("窗口 · 采集验证色块"), quintptr(target.winId())};
    CHECK(worker.begin(source));
    CHECK(until([&] { return worker.latestFrame().texture != nullptr || !error.isEmpty(); }));
    if (!error.isEmpty()) qWarning().noquote() << error;
    auto first = worker.latestFrame();
    CHECK(error.isEmpty() && first.texture && first.timestamp100ns > 0);
    const auto firstImage = readBack(first.texture.Get());
    auto color = firstImage.pixelColor(firstImage.width() / 2, firstImage.height() / 2);
    CHECK(std::abs(color.red() - 32) < 8 && std::abs(color.green() - 160) < 8 && std::abs(color.blue() - 96) < 8);
    D3D11_TEXTURE2D_DESC before{}; first.texture->GetDesc(&before);
    QTimer animation;
    animation.setInterval(80);
    int animationFrame = 0;
    QObject::connect(&animation, &QTimer::timeout, &target, [&] {
        target.setStyleSheet(QString("background:rgb(%1,160,96)").arg(32 + (++animationFrame % 2)));
        target.update();
    });
    animation.start();
    target.resize(560, 340);
    const bool resized = until([&] {
        auto frame = worker.latestFrame();
        if (!frame.texture) return false;
        D3D11_TEXTURE2D_DESC after{}; frame.texture->GetDesc(&after);
        return after.Width > before.Width && after.Height > before.Height;
    });
    if (!resized) {
        auto latest = worker.latestFrame();
        D3D11_TEXTURE2D_DESC description{};
        if (latest.texture) latest.texture->GetDesc(&description);
        qWarning().noquote() << "Resize diagnostic:" << before.Width << before.Height << description.Width
            << description.Height << "sequence" << latest.sequence << "error" << error;
        RECT rectangle{}; GetWindowRect(reinterpret_cast<HWND>(target.winId()), &rectangle);
        qWarning() << "Target size:" << target.size() << rectangle.right - rectangle.left << rectangle.bottom - rectangle.top;
    }
    CHECK(resized);
    {
        MainWindow view(ClientRole::Publisher);
        LoginModel login; HttpClient http; LoginController auth(&view, &login, &http);
        SessionModel model; SignalClient signal;
        SessionController controller(&view, &login, &model, &signal);
        CHECK(until([&] { return view.findChild<QPushButton *>("refreshDevicesButton")->isEnabled(); }));
        login.completeLogin("test", "root", "root", 60);
        CHECK(view.findChild<QStackedWidget *>("pages")->currentIndex() == 1);
        view.setCaptureSources({source});
        view.findChild<QComboBox *>("videoSourceCombo")->setCurrentIndex(1);
        int delivered = 0;
        int synchronized = 0; qint64 lastPts = -1; bool timedValid = true;
        QObject::connect(&controller, &SessionController::videoFrameReady, &view, [&delivered](const VideoFrame &) { ++delivered; });
        QObject::connect(&controller, &SessionController::synchronizedVideoReady, &view, [&](const TimedVideoFrame &frame) {
            timedValid = timedValid && frame.source.video.texture && frame.pts > lastPts;
            lastPts = frame.pts; ++synchronized;
        });
        view.findChild<QPushButton *>("startCaptureButton")->click();
        CHECK(until([&] { return delivered > 3 && synchronized > 3; }));
        CHECK(timedValid);
        auto *preview = view.findChild<PreviewWindow *>("previewWindow");
        CHECK(preview && preview->isVisible() && preview->windowModality() == Qt::NonModal);
        preview->close();
        const int beforeClose = delivered;
        CHECK(until([&] { return delivered > beforeClose + 3; }));
        CHECK(!preview->isVisible()); // 真实采集继续，关闭画面不会停采或自动重新弹窗。
        view.findChild<QPushButton *>("showPreviewButton")->click();
        CHECK(preview->isVisible());
        view.show(); QTest::qWait(200);
        const auto screenshot = qEnvironmentVariable("OBS_CAPTURE_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            CHECK(view.grab().save(screenshot));
            VideoCapture observer;
            QString observerError;
            QObject::connect(&observer, &VideoCapture::failed, &observer, [&](const QString &message) { observerError = message; });
            CHECK(observer.begin({CaptureSource::Kind::Window, "preview", "preview", quintptr(preview->winId())}));
            QImage rendered;
            CHECK(until([&] {
                const auto observed = observer.latestFrame();
                if (!observed.texture) return !observerError.isEmpty();
                rendered = readBack(observed.texture.Get());
                return nearColor(rendered.pixelColor(rendered.width()/2, rendered.height()/2), QColor(32,160,96));
            }));
            CHECK(observerError.isEmpty() && !rendered.isNull());
            observer.stop(); CHECK(observer.wait(10000));
            CHECK(rendered.save(QFileInfo(screenshot).absolutePath() + QStringLiteral("/独立窗口真实采集.png")));
        }
        login.logout();
        const int afterLogout = synchronized;
        CHECK(!preview->isVisible());
        CHECK(until([&] { return !view.findChild<QPushButton *>("stopCaptureButton")->isEnabled(); }));
        CHECK(!view.findChild<QPushButton *>("startCaptureButton")->isEnabled());
        QTest::qWait(100); CHECK(synchronized == afterLogout);
        qInfo("PASS WGC controller synchronized video: %d frames, monotonic PTS, logout discard", synchronized);
    }
    worker.stop(); CHECK(worker.wait(10000));
    // 停止后，消费者保留的快照仍有效；系统采集池不会复用它。
    const auto retained = readBack(first.texture.Get());
    CHECK(first.texture && retained.pixelColor(retained.width() / 2, retained.height() / 2) == color);
    CHECK(worker.begin(source));
    CHECK(until([&] { return worker.latestFrame().texture != nullptr; }));
    target.close();
    CHECK(until([&] { return !error.isEmpty() && !worker.isRunning(); }));
    qInfo("PASS WGC window / color / resize / restart / target close");
    return true;
}
bool loopbackMixHardware(bool withVideo = false, bool record = false, bool detect = false)
{
    qInfo("BEGIN WASAPI loopback to resampler / mixer / MVC / stop tail / logout");
    MainWindow view(ClientRole::Publisher);
    LoginModel login; HttpClient http; LoginController auth(&view, &login, &http);
    SessionModel model; SignalClient signal;
    SessionController controller(&view, &login, &model, &signal);
    CHECK(until([&] { return view.findChild<QPushButton *>("refreshDevicesButton")->isEnabled(); }));
    auto *combo = view.findChild<QComboBox *>("systemAudioCombo");
    // 选与 WAVE_MAPPER 对应的默认输出，而不是假定机器只有一个输出设备。
    QString defaultId;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT probe = apartment;
    if (SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE) {
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> manager;
        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        probe = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager));
        if (SUCCEEDED(probe)) probe = manager->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
        LPWSTR id = nullptr;
        if (SUCCEEDED(probe)) probe = endpoint->GetId(&id);
        if (SUCCEEDED(probe)) { defaultId = QString::fromWCharArray(id); CoTaskMemFree(id); }
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    int selected = -1;
    for (int index = 1; index < combo->count(); ++index)
        if (combo->itemData(index).value<CaptureSource>().id == defaultId) selected = index;
    if (selected < 1) {
        qInfo("SKIP loopback mix hardware: no enumerated default output; endpoints=%d HRESULT=0x%08x", combo->count() - 1, unsigned(probe));
        qInfo().noquote() << view.findChild<QLabel *>("captureStatusLabel")->text();
        return true;
    }
    combo->setCurrentIndex(selected);
    const auto recordingDirectory = QDir::current().filePath(QStringLiteral("out/采集编码验证_%1").arg(QDateTime::currentMSecsSinceEpoch()));
    if (record) {
        view.findChild<QCheckBox *>("recordingCheck")->setChecked(true);
        view.findChild<QLineEdit *>("recordingDirectoryEdit")->setText(recordingDirectory);
        view.findChild<QCheckBox *>("systemAudioMute")->setChecked(true); // 保存测试静音，避免录入其他应用声音。
    }
    if (detect) view.findChild<QCheckBox *>("faceDetectionCheck")->setChecked(true);
    QWidget target;
    QTimer animation;
    if (withVideo) {
        const auto endpoint = combo->currentData().value<CaptureSource>();
        target.setWindowTitle(QStringLiteral("音视频同步验证色块"));
        target.setStyleSheet(QStringLiteral("background:rgb(32,160,96)"));
        target.resize(320, 180); target.show();
        CHECK(QTest::qWaitForWindowExposed(&target));
        view.setCaptureSources({endpoint, {CaptureSource::Kind::Window, "sync-fixture", QStringLiteral("同步验证色块"), quintptr(target.winId())}});
        view.findChild<QComboBox *>("videoSourceCombo")->setCurrentIndex(1);
        animation.setInterval(100);
        QObject::connect(&animation, &QTimer::timeout, &target, [&target] {
            static bool light = false; light = !light;
            target.setStyleSheet(light ? QStringLiteral("background:rgb(64,128,192)") : QStringLiteral("background:rgb(32,160,96)"));
        });
        animation.start();
    }
    login.completeLogin("test", "root", "root", 60);
    int mixed = 0;
    int synchronized = 0; qint64 lastPts = -1;
    int synchronizedVideo = 0; qint64 lastVideoPts = -1, origin = 0;
    bool valid = true;
    qint64 last = 0;
    QObject::connect(&controller, &SessionController::mixedAudioReady, &view, [&](const AudioPacket &packet) {
        ++mixed;
        valid = valid && packet.channels == 2 && packet.channelMask == 3 && packet.sampleRate == 48000
            && packet.pcm.size() == 480 * 8 && packet.timestamp100ns > last;
        last = packet.timestamp100ns;
    });
    QObject::connect(&controller, &SessionController::synchronizedAudioReady, &view, [&](const TimedAudioPacket &packet) {
        valid = valid && packet.pts >= 0 && packet.pts > lastPts && packet.source.pcm.size() == 480 * 8;
        const qint64 inferred = packet.source.timestamp100ns - packet.pts * 10000000 / 48000;
        if (!origin) origin = inferred;
        valid = valid && std::abs(inferred - origin) <= 220; // 音频PTS取整误差最多一个48k采样。
        lastPts = packet.pts; ++synchronized;
    });
    QObject::connect(&controller, &SessionController::synchronizedVideoReady, &view, [&](const TimedVideoFrame &frame) {
        valid = valid && frame.pts > lastVideoPts && frame.source.video.texture;
        if (origin) {
            const qint64 presentation = origin + frame.pts * 10000000 / 30;
            valid = valid && frame.source.video.timestamp100ns <= presentation + 220;
        }
        lastVideoPts = frame.pts; ++synchronizedVideo;
    });
    // 只播放人工静音以驱动默认输出，捕获数据不保存，不打开屏幕/摄像头/麦克风。
    HWAVEOUT output = nullptr;
    auto waveFormat = format(16);
    const auto opened = waveOutOpen(&output, WAVE_MAPPER, &waveFormat, 0, 0, CALLBACK_NULL);
    QByteArray silence(48000 * 4 * 3, '\0');
    WAVEHDR header{}; header.lpData = silence.data(); header.dwBufferLength = DWORD(silence.size());
    MMRESULT prepared = MMSYSERR_ERROR, written = MMSYSERR_ERROR;
    bool received = false, stopped = false, restarted = false, loggedOut = false, noLate = false;
    if (opened == MMSYSERR_NOERROR) {
        prepared = waveOutPrepareHeader(output, &header, sizeof(header));
        if (prepared == MMSYSERR_NOERROR) written = waveOutWrite(output, &header, sizeof(header));
        if (written == MMSYSERR_NOERROR) {
            view.findChild<QPushButton *>("startCaptureButton")->click();
            received = until([&] { return mixed >= 15 && synchronized >= 10 && (!withVideo || synchronizedVideo >= 3); });
            if (record) QTest::qWait(1500);
            if (detect) qInfo().noquote() << view.findChild<QLabel *>("faceStatusLabel")->text();
            view.findChild<QPushButton *>("stopCaptureButton")->click();
            stopped = until([&] { return !view.findChild<QPushButton *>("stopCaptureButton")->isEnabled(); });
            const int afterStop = mixed;
            const int syncAfterStop = synchronized;
            const int videoAfterStop = synchronizedVideo;
            QTest::qWait(100); noLate = mixed == afterStop && synchronized == syncAfterStop && synchronizedVideo == videoAfterStop;
            lastPts = -1; // 新一轮共用原点重置，PTS从新时间线重新开始。
            lastVideoPts = -1; origin = 0;
            view.findChild<QPushButton *>("startCaptureButton")->click();
            restarted = until([&] { return mixed > afterStop + 10 && (!withVideo || synchronizedVideo > videoAfterStop + 3); });
            login.logout();
            const int afterLogout = mixed;
            const int syncAfterLogout = synchronized;
            const int videoAfterLogout = synchronizedVideo;
            loggedOut = until([&] { return !view.findChild<QPushButton *>("stopCaptureButton")->isEnabled(); });
            QTest::qWait(100); noLate = noLate && mixed == afterLogout && synchronized == syncAfterLogout && synchronizedVideo == videoAfterLogout;
        }
        // 无论验证结果如何，先停止播放、归还缓冲，再做会提前return的断言。
        waveOutReset(output);
        if (prepared == MMSYSERR_NOERROR) waveOutUnprepareHeader(output, &header, sizeof(header));
        waveOutClose(output);
    }
    qInfo().noquote() << view.findChild<QLabel *>("audioMixStatusLabel")->text();
    CHECK(opened == MMSYSERR_NOERROR && prepared == MMSYSERR_NOERROR && written == MMSYSERR_NOERROR);
    CHECK(received && stopped && restarted && loggedOut && noLate && valid);
    if (record) {
        qInfo().noquote() << view.findChild<QLabel *>("recordingStatusLabel")->text();
        const auto files = QDir(recordingDirectory).entryList({"*.mp4"}, QDir::Files);
        CHECK(files.size() == 2 && view.findChild<QLabel *>("recordingStatusLabel")->text().contains(QStringLiteral("已保存")));
        for (const auto &name : files) CHECK(QFileInfo(QDir(recordingDirectory).filePath(name)).size() > 1000);
        qInfo().noquote() << "PASS Controller -> WGC/WASAPI -> timeline -> MP4; normal stop, restart, logout:" << recordingDirectory;
    }
    if (detect) CHECK(!view.findChild<QLabel *>("faceStatusLabel")->text().contains(QStringLiteral("失败")));
    qInfo("PASS WASAPI to mixed/synchronized packets: %d / %d packets; stop/restart/logout, no late delivery", mixed, synchronized);
    if (withVideo) qInfo("PASS joint WGC/WASAPI timeline: %d synchronized video frames, shared origin / restart / no late delivery", synchronizedVideo);
    return true;
}

bool deviceHardware()
{
    MainWindow view(ClientRole::Publisher);
    LoginModel login; HttpClient http; LoginController auth(&view, &login, &http);
    SessionModel model; SignalClient signal;
    SessionController controller(&view, &login, &model, &signal);
    CHECK(until([&] { return view.findChild<QPushButton *>("refreshDevicesButton")->isEnabled(); }));
    QList<CaptureSource> sources;
    for (const auto *name : {"videoSourceCombo", "microphoneCombo", "systemAudioCombo"}) {
        auto *combo = view.findChild<QComboBox *>(name);
        for (int index = 1; index < combo->count(); ++index) sources.append(combo->itemData(index).value<CaptureSource>());
    }
    int cameras = 0, microphones = 0, outputs = 0, monitors = 0;
    for (const auto &source : sources) {
        cameras += source.kind == CaptureSource::Kind::Camera;
        microphones += source.kind == CaptureSource::Kind::Microphone;
        outputs += source.kind == CaptureSource::Kind::Loopback;
        monitors += source.kind == CaptureSource::Kind::Monitor;
    }
    qInfo("Devices: cameras=%d microphones=%d outputs=%d monitors=%d", cameras, microphones, outputs, monitors);
    for (const auto &source : sources) {
        if (source.kind != CaptureSource::Kind::Monitor) continue;
        VideoCapture screen;
        QString error;
        QObject::connect(&screen, &VideoCapture::failed, &screen, [&error](const QString &message) { error = message; });
        CHECK(screen.begin(source));
        const bool received = until([&] { return screen.latestFrame().texture != nullptr || !error.isEmpty(); });
        screen.stop(); const bool stopped = screen.wait(10000);
        if (!error.isEmpty()) qWarning().noquote() << error;
        CHECK(received && stopped && error.isEmpty());
        qInfo("PASS WGC monitor GPU frame / stop (no image saved)");
        break;
    }
    login.completeLogin("test", "root", "root", 60);
    QList<CaptureSource> selection;
    for (auto kind : {CaptureSource::Kind::Camera, CaptureSource::Kind::Microphone, CaptureSource::Kind::Loopback})
        for (const auto &source : sources) if (source.kind == kind) { selection.append(source); break; }
    if (selection.isEmpty()) { qInfo("SKIP camera/audio hardware: no devices"); return true; }
    int frames = 0, micPackets = 0, systemPackets = 0;
    QObject::connect(&controller, &SessionController::videoFrameReady, &view, [&frames](const VideoFrame &frame) {
        if (frame.texture && frame.timestamp100ns > 0) ++frames;
    });
    QObject::connect(&controller, &SessionController::audioPacketReady, &view, [&](CaptureSource::Kind kind, const AudioPacket &packet) {
        if (packet.channels > 0 && packet.sampleRate > 0 && !packet.pcm.isEmpty() && packet.timestamp100ns > 0) {
            if (kind == CaptureSource::Kind::Microphone) ++micPackets; else ++systemPackets;
        }
    });
    view.captureRequested(selection);
    QTest::qWait(4000);
    qInfo().noquote() << view.findChild<QLabel *>("captureStatusLabel")->text();
    qInfo("Captured: cameraFrames=%d microphonePackets=%d systemPackets=%d", frames, micPackets, systemPackets);
    if (cameras) CHECK(frames > 0);
    if (microphones) CHECK(micPackets > 0);
    if (outputs) CHECK(view.findChild<QLabel *>("captureStatusLabel")->text().contains(QStringLiteral("系统声音：音频采集中")));
    if (!systemPackets && outputs) qInfo("SKIP loopback packet verification: no system playback; initialized successfully");
    login.logout(); CHECK(until([&] { return !view.findChild<QPushButton *>("stopCaptureButton")->isEnabled(); }));
    CHECK(!view.findChild<QPushButton *>("startCaptureButton")->isEnabled());
    qInfo("PASS available camera/audio lifecycle and logout");
    // 用无声音频驱动默认输出端点，验证回环实际交付及慢消费者边界，不依赖用户播放音乐。
    if (outputs == 1) {
        CaptureSource source;
        for (const auto &entry : sources) if (entry.kind == CaptureSource::Kind::Loopback) source = entry;
        WasapiCapture loopback;
        QString error;
        QObject::connect(&loopback, &WasapiCapture::failed, &loopback, [&error](const QString &message) { error = message; });
        CHECK(loopback.begin(source));
        QTest::qWait(100);
        HWAVEOUT output = nullptr;
        auto waveFormat = format(16);
        const MMRESULT opened = waveOutOpen(&output, WAVE_MAPPER, &waveFormat, 0, 0, CALLBACK_NULL);
        QByteArray silence(48000 * 4 * 2, '\0');
        WAVEHDR header{}; header.lpData = silence.data(); header.dwBufferLength = DWORD(silence.size());
        MMRESULT prepared = MMSYSERR_ERROR, written = MMSYSERR_ERROR;
        if (opened == MMSYSERR_NOERROR) {
            prepared = waveOutPrepareHeader(output, &header, sizeof(header));
            if (prepared == MMSYSERR_NOERROR) written = waveOutWrite(output, &header, sizeof(header));
        }
        QTest::qWait(1200); // 不消费，故意让最多50包的邮箱达到上限。
        auto packets = loopback.takePackets();
        loopback.stop();
        const bool stopped = loopback.wait(10000);
        // 先结束播放并归还缓冲，之后才能做断言或销毁 QByteArray。
        if (opened == MMSYSERR_NOERROR) {
            waveOutReset(output);
            if (prepared == MMSYSERR_NOERROR) waveOutUnprepareHeader(output, &header, sizeof(header));
            waveOutClose(output);
        }
        CHECK(opened == MMSYSERR_NOERROR && prepared == MMSYSERR_NOERROR && written == MMSYSERR_NOERROR);
        if (!error.isEmpty()) qWarning().noquote() << error;
        CHECK(stopped && error.isEmpty() && !packets.isEmpty() && packets.size() <= 50);
        CHECK(packets.first().timestamp100ns > 0 && packets.first().channels > 0 && packets.first().sampleRate > 0);
        CHECK(packets.first().discontinuity); // 实际丢弃了较早的包，消费者必须收到不连续标记。
        for (qsizetype index = 1; index < packets.size(); ++index)
            CHECK(packets[index].timestamp100ns >= packets[index - 1].timestamp100ns);
        qInfo("PASS WASAPI loopback PCM / QPC timestamps / bounded queue: %lld packets", static_cast<long long>(packets.size()));
    }
    return true;
}
}
int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toUtf8(); std::fprintf(stderr, "%s\n", bytes.constData()); std::fflush(stderr);
    });
    QApplication app(argc, argv);
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (fontId >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).constFirst(), 10));
    const bool hardware = app.arguments().contains("--hardware");
    if (app.arguments().contains("--ui-only")) return previewUiLifecycle() ? 0 : 1;
    if (app.arguments().contains("--window-preview")) return gpuOutputHardware() && desktopHardware() ? 0 : 1;
    if (app.arguments().contains("--audio-mix")) return loopbackMixHardware() ? 0 : 1;
    if (app.arguments().contains("--media-sync")) return loopbackMixHardware(true) ? 0 : 1;
    if (app.arguments().contains("--record")) return loopbackMixHardware(true, true) ? 0 : 1;
    if (app.arguments().contains("--record-face")) return loopbackMixHardware(true, true, true) ? 0 : 1;
    if (hardware) return gpuOutputHardware() && desktopHardware() && deviceHardware() ? 0 : 1;
    if (!audioFormats() || !failedWorkerRestart() || !captureMvc() || !previewUiLifecycle()) return 1;
    qInfo("PASS 4 capture groups: PCM formats / failed worker restart / MVC and logout / independent preview");
    return 0;
}
