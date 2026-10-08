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
    view.applyLoginState(QStringLiteral("已登录：root"), false, true);
    auto *preview = view.findChild<PreviewWindow *>("previewWindow");
    auto *open = view.findChild<QPushButton *>("showPreviewButton");
    auto *scroll = view.findChild<QScrollArea *>("workspaceScroll");
    CHECK(preview && preview->isWindow() && preview->windowModality() == Qt::NonModal);
    CHECK(open && open->isEnabled() && scroll);
    CHECK(!view.findChild<QLabel *>("previewPlaceholder"));

    QImage image(320, 180, QImage::Format_RGB32);
    image.fill(QColor(32, 160, 96));
    view.showCapturePreview(image);
    CHECK(preview->isVisible() && view.isEnabled());
    preview->close();
    CHECK(!preview->isVisible());
    // 后续帧仍更新缓存，但不能把用户刚关掉的窗口重新弹出。
    image.fill(QColor(64, 128, 192));
    view.showCapturePreview(image);
    CHECK(!preview->isVisible());
    open->click();
    CHECK(preview->isVisible());
    preview->resize(320, 320);
    QTest::qWait(30);
    const auto rendered = preview->grab().toImage();
    CHECK(rendered.pixelColor(rendered.width() / 2, rendered.height() / 2) == QColor(64, 128, 192));
    CHECK(rendered.pixelColor(2, 2) == QColor("#101824")); // 保持比例，空余区域留黑边。

    const QString longName = QStringLiteral("窗口 · ") + QString(180, QChar(0x6d4b));
    view.setCaptureSources({{CaptureSource::Kind::Window, "layout", longName, 1}});
    view.findChild<QComboBox *>("videoSourceCombo")->setCurrentIndex(1);
    view.applyCaptureState(false, true, false, QStringLiteral("已发现采集设备"),
        {QStringLiteral("采集中：") + longName, QStringLiteral("未启用"), QStringLiteral("未启用")});
    const QSize compactSize = QSize(640, 480).boundedTo(initialSize);
    view.resize(compactSize);
    QTest::qWait(30);
    CHECK(view.size() == compactSize);
    CHECK(scroll->widget()->width() <= scroll->viewport()->width());
    CHECK(scroll->verticalScrollBar()->maximum() > 0); // 小窗口可滚动访问底部房间操作。

    // 只保存自建色块及人工构造的界面状态，不访问摄像头、麦克风或屏幕。
    const auto directory = qEnvironmentVariable("OBS_UI_SCREENSHOTS");
    if (!directory.isEmpty()) {
        CHECK(QDir().mkpath(directory));
        CHECK(view.grab().save(directory + QStringLiteral("/主窗口紧凑布局.png")));
        view.setCaptureSources({{CaptureSource::Kind::Window, "layout", QStringLiteral("窗口 · 采集验证色块"), 1}});
        view.applyCaptureState(false, true, false, QStringLiteral("已发现 3 个采集目标"),
            {QStringLiteral("视频采集中"), QStringLiteral("未启用"), QStringLiteral("未启用")});
        view.resize(initialSize); QTest::qWait(30);
        CHECK(view.grab().save(directory + QStringLiteral("/主窗口采集设置.png")));
        preview->resize(QSize(720, 480).boundedTo(preview->screen()->availableGeometry().size() - QSize(40, 80)));
        QTest::qWait(30);
        CHECK(preview->grab().save(directory + QStringLiteral("/独立采集预览.png")));
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
    view.showCapturePreview(image); // 新一轮采集首帧允许再次自动打开。
    CHECK(preview->isVisible());
    view.applyLoginState(QStringLiteral("已退出登录"), false, false);
    CHECK(!preview->isVisible() && !open->isEnabled());
    view.showCapturePreview(image); // 迟到的预览不能在退出后弹窗。
    CHECK(!preview->isVisible());
    view.applyLoginState(QStringLiteral("已登录：root"), false, true);
    view.showCapturePreview(image);
    CHECK(preview->isVisible());
    view.close();
    CHECK(!preview->isVisible());
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
    CHECK(until([&] { return !worker.latestFrame().preview.isNull() || !error.isEmpty(); }));
    if (!error.isEmpty()) qWarning().noquote() << error;
    auto first = worker.latestFrame();
    CHECK(error.isEmpty() && first.texture && first.timestamp100ns > 0);
    CHECK(first.previewTimestamp100ns > 0 && first.previewTimestamp100ns <= first.timestamp100ns);
    auto color = first.preview.pixelColor(first.preview.width() / 2, first.preview.height() / 2);
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
        QObject::connect(&controller, &SessionController::videoFrameReady, &view, [&delivered](const VideoFrame &) { ++delivered; });
        view.findChild<QPushButton *>("startCaptureButton")->click();
        CHECK(until([&] { return delivered > 3; }));
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
            CHECK(preview->grab().save(QFileInfo(screenshot).absolutePath() + QStringLiteral("/独立窗口真实采集.png")));
        }
        login.logout();
        CHECK(!preview->isVisible());
        CHECK(until([&] { return !view.findChild<QPushButton *>("stopCaptureButton")->isEnabled(); }));
        CHECK(!view.findChild<QPushButton *>("startCaptureButton")->isEnabled());
    }
    worker.stop(); CHECK(worker.wait(10000));
    // 停止后，消费者保留的快照仍有效；系统采集池不会复用它。
    CHECK(first.texture && first.preview.pixelColor(first.preview.width() / 2, first.preview.height() / 2) == color);
    CHECK(worker.begin(source));
    CHECK(until([&] { return worker.latestFrame().texture != nullptr; }));
    target.close();
    CHECK(until([&] { return !error.isEmpty() && !worker.isRunning(); }));
    qInfo("PASS WGC window / color / resize / restart / target close");
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
    if (app.arguments().contains("--window-preview")) return desktopHardware() ? 0 : 1;
    if (hardware) return desktopHardware() && deviceHardware() ? 0 : 1;
    if (!audioFormats() || !failedWorkerRestart() || !captureMvc() || !previewUiLifecycle()) return 1;
    qInfo("PASS 4 capture groups: PCM formats / failed worker restart / MVC and logout / independent preview");
    return 0;
}
