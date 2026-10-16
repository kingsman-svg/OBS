// SRS副项目集成验证：生成GPU画面/PCM，不打开摄像头、麦克风或屏幕采集。
#include "ffmpeg/MediaPublisher.h"
#include "ffmpeg/MediaPlayer.h"
#include "SessionController.h"
#include "PreviewWindow.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "LoginModel.h"
#include "HttpClient.h"
#include "mainwindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QLabel>
#include <QScrollArea>
#include <QLineEdit>
#include <QImage>
#include <QPixmap>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <windows.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace csn;
using Microsoft::WRL::ComPtr;
#define CHECK(value) do { if (!(value)) { qCritical("FAIL line %d: %s", __LINE__, #value); return false; } } while (0)
namespace {
bool until(const std::function<bool()> &condition, int timeout = 8000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) QTest::qWait(5);
    return condition();
}
bool request(SignalClient &signal, const QString &type, const QJsonObject &fields, QJsonObject &result)
{
    QSignalSpy replies(&signal, &SignalClient::response);
    const auto id = signal.request(type, fields); bool done = false, ok = false;
    CHECK(!id.isEmpty());
    CHECK(until([&] {
        for (const auto &reply : replies) if (reply[0].toString() == id) {
            done = true; ok = reply[2].toBool(); result = reply[3].toJsonObject();
        }
        return done;
    }));
    CHECK(ok); return true;
}
TimedAudioPacket audio(qint64 pts)
{
    TimedAudioPacket packet; packet.pts = pts;
    packet.source.sampleRate = 48000; packet.source.channels = 2; packet.source.channelMask = 3;
    packet.source.pcm.resize(480 * 8);
    for (int i = 0; i < 480; ++i) {
        const float value = float(.15 * std::sin(2 * 3.141592653589793 * 440 * (pts + i) / 48000));
        std::memcpy(packet.source.pcm.data() + i * 8, &value, 4);
        std::memcpy(packet.source.pcm.data() + i * 8 + 4, &value, 4);
    }
    return packet;
}
ComPtr<ID3D11Texture2D> generatedVideo()
{
    ComPtr<IDXGIFactory1> factory; winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> selected;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{}; winrt::check_hresult(adapter->GetDesc1(&desc));
        if (desc.VendorId == 0x10de) { selected = adapter; break; }
    }
    if (!selected) throw std::runtime_error("集成验证需要NVIDIA编码设备");
    ComPtr<ID3D11Device> device;
    winrt::check_hresult(D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 320; desc.Height = 180;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> pixels(320 * 180, 0xff2020ee); // 生成红色画面，与私有采集无关。
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 320 * 4, 0};
    ComPtr<ID3D11Texture2D> texture; winrt::check_hresult(device->CreateTexture2D(&desc, &data, &texture)); return texture;
}
bool integration()
{
    // 1. 独立部署的SRS API健康检查，再从OBS登录服务器拿开发会话，不打印令牌。
    HttpClient http; QSignalSpy success(&http, &HttpClient::succeeded);
    const auto health = http.getJson(QUrl("http://127.0.0.1:1985/api/v1/versions"));
    CHECK(until([&] { return !success.isEmpty(); }));
    CHECK(success.last()[0].toULongLong() == health && success.last()[2].toJsonObject().value("code").toInt(-1) == 0);
    success.clear(); http.postJson(QUrl("http://127.0.0.1:8081/auth/login"), {{"account", "root"}, {"password", "root"}});
    CHECK(until([&] { return !success.isEmpty(); }));
    const auto token = success.last()[2].toJsonObject().value("accessToken").toString(); CHECK(!token.isEmpty());
    SignalClient publisherSignal; publisherSignal.connectToServer("127.0.0.1", 9000, token);
    CHECK(until([&] { return publisherSignal.isReady(); }));
    QJsonObject room, response;
    CHECK(request(publisherSignal, "room.create", {{"title", QStringLiteral("生成画面直播验证")}}, room));
    CHECK(!room.value("streaming").toBool() && room.value("pushUrl").toString().startsWith("rtmp://"));
    // 2. Player实际MVC先加入等待中的房间；输出音量为零，仍验证WASAPI主时钟。
    MainWindow view(ClientRole::Player); LoginModel login; SessionModel session; SignalClient viewerSignal;
    SessionController controller(&view, &login, &session, &viewerSignal);
    view.applyLoginState("root", false, true); login.completeLogin(token, "root", "root", 3600);
    view.findChild<QSpinBox *>("playbackVolumeSpin")->setValue(0);
    CHECK(until([&] { return session.ready() && !session.busy(); }));
    view.joinRoomRequested(room.value("roomId").toString());
    CHECK(until([&] { return !session.room().isEmpty() && !session.busy(); }));
    auto *player = controller.findChild<MediaPlayer *>(); CHECK(player && !player->isRunning());
    int displayed = 0;
    QObject::connect(&controller, &SessionController::playbackFrameReady, &view, [&](const VideoFrame &) { ++displayed; });
    auto texture = generatedVideo(); MediaPublisher publisher;
    RecordingSettings settings; settings.live = true; settings.path = room.value("pushUrl").toString();
    settings.width = 320; settings.height = 180; settings.videoBitrate = 1000000;
    // 3. 同一个输出线程和房间连续两轮启停；每轮PTS从零开始，首次媒体写出后才live.start。
    for (int round = 0; round < 2; ++round) {
        QSignalSpy transmitted(&publisher, &MediaPublisher::transmitting), errors(&publisher, &MediaPublisher::failed);
        QSignalSpy playErrors(player, &MediaPlayer::failed);
        CHECK(publisher.begin(settings));
        QElapsedTimer timer; timer.start(); int video = 0, samples = 0; bool announced = false, lowered = false, raised = false;
        const int before = displayed;
        while (timer.elapsed() < 4500) {
            if (timer.elapsed() * 30 >= video * 1000) {
                FaceFrame frame; frame.video.texture = texture; frame.video.sequence = quint64(video + 1);
                publisher.pushVideo({frame, video++});
            }
            while (samples * 10 <= timer.elapsed()) publisher.pushAudio(audio(samples++ * 480));
            if (!lowered && timer.elapsed() >= 1500) { CHECK(publisher.requestVideoBitrate(500000)); lowered = true; }
            if (!raised && timer.elapsed() >= 3000) { CHECK(publisher.requestVideoBitrate(1200000)); raised = true; }
            QTest::qWait(2);
            if (!announced && !transmitted.isEmpty()) {
                CHECK(request(publisherSignal, "live.start", {}, response)); announced = true;
            }
            if (!errors.isEmpty()) qCritical().noquote() << errors.first().first().toString();
            if (!playErrors.isEmpty()) qCritical().noquote() << playErrors.first().first().toString();
            CHECK(errors.isEmpty() && playErrors.isEmpty());
        }
        const auto liveStats = player->stats();
        qInfo("live startup: announced=%d transmitting=%lld running=%d decoded=%llu GPU=%llu displayed=%d dropped=%llu samples=%llu clock=%.3fs",
            announced, transmitted.size(), player->isRunning(), liveStats.decodedVideo, liveStats.gpuVideo, displayed - before,
            liveStats.droppedVideo, liveStats.audioSamples, liveStats.positionUs / 1000000.0);
        CHECK(announced && transmitted.size() == 1 && displayed - before >= 40);
        const auto stats = player->stats(); CHECK(stats.audioSamples > 48000 && stats.positionUs > 1000000);
        CHECK(stats.gpuVideo == stats.decodedVideo && stats.gpuVideo > 0 && stats.readbackVideo == 0 && stats.softwareVideo == 0);
        CHECK(view.findChild<PreviewWindow *>()->isVisible());
        const auto sending = publisher.stats();
        CHECK(sending.requestedVideoBitrate == 1200000 && sending.encoderVideoBitrate == 1200000);
        CHECK(sending.writeCalls > 0 && sending.writeFailures == 0 && sending.outputBytes > 0 && sending.outputBitrate > 0);
        CHECK(sending.totalWriteUs >= sending.maxWriteUs && sending.totalVideoEncodeUs > 0 && sending.peakQueuedVideo <= 16 && sending.peakQueuedAudio <= 50);
        qInfo("PASS RTMP control: target=%d localIO=%llu kbps maxWrite=%.2fms peakQueue=%llu/%llu", sending.encoderVideoBitrate,
            sending.outputBitrate / 1000, sending.maxWriteUs / 1000.0, sending.peakQueuedVideo, sending.peakQueuedAudio);
        qInfo("PASS live round %d: decoded=%llu GPU=%llu displayed=%d audioSamples=%llu clock=%.3fs underruns=%llu",
            round + 1, stats.decodedVideo, stats.gpuVideo, displayed - before, stats.audioSamples, stats.positionUs / 1000000.0, stats.audioUnderruns);
        if (round == 0) {
            view.resize(640, 480); view.show(); QTest::qWait(30);
            QDir().mkpath("out/直播接入"); CHECK(view.grab().save(QStringLiteral("out/直播接入/播放端直播布局.png")));
        }
        QElapsedTimer stopping; stopping.start(); publisher.stop(); CHECK(publisher.wait(2000));
        CHECK(request(publisherSignal, "live.stop", {}, response));
        CHECK(until([&] { return !player->isRunning() && !view.findChild<QPushButton *>("stopLivePlaybackButton")->isEnabled(); }, 2500));
        CHECK(!session.room().value("streaming").toBool());
        qInfo("PASS stop: %lldms / player released / room waiting", stopping.elapsed());
    }
    // 4. 房间关闭清理；一个只接受TCP却不回答RTMP握手的端点验证IO取消。
    CHECK(request(publisherSignal, "room.leave", {}, response));
    CHECK(until([&] { return session.room().isEmpty(); }));
    QTcpServer stalled; CHECK(stalled.listen(QHostAddress::LocalHost));
    QObject::connect(&stalled, &QTcpServer::newConnection, &stalled, [&] {
        auto *socket = stalled.nextPendingConnection(); socket->setParent(&stalled);
    });
    settings.video = false; settings.path = QString("rtmp://127.0.0.1:%1/live/test").arg(stalled.serverPort());
    CHECK(publisher.begin(settings)); publisher.pushAudio(audio(0)); QTest::qWait(120);
    QElapsedTimer cancelled; cancelled.start(); publisher.stop(); CHECK(publisher.wait(2000));
    qInfo("PASS stalled RTMP handshake cancelled: %lldms", cancelled.elapsed());
    MediaPlayer stalledPlayer;
    CHECK(stalledPlayer.begin(settings.path, false, true)); QTest::qWait(120);
    cancelled.restart(); stalledPlayer.stop(); CHECK(stalledPlayer.wait(2000));
    qInfo("PASS stalled RTMP player cancelled: %lldms", cancelled.elapsed());
    // 5. 实际Publisher MVC只捕获本测试创建的窗口；验证begin/采集/同步/RTMP全流程。
    QLabel target; target.setWindowTitle(QStringLiteral("直播验证生成窗口"));
    const bool beauty = qApp->arguments().contains("--beauty");
    const auto argument = [](const QString &key) { const int i = qApp->arguments().indexOf(key);
        return i >= 0 && i + 1 < qApp->arguments().size() ? qApp->arguments()[i + 1] : QString(); };
    if (beauty) {
        const QImage image(argument("--image")); CHECK(!image.isNull());
        target.setPixmap(QPixmap::fromImage(image)); target.setScaledContents(true); target.resize(image.size());
    } else { target.setStyleSheet("background:#df2020;"); target.resize(320, 180); }
    target.show();
    CHECK(QTest::qWaitForWindowExposed(&target));
    MainWindow publisherView(ClientRole::Publisher); LoginModel publisherLogin; SessionModel publisherSession; SignalClient guiSignal;
    SessionController publisherController(&publisherView, &publisherLogin, &publisherSession, &guiSignal);
    publisherView.applyLoginState("root", false, true); publisherLogin.completeLogin(token, "root", "root", 3600);
    CHECK(until([&] { return publisherSession.ready() && publisherView.findChild<QPushButton *>("startCaptureButton")->isEnabled(); }));
    publisherView.setCaptureSources({{CaptureSource::Kind::Window, "generated", QStringLiteral("窗口 · 直播验证生成窗口"), quintptr(target.winId())}});
    publisherView.findChild<QComboBox *>("videoSourceCombo")->setCurrentIndex(1);
    publisherView.findChild<QSpinBox *>("liveVideoBitrateSpin")->setValue(800);
    int beautified = 0;
    if (beauty) {
        publisherView.findChild<QCheckBox *>("beautyCheck")->setChecked(true);
        publisherView.findChild<QLineEdit *>("faceEngineEdit")->setText(argument("--engine"));
        QObject::connect(&publisherController, &SessionController::synchronizedVideoReady, &publisherView, [&](const TimedVideoFrame &frame) {
            if (frame.source.beautified && !frame.source.drawFaces) ++beautified;
        });
    }
    publisherView.createRoomRequested(QStringLiteral("完整采集直播验证"));
    CHECK(until([&] { return publisherView.findChild<QPushButton *>("startLiveButton")->isEnabled(); }));
    QTest::mouseClick(publisherView.findChild<QPushButton *>("startLiveButton"), Qt::LeftButton);
    CHECK(until([&] { return publisherSession.room().value("streaming").toBool() && !publisherSession.busy(); }));
    const int prior = displayed;
    view.joinRoomRequested(publisherSession.room().value("roomId").toString());
    CHECK(until([&] { return displayed >= prior + 30; }));
    CHECK(player->stats().positionUs > 500000);
    if (beauty) CHECK(beautified >= 20 && player->stats().gpuVideo > 20);
    MediaPublisher *liveOutput = nullptr;
    for (auto *output : publisherController.findChildren<MediaPublisher *>()) if (output->isRunning()) liveOutput = output;
    CHECK(liveOutput && liveOutput->stats().requestedVideoBitrate == 800000);
    publisherView.findChild<QSpinBox *>("liveVideoBitrateSpin")->setValue(600);
    publisherView.findChild<QPushButton *>("applyLiveBitrateButton")->click();
    CHECK(until([&] { return liveOutput->stats().encoderVideoBitrate == 600000; }));
    CHECK(until([&] { return publisherView.findChild<QLabel *>("liveOutputStatusLabel")->text().contains(QStringLiteral("编码器目标 600 kbps")); }));
    publisherView.resize(720, 800); publisherView.show(); QTest::qWait(40);
    auto *scroll = publisherView.findChild<QScrollArea *>("workspaceScroll"); CHECK(scroll);
    scroll->ensureWidgetVisible(publisherView.findChild<QPushButton *>("applyLiveBitrateButton")); QTest::qWait(30);
    QDir().mkpath("out/推流控制验证"); CHECK(publisherView.grab().save(QStringLiteral("out/推流控制验证/真实直播统计页面.png")));
    // 直接关闭直播中的房间，验证停止媒体/leave串行和观众清理，不只验证单独stop按钮。
    QTest::mouseClick(publisherView.findChild<QPushButton *>("leaveRoomButton"), Qt::LeftButton);
    CHECK(until([&] { return session.room().isEmpty() && publisherSession.room().isEmpty() && !player->isRunning()
        && publisherView.findChild<QPushButton *>("startCaptureButton")->isEnabled(); }));
    qInfo("PASS Publisher MVC: generated window WGC -> timeline -> NVENC -> SRS -> late viewer / close-room cleanup");
    if (beauty) qInfo("PASS local beauty -> existing RTMP -> GPU Player: beautified=%d", beautified);
    return true;
}
}
int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData()); std::fflush(stderr);
    });
    QApplication app(argc, argv);
    const int font = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).constFirst(), 10));
    try { return integration() ? 0 : 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
    catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
