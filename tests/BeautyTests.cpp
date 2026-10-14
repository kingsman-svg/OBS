#include "GpuBeauty.h"
#include "GpuFaceDetector.h"
#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "LoginModel.h"
#include "mainwindow.h"
#include "PreviewWindow.h"
#include "ffmpeg/MediaDecoder.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <winrt/base.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace csn;
using Microsoft::WRL::ComPtr;
#define CHECK(value) do { if (!(value)) { qCritical("FAIL line %d: %s", __LINE__, #value); return false; } } while (0)
namespace {
bool until(const std::function<bool()> &condition, int timeout = 10000)
{
    QElapsedTimer timer; timer.start(); while (!condition() && timer.elapsed() < timeout) QTest::qWait(5); return condition();
}
VideoFrame texture(const QImage &source, ID3D11Device *device, quint64 sequence = 1)
{
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = UINT(image.width()); desc.Height = UINT(image.height());
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; const D3D11_SUBRESOURCE_DATA data{image.constBits(), UINT(image.bytesPerLine()), 0};
    VideoFrame frame; frame.sequence = sequence; frame.timestamp100ns = 123450;
    winrt::check_hresult(device->CreateTexture2D(&desc, &data, &frame.texture)); return frame;
}
QImage pixels(ID3D11Texture2D *source)
{
    // 只在验证中读回；应用处理与编码链路不下载视频图像。
    ComPtr<ID3D11Device> device; source->GetDevice(&device); ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc); desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy; winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &copy)); context->CopyResource(copy.Get(), source);
    D3D11_MAPPED_SUBRESOURCE data{}; winrt::check_hresult(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &data));
    QImage result(static_cast<const uchar *>(data.pData), int(desc.Width), int(desc.Height), int(data.RowPitch), QImage::Format_ARGB32);
    result = result.copy(); context->Unmap(copy.Get(), 0); return result;
}
bool shaderChecks()
{
    ComPtr<ID3D11Device> device; winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    QImage image(80, 80, QImage::Format_ARGB32); image.fill(QColor(25, 50, 150));
    for (int y = 12; y < 68; ++y) for (int x = 12; x < 68; ++x) {
        const int noise = (x + y) % 2 ? 12 : -12; image.setPixelColor(x, y, QColor(180 + noise, 135 + noise, 110 + noise));
    }
    FaceFrame frame; frame.video = texture(image, device.Get());
    FaceDetection face; face.box = {12, 12, 56, 56}; face.confidence = 0.95f;
    face.landmarks = {QPointF(28, 28), QPointF(52, 28), QPointF(40, 40), QPointF(28, 55), QPointF(52, 55)}; frame.faces = {face};
    GpuBeauty beauty(device.Get()); const auto cancelled = [] { return false; };
    CHECK(beauty.apply(frame, {}, cancelled).texture.Get() == frame.video.texture.Get());
    CHECK(beauty.apply(frame, {true, 0, 0}, cancelled).texture.Get() == frame.video.texture.Get());
    auto noFace = frame; noFace.faces.clear(); CHECK(beauty.apply(noFace, {true, 1, 1}, cancelled).texture.Get() == frame.video.texture.Get());
    auto invalid = frame; invalid.faces[0].box.setWidth(std::numeric_limits<double>::quiet_NaN());
    CHECK(beauty.apply(invalid, {true, 1, 1}, cancelled).texture.Get() == frame.video.texture.Get());
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    const D3D11_VIEWPORT sentinel{3, 5, 7, 9, 0, 1}; context->RSSetViewports(1, &sentinel);
    const auto smooth = beauty.apply(frame, {true, 1, 0}, cancelled); const auto softened = pixels(smooth.texture.Get());
    CHECK(smooth.texture.Get() != frame.video.texture.Get() && smooth.sequence == frame.video.sequence && smooth.timestamp100ns == frame.video.timestamp100ns);
    D3D11_VIEWPORT restored{}; UINT count = 1; context->RSGetViewports(&count, &restored);
    CHECK(count == 1 && restored.TopLeftX == 3 && restored.Height == 9);
    double beforeNoise = 0, afterNoise = 0;
    for (int y = 35; y <= 45; ++y) for (int x = 22; x <= 26; ++x) {
        beforeNoise += std::abs(image.pixelColor(x, y).red() - image.pixelColor(x + 1, y).red());
        afterNoise += std::abs(softened.pixelColor(x, y).red() - softened.pixelColor(x + 1, y).red());
    }
    CHECK(afterNoise < beforeNoise * 0.8 && softened.pixel(3, 3) == image.pixel(3, 3));
    CHECK(softened.pixel(28, 28) == image.pixel(28, 28)); // 双眼中心不被磨皮。
    const auto bright = beauty.apply(frame, {true, 0, 1}, cancelled); const auto lightened = pixels(bright.texture.Get());
    CHECK(lightened.pixelColor(24, 40).red() > image.pixelColor(24, 40).red() + 3);
    for (int y = 0; y < 80; ++y) for (int x = 0; x < 80; ++x)
        if (x < 12 || x >= 68 || y < 12 || y >= 68) CHECK(lightened.pixel(x, y) == image.pixel(x, y));
    CHECK(pixels(frame.video.texture.Get()) == image && pixels(smooth.texture.Get()) == softened);
    QImage resized(128, 96, QImage::Format_ARGB32); resized.fill(QColor(180, 135, 110));
    auto changed = frame; changed.video = texture(resized, device.Get(), 2);
    CHECK(pixels(beauty.apply(changed, {true, 0.5f, 0.5f}, cancelled).texture.Get()).size() == resized.size());
    CHECK(pixels(smooth.texture.Get()) == softened);
    bool stopped = false; try { beauty.apply(frame, {true, 1, 1}, [] { return true; }); } catch (const std::exception &) { stopped = true; }
    CHECK(stopped); QDir().mkpath("out/美颜验证");
    CHECK(image.save("out/美颜验证/合成原图.png") && softened.save("out/美颜验证/合成磨皮.png") && lightened.save("out/美颜验证/合成提亮.png"));
    qInfo("PASS beauty Shader: face mask / background / smoothing %.0f->%.0f / brightness / bypass / source immutable / retained output / resize / context / cancel",
        beforeNoise, afterNoise); return true;
}
bool uiChecks()
{
    MainWindow view(ClientRole::Publisher); view.applyLoginState("root", false, true); view.resize(640, 480); view.show();
    auto *smooth = view.findChild<QSpinBox *>("beautySmoothingSpin"), *bright = view.findChild<QSpinBox *>("beautyBrightnessSpin");
    auto *toggle = view.findChild<QCheckBox *>("beautyCheck"); CHECK(smooth && bright && toggle);
    QSignalSpy changes(&view, &MainWindow::beautyStrengthChanged);
    toggle->setChecked(true); smooth->setValue(70); bright->setValue(30);
    const auto settings = view.beautySettings(); CHECK(settings.enabled && std::abs(settings.smoothing - 0.7f) < 0.01f && changes.size() == 2);
    view.applyCaptureState(false, true, false, "test", {}); CHECK(!toggle->isEnabled() && smooth->isEnabled() && bright->isEnabled());
    auto *scroll = view.findChild<QScrollArea *>("captureScroll");
    if (!scroll) scroll = view.findChild<QScrollArea *>(); CHECK(scroll);
    scroll->ensureWidgetVisible(view.findChild<QLabel *>("faceStatusLabel")); QTest::qWait(50);
    CHECK(smooth->width() >= smooth->minimumSizeHint().width());
    CHECK(scroll->viewport()->rect().contains(QRect(smooth->mapTo(scroll->viewport(), QPoint()), smooth->size())));
    CHECK(scroll->viewport()->rect().contains(QRect(bright->mapTo(scroll->viewport(), QPoint()), bright->size())));
    const auto screenshot = qEnvironmentVariable("QT_SCALE_FACTOR") == "2" ? QStringLiteral("out/美颜验证/美颜控件二倍缩放.png")
        : QStringLiteral("out/美颜验证/美颜控件页面.png");
    CHECK(view.grab().save(screenshot)); qInfo("PASS beauty UI: values / real-time intent / run locks / compact scrolling layout"); return true;
}
bool hardware(const QString &engine, const QString &imagePath)
{
    const QImage image(imagePath); CHECK(!image.isNull());
    ComPtr<ID3D11Device> device; winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    auto input = texture(image, device.Get()); GpuFaceDetector detector; QSignalSpy errors(&detector, &GpuFaceDetector::failed);
    CHECK(detector.begin(engine, {true, 0.7f, 0.7f}, false)); detector.submit(input);
    CHECK(until([&] { return detector.latestResult().video.texture || !errors.isEmpty(); }, 20000));
    if (!errors.isEmpty()) qWarning().noquote() << errors.first().first().toString(); CHECK(errors.isEmpty());
    const auto result = detector.latestResult(); CHECK(result.faces.size() > 0 && result.beautified && !result.drawFaces);
    CHECK(result.video.sequence == input.sequence && result.video.timestamp100ns == input.timestamp100ns);
    const auto before = pixels(input.texture.Get()), after = pixels(result.video.texture.Get()); CHECK(before != after);
    CHECK(before.save("out/美颜验证/人脸原图.png") && after.save("out/美颜验证/人脸美颜.png"));
    qint64 warmBeauty = 0, warmProcessing = 0;
    for (quint64 sequence = 2; sequence <= 6; ++sequence) {
        input.sequence = sequence; detector.submit(input);
        CHECK(until([&] { return detector.latestResult().video.sequence == sequence; }));
        const auto warm = detector.latestResult(); CHECK(warm.beautified && warm.registrations == 1);
        CHECK(pixels(warm.video.texture.Get()) == after); warmBeauty += warm.beautyMs; warmProcessing += warm.processingMs;
    }
    qInfo("beauty warm average: processing=%.2fms beauty=%.2fms (5 repeated image frames, wall time)", warmProcessing / 5.0, warmBeauty / 5.0);
    detector.setBeautyStrength(0, 0); input.sequence = 7; detector.submit(input);
    CHECK(until([&] { return detector.latestResult().video.sequence == 7; }));
    CHECK(!detector.latestResult().beautified && detector.latestResult().video.texture.Get() == input.texture.Get());
    detector.stop(); CHECK(detector.wait(3000) && !detector.latestResult().video.texture);
    CHECK(pixels(result.video.texture.Get()) == after && pixels(input.texture.Get()) == before);
    CHECK(detector.begin(engine, {true, 0, 0.7f}, false)); input.sequence = 1; detector.submit(input);
    CHECK(until([&] { return detector.latestResult().beautified || !errors.isEmpty(); }, 20000)); CHECK(errors.isEmpty());
    detector.stop(); CHECK(detector.wait(3000));
    qInfo("PASS TensorRT -> beauty: detected=%lld processing=%lldms beauty=%lldms / same-frame GPU / zero strength / no overlay / retained output / stop / restart",
        result.faces.size(), result.processingMs, result.beautyMs);
    // 实际Publisher只采集此测试自己创建的示例图窗口；同步输出同时供预览和MP4编码。
    QLabel fixture; fixture.setPixmap(QPixmap::fromImage(image)); fixture.setScaledContents(true); fixture.resize(image.size()); fixture.show();
    CHECK(QTest::qWaitForWindowExposed(&fixture));
    QTemporaryDir directory("out/beauty-record-XXXXXX"); CHECK(directory.isValid());
    MainWindow view(ClientRole::Publisher); LoginModel login; SessionModel session; SignalClient signal;
    SessionController controller(&view, &login, &session, &signal); view.applyLoginState("root", false, true);
    login.completeLogin("beauty-test", "root", "root", 60);
    CHECK(until([&] { return view.findChild<QPushButton *>("startCaptureButton")->isEnabled(); }));
    view.setCaptureSources({{CaptureSource::Kind::Window, "beauty-fixture", "test", quintptr(fixture.winId())}});
    view.findChild<QComboBox *>("videoSourceCombo")->setCurrentIndex(1);
    view.findChild<QCheckBox *>("beautyCheck")->setChecked(true);
    view.findChild<QLineEdit *>("faceEngineEdit")->setText("missing-beauty.engine");
    view.findChild<QLineEdit *>("recordingDirectoryEdit")->setText(directory.path());
    int effects = 0, originalFrames = 0; bool valid = true; qint64 last = -1;
    QObject::connect(&controller, &SessionController::synchronizedVideoReady, &view, [&](const TimedVideoFrame &frame) {
        valid = valid && frame.pts > last; last = frame.pts;
        if (frame.source.beautified && !frame.source.drawFaces) ++effects;
        if (!frame.source.beautified) ++originalFrames;
    });
    view.findChild<QPushButton *>("startCaptureButton")->click();
    CHECK(until([&] { return originalFrames > 0 && view.findChild<QLabel *>("faceStatusLabel")->text().contains(QStringLiteral("失败")); }));
    view.findChild<QPushButton *>("stopCaptureButton")->click();
    CHECK(until([&] { return view.findChild<QPushButton *>("startCaptureButton")->isEnabled(); }));
    CHECK(effects == 0); last = -1; valid = true;
    view.findChild<QLineEdit *>("faceEngineEdit")->setText(engine);
    view.findChild<QCheckBox *>("recordingCheck")->setChecked(true);
    view.findChild<QPushButton *>("startCaptureButton")->click();
    CHECK(until([&] { return effects >= 25; }, 20000));
    CHECK(valid && view.findChild<PreviewWindow *>()->isVisible());
    view.findChild<QPushButton *>("stopCaptureButton")->click();
    CHECK(until([&] { return view.findChild<QPushButton *>("startCaptureButton")->isEnabled(); }, 10000));
    const auto files = QDir(directory.path()).entryList({"*.mp4"}, QDir::Files); CHECK(files.size() == 1);
    int decoded = 0; MediaDecoder decoder; decoder.open(QDir(directory.path()).filePath(files.first()), [] { return false; },
        [&](const PlaybackVideo &) { ++decoded; }, [](const PlaybackAudio &) {});
    while (decoder.readNext()) {} decoder.finish(); CHECK(decoded >= 20);
    qInfo("PASS Publisher MVC: missing-engine fallback / retry / generated face window -> WGC -> TensorRT -> beauty -> timeline -> preview / NVENC MP4 -> GPU decode, effects=%d decoded=%d", effects, decoded);
    return true;
}
}
int main(int argc, char **argv)
{
    SetErrorMode(0x8003); qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData()); std::fflush(stderr);
    });
    QApplication app(argc, argv); QDir().mkpath("out/美颜验证");
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).constFirst(), 10));
    const auto value = [&](const QString &key) { const int i = app.arguments().indexOf(key); return i >= 0 && i + 1 < app.arguments().size() ? app.arguments()[i + 1] : QString(); };
    try { if (!shaderChecks() || !uiChecks()) return 1; return !app.arguments().contains("--gpu") || hardware(value("--engine"), value("--image")) ? 0 : 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
    catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
