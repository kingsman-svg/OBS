#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "VideoCapture.h"
#include "WasapiCapture.h"
#include "LoginModel.h"
#include "LoginController.h"
#include "HttpClient.h"
#include "mainwindow.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
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
        view.resize(1000, 850); view.show(); QTest::qWait(200);
        const auto screenshot = qEnvironmentVariable("OBS_CAPTURE_SCREENSHOT");
        if (!screenshot.isEmpty()) CHECK(view.grab().save(screenshot));
        login.logout();
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
    if (hardware) return desktopHardware() && deviceHardware() ? 0 : 1;
    if (!audioFormats() || !failedWorkerRestart() || !captureMvc()) return 1;
    qInfo("PASS 3 capture groups: PCM formats / failed worker restart / MVC and logout");
    return 0;
}
