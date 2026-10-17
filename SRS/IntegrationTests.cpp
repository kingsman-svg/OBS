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
#include <QPainter>
#include <QFile>
#include <QJsonDocument>
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
#include <algorithm>
#include <vector>

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
        MediaPlayer silentPlayer; bool silentStarted = false; qint64 silentClock = 0; int silentFrames = 0;
        bool silentBuffered = false, silentFrozen = false, silentRecovered = false; qint64 frozenClock = -1;
        QSignalSpy transmitted(&publisher, &MediaPublisher::transmitting), errors(&publisher, &MediaPublisher::failed);
        QSignalSpy playErrors(player, &MediaPlayer::failed);
        CHECK(publisher.begin(settings));
        QElapsedTimer timer; timer.start(); int video = 0, samples = 0; bool announced = false, lowered = false, raised = false;
        qint64 previousFeedMs = 0, maxFeedGapMs = 0;
        const int before = displayed;
        while (timer.elapsed() < 4500) {
            maxFeedGapMs = std::max(maxFeedGapMs, timer.elapsed() - previousFeedMs); previousFeedMs = timer.elapsed();
            // 合成视频源短停200ms，音轨继续；核对无声Player独立视频结束点与冻结/恢复，不注入网络故障。
            if (round == 0 && timer.elapsed() >= 1900 && timer.elapsed() < 2100) video = int(timer.elapsed() * 30 / 1000);
            else if (timer.elapsed() * 30 >= video * 1000) {
                FaceFrame frame; frame.video.texture = texture; frame.video.sequence = quint64(video + 1);
                publisher.pushVideo({frame, video++});
            }
            while (samples * 10 <= timer.elapsed()) publisher.pushAudio(audio(samples++ * 480));
            if (!lowered && timer.elapsed() >= 1500) { CHECK(publisher.requestVideoBitrate(500000)); lowered = true; }
            if (!raised && timer.elapsed() >= 3000) { CHECK(publisher.requestVideoBitrate(1200000)); raised = true; }
            QTest::qWait(2);
            if (!announced && !transmitted.isEmpty()) {
                CHECK(request(publisherSignal, "live.start", {}, response)); announced = true;
                if (round == 0) {
                    CHECK(silentPlayer.begin(room.value("pullUrl").toString(), false, true, VideoDecodeMode::RequireHardware));
                    silentStarted = true;
                }
            }
            if (silentStarted) {
                if (silentPlayer.takeVideo().source.texture) ++silentFrames;
                const auto clock = silentPlayer.clockUs(); CHECK(clock >= silentClock); silentClock = clock;
                const auto state = silentPlayer.stats();
                if (state.buffering && state.rebuffers > 0) {
                    silentBuffered = true; if (frozenClock >= 0 && clock == frozenClock) silentFrozen = true; frozenClock = clock;
                } else if (silentBuffered && clock > frozenClock) silentRecovered = true;
            }
            if (!errors.isEmpty()) qCritical().noquote() << errors.first().first().toString();
            if (!playErrors.isEmpty()) qCritical().noquote() << playErrors.first().first().toString();
            CHECK(errors.isEmpty() && playErrors.isEmpty());
        }
        const auto liveStats = player->stats();
        qInfo("live startup: announced=%d transmitting=%lld running=%d decoded=%llu GPU=%llu displayed=%d dropped=%llu samples=%llu clock=%.3fs",
            announced, transmitted.size(), player->isRunning(), liveStats.decodedVideo, liveStats.gpuVideo, displayed - before,
            liveStats.droppedVideo, liveStats.audioSamples, liveStats.positionUs / 1000000.0);
        qInfo("live source: frames=%d audioPackets=%d maxFeedGap=%lldms open=%.1fms", video, samples, maxFeedGapMs, liveStats.openUs / 1000.0);
        CHECK(announced && transmitted.size() == 1 && displayed - before >= 40);
        const auto stats = player->stats(); CHECK(stats.audioSamples > 48000 && stats.positionUs > 1000000);
        CHECK(stats.adaptiveBuffer && stats.bufferTargetUs >= 50000 && stats.bufferTargetUs <= 150000);
        CHECK(stats.audioQueuedUs <= PlaybackBuffer::CapacityUs && stats.audioPaddingUs <= 30000);
        if (silentStarted) {
            const auto silent = silentPlayer.stats();
            CHECK(silent.adaptiveBuffer && silentFrames > 60 && silent.gpuVideo > 60 && silent.bufferTargetUs >= 50000);
            CHECK(silentBuffered && silentFrozen && silentRecovered && silent.rebuffers >= 1 && silent.bufferIncreases >= 1);
            silentPlayer.stop(); CHECK(silentPlayer.wait(2000));
            qInfo("PASS adaptive video-only clock: GPU=%llu delivered=%d target=%.0fms rebuffers=%llu", silent.gpuVideo, silentFrames, silent.bufferTargetUs / 1000.0, silent.rebuffers);
        }
        CHECK(stats.renderedAudioSamples * 100 >= stats.audioSamples * 75); // 时钟推进不能掩盖持续补零。
        CHECK(stats.gpuVideo == stats.decodedVideo && stats.gpuVideo > 0 && stats.readbackVideo == 0 && stats.softwareVideo == 0);
        CHECK(view.findChild<PreviewWindow *>()->isVisible());
        const auto sending = publisher.stats();
        CHECK(sending.requestedVideoBitrate == 1200000 && sending.encoderVideoBitrate == 1200000);
        CHECK(sending.encoderVbvMs == 200 && sending.encoderGopFrames == 30 && sending.muxInterleaveMs == 40);
        CHECK(sending.connectUs > 0 && sending.readyUs >= sending.connectUs && sending.firstMediaUs >= sending.readyUs);
        CHECK(sending.writeCalls > 0 && sending.writeFailures == 0 && sending.outputBytes > 0 && sending.outputBitrate > 0);
        CHECK(sending.totalWriteUs >= sending.maxWriteUs && sending.totalVideoEncodeUs > 0 && sending.peakQueuedVideo <= 16 && sending.peakQueuedAudio <= 50);
        qInfo("PASS RTMP control: target=%d localIO=%llu kbps maxWrite=%.2fms peakQueue=%llu/%llu", sending.encoderVideoBitrate,
            sending.outputBitrate / 1000, sending.maxWriteUs / 1000.0, sending.peakQueuedVideo, sending.peakQueuedAudio);
        qInfo("PASS live round %d: decoded=%llu GPU=%llu displayed=%d audioSamples=%llu clock=%.3fs underruns=%llu",
            round + 1, stats.decodedVideo, stats.gpuVideo, displayed - before, stats.audioSamples, stats.positionUs / 1000000.0, stats.audioUnderruns);
        qInfo("adaptive playback: target=%.0fms increases=%llu decreases=%llu rebuffers=%llu wait=%.1fms catchups=%llu PCM=%.1fms",
            stats.bufferTargetUs / 1000.0, stats.bufferIncreases, stats.bufferDecreases, stats.rebuffers, stats.rebufferUs / 1000.0, stats.audioCatchups, stats.audioQueuedUs / 1000.0);
        if (round == 0) {
            view.resize(640, 480); view.show(); QTest::qWait(30);
            QDir().mkpath("out/直播接入"); CHECK(view.grab().save(QStringLiteral("out/直播接入/播放端直播布局.png")));
            auto *status = view.findChild<QLabel *>("livePlaybackStatusLabel");
            auto *scroll = view.findChild<QScrollArea *>("workspaceScroll"); CHECK(status && scroll);
            CHECK(status->text().contains(QStringLiteral("自适应缓冲")) && status->height() >= status->heightForWidth(status->width()));
            scroll->ensureWidgetVisible(status); QTest::qWait(30);
            CHECK(view.grab().save(QStringLiteral("out/直播接入/播放端自适应缓冲统计.png")));
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
    auto *automatic = publisherView.findChild<QCheckBox *>("liveAutomaticBitrateCheck");
    auto *minimum = publisherView.findChild<QSpinBox *>("liveMinimumBitrateSpin"); CHECK(automatic && minimum);
    automatic->setChecked(true); minimum->setValue(200); CHECK(!liveOutput->stats().automaticBitrate); // 编辑不立即提交。
    publisherView.findChild<QPushButton *>("applyLiveBitrateButton")->click();
    CHECK(until([&] { return liveOutput->stats().automaticBitrate && !liveOutput->stats().bitrateControlPending; }));
    automatic->setChecked(false); publisherView.findChild<QSpinBox *>("liveVideoBitrateSpin")->setValue(500);
    publisherView.findChild<QPushButton *>("applyLiveBitrateButton")->click();
    CHECK(until([&] { return !liveOutput->stats().automaticBitrate && liveOutput->stats().encoderVideoBitrate == 500000; }));
    automatic->setChecked(true); publisherView.findChild<QSpinBox *>("liveVideoBitrateSpin")->setValue(600);
    publisherView.findChild<QPushButton *>("applyLiveBitrateButton")->click();
    CHECK(until([&] { return liveOutput->stats().automaticBitrate && !liveOutput->stats().bitrateControlPending; }));
    CHECK(liveOutput->stats().targetVideoBitrate == 500000 && liveOutput->stats().requestedVideoBitrate == 600000); // 自动模式保留当前值。
    qInfo("PASS Publisher ABR UI: explicit apply / automatic-manual switch / user ceiling / no jump to ceiling / one output session");
    publisherView.resize(720, 800); publisherView.show(); QTest::qWait(40);
    auto *scroll = publisherView.findChild<QScrollArea *>("workspaceScroll"); CHECK(scroll);
    scroll->ensureWidgetVisible(publisherView.findChild<QPushButton *>("applyLiveBitrateButton")); QTest::qWait(30);
    QDir().mkpath("out/推流控制验证"); CHECK(publisherView.grab().save(QStringLiteral("out/推流控制验证/真实直播统计页面.png")));
    scroll->ensureWidgetVisible(publisherView.findChild<QLabel *>("liveOutputStatusLabel")); QTest::qWait(30);
    CHECK(publisherView.grab().save(QStringLiteral("out/推流控制验证/真实直播延迟统计.png")));
    // 直接关闭直播中的房间，验证停止媒体/leave串行和观众清理，不只验证单独stop按钮。
    QTest::mouseClick(publisherView.findChild<QPushButton *>("leaveRoomButton"), Qt::LeftButton);
    CHECK(until([&] { return session.room().isEmpty() && publisherSession.room().isEmpty() && !player->isRunning()
        && publisherView.findChild<QPushButton *>("startCaptureButton")->isEnabled(); }));
    qInfo("PASS Publisher MVC: generated window WGC -> timeline -> NVENC -> SRS -> late viewer / close-room cleanup");
    if (beauty) qInfo("PASS local beauty -> existing RTMP -> GPU Player: beautified=%d", beautified);
    return true;
}

