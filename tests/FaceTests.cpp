#include "FaceDetection.h"
#include "GpuFaceDetector.h"
#include "VideoRenderer.h"
#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "LoginModel.h"
#include "LoginController.h"
#include "HttpClient.h"
#include "mainwindow.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QScreen>
#include <QLineF>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <winrt/base.h>
#include <functional>
#include <limits>
#include <cstdio>
#ifdef OBS_FACE_REFERENCE_TESTS
#include "EngineSession.h"
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <cuda_d3d11_interop.h>
#include <dxgi.h>
#endif

using namespace csn;
#define CHECK(condition) do { if (!(condition)) { qCritical("Check failed at line %d: %s", __LINE__, #condition); return false; } } while (false)
namespace {
bool until(const std::function<bool()> &predicate, int timeout = 10000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) QTest::qWait(5);
    return predicate();
}
FaceTensors emptyTensors()
{
    FaceTensors result;
    for (int stride : {8, 16, 32}) {
        const std::string suffix = "_" + std::to_string(stride);
        const size_t count = size_t(640 / stride) * (640 / stride) * 2;
        result["score" + suffix].resize(count);
        result["bbox" + suffix].resize(count * 4);
        result["kps" + suffix].resize(count * 10);
    }
    return result;
}
bool decoder()
{
    auto tensors = emptyTensors();
    CHECK(scrfdResize(QSize(1920, 1080)) == QSize(640, 360));
    CHECK(decodeScrfd(tensors, QSize(640, 640)).isEmpty());
    const size_t anchor = size_t(5 * 80 + 4) * 2 + 1; // 第二个 anchor，中心为32,40。
    tensors["score_8"][anchor] = 0.9f;
    for (int j = 0; j < 4; ++j) tensors["bbox_8"][4 * anchor + j] = float(j + 1);
    for (int j = 0; j < 10; ++j) tensors["kps_8"][10 * anchor + j] = 0.5f;
    auto faces = decodeScrfd(tensors, QSize(1280, 720));
    CHECK(faces.size() == 1 && faces[0].box == QRectF(48, 48, 64, 96));
    CHECK(faces[0].landmarks[0] == QPointF(72, 88));
    // 相同位置的第一 anchor 必须被更高分的第二 anchor 抑制。
    tensors["score_8"][anchor - 1] = 0.8f;
    std::copy_n(tensors["bbox_8"].begin() + 4 * anchor, 4, tensors["bbox_8"].begin() + 4 * (anchor - 1));
    CHECK(decodeScrfd(tensors, QSize(1280, 720)).size() == 1);
    tensors["kps_8"][0] = std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try { decodeScrfd(tensors, QSize(640, 640)); } catch (const std::exception &) { rejected = true; }
    CHECK(rejected);
    tensors = emptyTensors(); tensors["score_8"].pop_back(); rejected = false;
    try { decodeScrfd(tensors, QSize(640, 640)); } catch (const std::exception &) { rejected = true; }
    CHECK(rejected);
    qInfo("PASS SCRFD anchor / NMS / wide-frame inverse scale / malformed output");
    return true;
}
bool lifecycle()
{
    GpuFaceDetector detector; QSignalSpy errors(&detector, &GpuFaceDetector::failed);
    CHECK(detector.begin(QStringLiteral("missing-face-engine.engine")));
    CHECK(until([&] { return !detector.isRunning() && errors.size() == 1; }));
    CHECK(!detector.latestResult().video.texture);
    // 一个有内容但尚未反序列化的文件；等待首帧时 stop 必须能立即唤醒。
    QTemporaryDir directory; CHECK(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("等待首帧.engine"));
    QFile file(path); CHECK(file.open(QIODevice::WriteOnly)); file.write("not-an-engine"); file.close();
    for (int i = 0; i < 3; ++i) {
        CHECK(detector.begin(path)); QTest::qWait(20); detector.stop();
        CHECK(until([&] { return !detector.isRunning(); }));
        CHECK(!detector.latestResult().video.texture && errors.size() == 1);
    }
    qInfo("PASS missing engine / idle stop / three restarts / cleared result");
    return true;
}
VideoFrame imageTexture(const QImage &source, quint64 sequence, ID3D11Device *reuse = nullptr)
{
    const auto image = source.convertToFormat(QImage::Format_ARGB32);
    Microsoft::WRL::ComPtr<ID3D11Device> device = reuse;
    if (!device) winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    D3D11_TEXTURE2D_DESC description{};
    description.Width = UINT(image.width()); description.Height = UINT(image.height());
    description.MipLevels = 1; description.ArraySize = 1; description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{image.constBits(), UINT(image.bytesPerLine()), 0};
    VideoFrame result; result.sequence = sequence;
    LARGE_INTEGER value{}, frequency{}; QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    result.timestamp100ns = value.QuadPart / frequency.QuadPart * 10000000 + value.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
    winrt::check_hresult(device->CreateTexture2D(&description, &data, &result.texture));
    return result;
}
QImage readBack(ID3D11Texture2D *source)
{
    // 仅硬件验收读回测试窗口 WGC 纹理；客户端视频处理和预览没有此路径。
    Microsoft::WRL::ComPtr<ID3D11Device> device; source->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.BindFlags = 0; desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &staging)); context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{}; winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const QImage view(static_cast<const uchar *>(mapped.pData), int(desc.Width), int(desc.Height), int(mapped.RowPitch), QImage::Format_ARGB32);
    auto result = view.copy(); context->Unmap(staging.Get(), 0); return result;
}
bool controllerFlow(HWND fixture, const QString &enginePath)
{
    // 使用测试自己创建的窗口，验证真正的 WGC → 控制器 → 检测 → 独立预览。
    MainWindow view(ClientRole::Publisher); LoginModel login; HttpClient http; LoginController auth(&view, &login, &http);
    SessionModel model; SignalClient signal; SessionController controller(&view, &login, &model, &signal);
    CHECK(until([&] { return view.findChild<QPushButton *>("refreshDevicesButton")->isEnabled(); }));
    login.completeLogin("test", "root", "root", 60);
    auto *enabled = view.findChild<QCheckBox *>("faceDetectionCheck");
    auto *path = view.findChild<QLineEdit *>("faceEngineEdit");
    auto *status = view.findChild<QLabel *>("faceStatusLabel");
    auto *stop = view.findChild<QPushButton *>("stopCaptureButton");
    auto *preview = view.findChild<VideoRenderer *>();
    QSignalSpy displayed(preview, &VideoRenderer::framePresented);
    const QList<CaptureSource> source{{CaptureSource::Kind::Window, "face-fixture", "GPU test fixture", quintptr(fixture)}};
    enabled->setChecked(true); path->setText(QStringLiteral("missing-face-engine.engine"));
    view.captureRequested(source);
    CHECK(!enabled->isEnabled());
    CHECK(until([&] { return status->text().contains(QStringLiteral("已恢复原画面")) && !displayed.isEmpty(); }));
    view.stopCaptureRequested(); CHECK(until([&] { return !stop->isEnabled(); }));
    CHECK(enabled->isEnabled()); displayed.clear();
    path->setText(enginePath); view.captureRequested(source);
    CHECK(until([&] { return status->text().contains(QStringLiteral("张人脸")) && !displayed.isEmpty(); }, 15000));
    qInfo().noquote() << "Controller:" << status->text();
    login.logout(); CHECK(until([&] { return !stop->isEnabled(); }));
    const auto *worker = controller.findChild<GpuFaceDetector *>();
    CHECK(worker && !worker->latestResult().video.texture);
    CHECK(!view.findChild<QPushButton *>("startCaptureButton")->isEnabled());
    qInfo("PASS controller / WGC / missing-engine fallback / restart / login exit");
    return true;
}
#ifdef OBS_FACE_REFERENCE_TESTS
QVector<FaceDetection> reference(EngineSession &session, const QImage &image)
{
    // 独立 OpenCV CPU resize + blob 对照 CUDA 颜色/布局/采样，客户端本身不依赖 OpenCV。
    const auto bgra = image.convertToFormat(QImage::Format_ARGB32);
    cv::Mat pixels(bgra.height(), bgra.width(), CV_8UC4, const_cast<uchar *>(bgra.constBits()), size_t(bgra.bytesPerLine()));
    cv::Mat bgr, resized, padded = cv::Mat::zeros(640, 640, CV_8UC3);
    cv::cvtColor(pixels, bgr, cv::COLOR_BGRA2BGR);
    const auto size = scrfdResize(image.size());
    cv::resize(bgr, resized, cv::Size(size.width(), size.height()));
    resized.copyTo(padded(cv::Rect(0, 0, resized.cols, resized.rows)));
    const auto blob = cv::dnn::blobFromImage(padded, 1.0 / 128, cv::Size(640, 640), cv::Scalar(127.5, 127.5, 127.5), true, false, CV_32F);
    const std::vector<float> input(blob.ptr<float>(), blob.ptr<float>() + blob.total());
    return decodeScrfd(session.run(input), image.size());
}
bool agrees(const QVector<FaceDetection> &expected, const QVector<FaceDetection> &actual)
{
    CHECK(expected.size() == actual.size() && !actual.isEmpty());
    for (qsizetype i = 0; i < actual.size(); ++i) {
        const auto overlap = expected[i].box.intersected(actual[i].box);
        const double area = overlap.width() * overlap.height();
        const double total = expected[i].box.width() * expected[i].box.height() + actual[i].box.width() * actual[i].box.height() - area;
        CHECK(total > 0 && area / total >= 0.97);
        for (int j = 0; j < 5; ++j) CHECK(QLineF(expected[i].landmarks[j], actual[i].landmarks[j]).length() < 4);
    }
    return true;
}
#endif
bool hardware(const QString &enginePath, const QString &imagePath, const QString &outputPath)
{
    QImage image(imagePath); CHECK(!image.isNull());
    QFile planFile(enginePath); CHECK(planFile.open(QIODevice::ReadOnly)); const auto plan = planFile.readAll();
    auto frame = imageTexture(image, 1);
    Microsoft::WRL::ComPtr<ID3D11Device> device; frame.texture->GetDevice(&device);
#ifdef OBS_FACE_REFERENCE_TESTS
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    winrt::check_hresult(device.As(&dxgi)); winrt::check_hresult(dxgi->GetAdapter(&adapter));
    int ordinal = -1; CHECK(cudaD3D11GetDevice(&ordinal, adapter.Get()) == cudaSuccess && cudaSetDevice(ordinal) == cudaSuccess);
    TrtLogger logger; EngineSession baseline(std::vector<char>(plan.begin(), plan.end()), logger);
#endif
    GpuFaceDetector detector; QSignalSpy errors(&detector, &GpuFaceDetector::failed);
    VideoRenderer renderer; renderer.resize(960, 540); renderer.show();
    QSignalSpy presented(&renderer, &VideoRenderer::framePresented);
    QSignalSpy renderErrors(&renderer, &VideoRenderer::failed);
    CHECK(detector.begin(enginePath));
    for (quint64 sequence = 1; sequence <= 5; ++sequence) {
        frame.sequence = sequence; detector.submit(frame);
        CHECK(until([&] { return detector.latestResult().video.sequence == sequence || !errors.isEmpty(); }, 15000));
        if (!errors.isEmpty()) qCritical().noquote() << errors[0][0].toString();
        CHECK(errors.isEmpty()); const auto result = detector.latestResult();
        CHECK(result.faces.size() > 0 && result.registrations == 1 && result.gpuMs > 0);
#ifdef OBS_FACE_REFERENCE_TESTS
        if (sequence == 1) CHECK(agrees(reference(baseline, image), result.faces));
#endif
        renderer.setFrame(result.video, result.faces);
        CHECK(until([&] { return !presented.isEmpty() && presented.last()[0].toULongLong() == sequence; }));
        qInfo("GPU frame %llu: faces=%lld gpu=%.3fms process=%lldms registrations=%llu", sequence,
            static_cast<long long>(result.faces.size()), result.gpuMs, result.processingMs, result.registrations);
    }
    CHECK(renderErrors.isEmpty());
    if (!outputPath.isEmpty()) {
        // WGC 只捕获本测试窗口，即使被其它窗口遮住也能核查 Shader 输出。
        VideoCapture observer; QSignalSpy observationErrors(&observer, &VideoCapture::failed);
        CHECK(observer.begin({CaptureSource::Kind::Window, "face-overlay", "Face overlay", quintptr(renderer.winId())}));
        QTimer redraw; redraw.setInterval(40);
        QObject::connect(&redraw, &QTimer::timeout, &renderer, [&] { const auto result = detector.latestResult(); renderer.setFrame(result.video, result.faces); }); redraw.start();
        const bool received = until([&] { return observer.latestFrame().texture != nullptr || !observationErrors.isEmpty(); });
        const auto observed = observer.latestFrame(); observer.stop(); CHECK(observer.wait(10000)); redraw.stop();
        CHECK(received && observationErrors.isEmpty() && observed.texture);
        const auto screenshot = readBack(observed.texture.Get());
        CHECK(!screenshot.isNull() && screenshot.save(outputPath));
        int green = 0, red = 0;
        for (int y = 0; y < screenshot.height(); ++y) for (int x = 0; x < screenshot.width(); ++x) {
            const auto pixel = screenshot.pixelColor(x, y);
            green += pixel.green() > 220 && pixel.red() < 60 && pixel.blue() < 110;
            red += pixel.red() > 220 && pixel.green() < 80 && pixel.blue() < 80;
        }
        qInfo("Overlay pixels: green=%d red=%d", green, red);
        CHECK(green > 100 && red > 20); // 验证 Shader 真正输出了框点，而不只是 CPU 检测成功。
    }
    // 宽幅和竖幅输入检查右/下补边及逆缩放；同设备换尺寸应各多一次注册。
    for (int i = 0; i < 2; ++i) {
        QImage shaped(i == 0 ? QSize(1024, 576) : QSize(576, 1024), QImage::Format_ARGB32); shaped.fill(Qt::black);
        for (int y = 0; y < image.height() && y < shaped.height(); ++y)
            for (int x = 0; x < image.width() && x < shaped.width(); ++x) shaped.setPixel(x, y, image.pixel(x, y));
        auto changed = imageTexture(shaped, quint64(6 + i), device.Get()); detector.submit(changed);
        CHECK(until([&] { return detector.latestResult().video.sequence == changed.sequence || !errors.isEmpty(); }));
        CHECK(errors.isEmpty()); const auto result = detector.latestResult();
        CHECK(result.registrations == quint64(2 + i) && !result.faces.isEmpty());
#ifdef OBS_FACE_REFERENCE_TESTS
        CHECK(agrees(reference(baseline, shaped), result.faces));
#endif
    }
    // 快速提交模拟慢消费者，停止时必须丢弃待处理输入及迟到结果。
    for (quint64 sequence = 8; sequence < 100; ++sequence) { frame.sequence = sequence; detector.submit(frame); }
    CHECK(until([&] { return detector.latestResult().replacedFrames > 0 || !errors.isEmpty(); }));
    CHECK(errors.isEmpty()); detector.stop(); CHECK(until([&] { return !detector.isRunning(); }));
    CHECK(!detector.latestResult().video.texture); renderer.clear();
    // 再运行一轮，确认注册数重新从1开始，GPU和交换链都能再次工作。
    CHECK(detector.begin(enginePath)); frame.sequence = 1; detector.submit(frame);
    CHECK(until([&] { return detector.latestResult().video.sequence == 1 || !errors.isEmpty(); }));
    CHECK(errors.isEmpty() && detector.latestResult().registrations == 1);
    detector.stop(); CHECK(until([&] { return !detector.isRunning(); }));
    // 为控制器测试保留持续更新的测试窗口，摄像头/麦克风/屏幕均不打开。
    renderer.setFrame(frame);
    QTimer redraw; redraw.setInterval(40);
    QObject::connect(&redraw, &QTimer::timeout, &renderer, [&] { renderer.setFrame(frame); }); redraw.start();
    CHECK(controllerFlow(reinterpret_cast<HWND>(renderer.winId()), enginePath));
    redraw.stop(); renderer.clear();
    qInfo("PASS GPU texture / TensorRT / overlay / texture reuse / dimensions / bounded mailbox / restart");
    return true;
}
}
int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toUtf8(); std::fprintf(stderr, "%s\n", bytes.constData()); std::fflush(stderr);
    });
    QApplication app(argc, argv);
#ifdef OBS_FACE_REFERENCE_TESTS
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);
#endif
    const auto args = app.arguments();
    const auto value = [&](const QString &key) { const auto index = args.indexOf(key); return index >= 0 && index + 1 < args.size() ? args[index + 1] : QString(); };
    try {
        if (!decoder() || !lifecycle()) return 1;
        if (args.contains(QStringLiteral("--gpu"))) return hardware(value("--engine"), value("--image"), value("--output")) ? 0 : 1;
    } catch (const std::exception &error) { qCritical("%s", error.what()); return 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); return 1; }
    return 0;
}