// 编号直接画进像素，经真实编码/RTMP/解码后再读回；不以发送端PTS或旁路消息冒充到达时间。
ComPtr<ID3D11Texture2D> markedVideo(ID3D11Device *device, quint16 sequence)
{
    QImage pixels(320, 180, QImage::Format_ARGB32); pixels.fill(QColor(195, 35, 35));
    const quint32 marker = 0xa5000000U | (quint32((sequence & 255) ^ (sequence >> 8) ^ 0x5a) << 16) | sequence;
    QPainter painter(&pixels);
    for (int bit = 0; bit < 32; ++bit) painter.fillRect(16 + bit * 8, 12, 8, 24, marker & (1U << bit) ? Qt::white : Qt::black);
    painter.setPen(Qt::white); painter.setFont(QFont(qApp->font().family(), 18));
    painter.drawText(pixels.rect(), Qt::AlignCenter, QStringLiteral("生成帧 %1").arg(sequence)); painter.end();
    if (sequence == 30) { QDir().mkpath("out/低延迟验证"); pixels.save(QStringLiteral("out/低延迟验证/推流编号画面.png")); }
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 320; desc.Height = 180;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.constBits(), UINT(pixels.bytesPerLine()), 0};
    ComPtr<ID3D11Texture2D> texture; winrt::check_hresult(device->CreateTexture2D(&desc, &data, &texture)); return texture;
}
quint16 readMarker(const VideoFrame &frame, ComPtr<ID3D11Texture2D> &staging)
{
    ComPtr<ID3D11Device> device; frame.texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    if (!staging) {
        D3D11_TEXTURE2D_DESC desc{}; frame.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &staging));
    }
    context->CopyResource(staging.Get(), frame.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    quint32 marker = 0;
    for (int bit = 0; bit < 32; ++bit) {
        const auto *pixel = static_cast<const uchar *>(mapped.pData) + 24 * mapped.RowPitch + (20 + bit * 8) * 4;
        if (int(pixel[0]) + pixel[1] + pixel[2] > 384) marker |= 1U << bit;
    }
    context->Unmap(staging.Get(), 0);
    const auto sequence = quint16(marker);
    const auto checksum = quint8((sequence & 255) ^ (sequence >> 8) ^ 0x5a);
    return (marker >> 24) == 0xa5 && quint8(marker >> 16) == checksum ? sequence : 0;
}
bool latencyRound(bool lowLatency, QJsonObject &result)
{
    // 1. 同一个SRS配置，独立生成流；音量为0但真实运行WASAPI，保持音频主时钟。
    auto base = generatedVideo(); ComPtr<ID3D11Device> device; base->GetDevice(&device);
    MediaPublisher publisher; MediaPlayer player; player.setVolume(0);
    RecordingSettings settings; settings.live = true; settings.lowLatency = lowLatency;
    settings.width = 320; settings.height = 180; settings.videoBitrate = 1000000;
    settings.path = QString("rtmp://127.0.0.1:1935/live/latency_%1_%2").arg(QCoreApplication::applicationPid()).arg(lowLatency);
    QSignalSpy outputErrors(&publisher, &MediaPublisher::failed), playErrors(&player, &MediaPlayer::failed);
    QSignalSpy transmitting(&publisher, &MediaPublisher::transmitting);
    QElapsedTimer timer; timer.start(); CHECK(publisher.begin(settings));
    int videos = 0, audios = 0, invalid = 0; bool watching = false;
    std::vector<qint64> sent(1, 0); std::vector<double> delays; ComPtr<ID3D11Texture2D> staging;
    qint64 previousPts = -1, previousClock = -1, peakAudio = 0, peakPadding = 0, peakVideo = 0;
    // 2. 发送与交付共用本测试进程的单调时钟；纹理生成不计入发送端入队后的耗时。
    while (timer.elapsed() < 6500) {
        if (timer.elapsed() * 30 >= videos * 1000) {
            const auto id = quint16(videos + 1); FaceFrame frame; frame.video.texture = markedVideo(device.Get(), id); frame.video.sequence = id;
            sent.push_back(timer.nsecsElapsed() / 1000); publisher.pushVideo({frame, videos++});
        }
        while (audios * 10 <= timer.elapsed()) publisher.pushAudio(audio(audios++ * 480));
        if (!watching && !transmitting.isEmpty()) {
            CHECK(player.begin(settings.path, true, true, VideoDecodeMode::RequireHardware, lowLatency, false)); watching = true; // 固定预算对照保持原测量语义。
        }
        const auto frame = player.takeVideo(); const auto delivered = timer.nsecsElapsed() / 1000;
        if (frame.source.texture) {
            CHECK(frame.ptsUs > previousPts); previousPts = frame.ptsUs;
            const auto id = readMarker(frame.source, staging);
            if (!id || id >= sent.size()) ++invalid;
            else if (sent[id] >= 2000000) delays.push_back((delivered - sent[id]) / 1000.0); // 预热后统计稳态分布。
        }
        const auto stats = player.stats(); CHECK(stats.positionUs >= previousClock); previousClock = stats.positionUs;
        peakAudio = std::max(peakAudio, stats.audioQueuedUs); peakPadding = std::max(peakPadding, stats.audioPaddingUs);
        peakVideo = std::max(peakVideo, stats.videoQueueSpanUs);
        CHECK(outputErrors.isEmpty() && playErrors.isEmpty()); QTest::qWait(3);
    }
    // 3. 验证运行时变码率仍保留当前VBV预算，不在新请求后退回旧配置。
    CHECK(publisher.requestVideoBitrate(800000));
    while (publisher.stats().encoderVideoBitrate != 800000 && timer.elapsed() < 7500) {
        FaceFrame frame; frame.video.texture = base; publisher.pushVideo({frame, videos++});
        publisher.pushAudio(audio(audios++ * 480)); player.takeVideo(); QTest::qWait(10);
    }
    const auto sending = publisher.stats(); const auto playing = player.stats();
    qInfo("latency diagnostics %s: invalid=%d samples=%zu GPU=%llu decoded=%llu delivered=%llu dropped=%llu clock=%.3fs first=%.1fms audioPeak=%.1fms videoPeak=%.1fms",
        lowLatency ? "low" : "baseline", invalid, delays.size(), playing.gpuVideo, playing.decodedVideo, playing.displayedVideo,
        playing.droppedVideo, playing.positionUs / 1000000.0, playing.firstDeliveryUs / 1000.0, peakAudio / 1000.0, peakVideo / 1000.0);
    CHECK(sending.encoderVideoBitrate == 800000 && sending.encoderVbvMs == (lowLatency ? 200 : 2000));
    CHECK(sending.encoderGopFrames == (lowLatency ? 30 : 60) && sending.muxInterleaveMs == (lowLatency ? 40 : 100));
    CHECK(watching && invalid == 0 && delays.size() >= 60 && playing.gpuVideo == playing.decodedVideo);
    CHECK(playing.displayedVideo * 100 >= playing.decodedVideo * 80); // 不能靠大量丢帧制造低延迟结果。
    CHECK(playing.renderedAudioSamples * 100 >= playing.audioSamples * 75);
    CHECK(playing.firstDeliveryUs >= playing.firstDecodedUs && playing.firstDecodedUs >= playing.openUs && playing.openUs > 0);
    CHECK(peakAudio <= (lowLatency ? 120000 : 200000));
    if (lowLatency) CHECK(peakVideo <= 340000 && peakPadding <= 30000);
    std::sort(delays.begin(), delays.end()); const auto p50 = delays[delays.size() / 2], p95 = delays[size_t(std::ceil((delays.size() - 1) * .95))];
    CHECK(delays.front() >= 0 && p95 < 3000);
    result = {{"lowLatency", lowLatency}, {"samples", int(delays.size())}, {"p50Ms", p50}, {"p95Ms", p95},
        {"minMs", delays.front()}, {"maxMs", delays.back()}, {"openMs", playing.openUs / 1000.0}, {"firstDeliveryMs", playing.firstDeliveryUs / 1000.0},
        {"connectMs", sending.connectUs / 1000.0}, {"peakAudioQueueMs", peakAudio / 1000.0}, {"peakDevicePaddingMs", peakPadding / 1000.0},
        {"deviceBufferMs", playing.audioBufferUs / 1000.0}, {"peakVideoSpanMs", peakVideo / 1000.0}, {"audioUnderruns", double(playing.audioUnderruns)},
        {"decodedVideo", double(playing.decodedVideo)}, {"deliveredVideo", double(playing.displayedVideo)}, {"droppedVideo", double(playing.droppedVideo)},
        {"droppedAudioSamples", double(playing.droppedAudioSamples)}, {"audioCatchups", double(playing.audioCatchups)},
        {"decodedAudioSamples", double(playing.audioSamples)}, {"renderedAudioSamples", double(playing.renderedAudioSamples)},
        {"insertedSilenceSamples", double(playing.insertedSilenceSamples)}};
    qInfo("RTMP latency %s: n=%zu p50=%.1fms p95=%.1fms first=%.1fms PCM/endpoint/video=%.1f/%.1f/%.1fms underruns=%llu",
        lowLatency ? "low" : "baseline", delays.size(), p50, p95, playing.firstDeliveryUs / 1000.0,
        peakAudio / 1000.0, peakPadding / 1000.0, peakVideo / 1000.0, playing.audioUnderruns);
    // 4. 先停观众避免把主动结束流当成读取失败；清理后才开始下一组预算对照。
    player.stop(); CHECK(player.wait(2000)); publisher.stop(); CHECK(publisher.wait(2000));
    return true;
}
bool latencyChecks()
{
    QJsonObject baseline, optimized; CHECK(latencyRound(false, baseline) && latencyRound(true, optimized));
    CHECK(optimized.value("p50Ms").toDouble() < baseline.value("p50Ms").toDouble());
    const QJsonObject report{{"measurement", "generated frame enqueue -> GUI takeVideo; same process monotonic clock; excludes capture and display scanout"},
        {"baseline", baseline}, {"optimized", optimized}};
    QDir().mkpath("out/低延迟验证"); QFile file(QStringLiteral("out/低延迟验证/RTMP延迟对照.json"));
    CHECK(file.open(QIODevice::WriteOnly)); const auto bytes = QJsonDocument(report).toJson(); CHECK(file.write(bytes) == bytes.size());
    qInfo("PASS RTMP marked-frame latency comparison / real NVENC and D3D11VA / audio master / fixed budgets after bitrate change / queue bounds / stop");
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
    try { return integration() && (!app.arguments().contains("--latency") || latencyChecks()) ? 0 : 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
    catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
