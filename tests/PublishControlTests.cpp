#include "ffmpeg/MediaPublisher.h"
#include "mainwindow.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QImage>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <winrt/base.h>
#include <array>
#include <cstdio>
#include <vector>
#include <stdexcept>

using namespace csn;
using Microsoft::WRL::ComPtr;
#define CHECK(value) do { if (!(value)) { qCritical("FAIL line %d: %s", __LINE__, #value); return false; } } while (0)
namespace {
bool until(const std::function<bool()> &condition, int timeout = 10000)
{
    QElapsedTimer timer; timer.start(); while (!condition() && timer.elapsed() < timeout) QTest::qWait(5); return condition();
}
ComPtr<ID3D11Device> device(bool gpu)
{
    ComPtr<ID3D11Device> result;
    winrt::check_hresult(D3D11CreateDevice(nullptr, gpu ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &result, nullptr, nullptr)); return result;
}
TimedVideoFrame video(ID3D11Device *device, qint64 pts, bool noise = false)
{
    // 人工纹理，无采集源；每帧不同的确定性噪声让CBR在不同目标下都有足够编码负载。
    const UINT width = noise ? 640 : 64, height = noise ? 360 : 64;
    std::vector<quint32> pixels(size_t(width) * height); quint32 random = quint32(pts + 1) * 2654435761U;
    for (auto &pixel : pixels) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        // 限幅噪声仍有足够复杂度区分目标，避免全幅随机噪声在最高QP时也高于400kbps。
        pixel = noise ? 0xff606060U + (random & 0x3f3f3fU) : 0xff203040U;
    }
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0}; TimedVideoFrame frame; frame.pts = pts;
    frame.source.video.sequence = quint64(pts + 1);
    winrt::check_hresult(device->CreateTexture2D(&desc, &data, &frame.source.video.texture)); return frame;
}
TimedAudioPacket audio(qint64 pts)
{
    TimedAudioPacket packet; packet.pts = pts; packet.source.sampleRate = 48000; packet.source.channels = 2;
    packet.source.channelMask = 3; packet.source.pcm = QByteArray(480 * 8, '\0'); return packet;
}
bool telemetryChecks()
{
    MediaPublisher publisher; CHECK(!publisher.requestVideoBitrate(500000));
    QTcpServer stalled; CHECK(stalled.listen(QHostAddress::LocalHost));
    QObject::connect(&stalled, &QTcpServer::newConnection, &stalled, [&] {
        while (stalled.hasPendingConnections()) {
            auto *socket = stalled.nextPendingConnection(); socket->setParent(&stalled);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->readAll(); });
        }
    });
    RecordingSettings settings; settings.live = true; settings.width = settings.height = 64;
    settings.path = QString("rtmp://127.0.0.1:%1/live/test").arg(stalled.serverPort());
    auto d3d = device(false); const auto source = video(d3d.Get(), 0);
    for (int round = 0; round < 2; ++round) {
        CHECK(publisher.begin(settings)); CHECK(publisher.stats().writeCalls == 0 && publisher.stats().peakQueuedVideo == 0);
        CHECK(!publisher.requestVideoBitrate(99999) && !publisher.requestVideoBitrate(50000001));
        for (int i = 0; i < 30; ++i) { auto frame = source; frame.pts = i; publisher.pushVideo(frame); }
        for (int i = 0; i < 60; ++i) publisher.pushAudio(audio(i * 480));
        CHECK(publisher.requestVideoBitrate(900000) && publisher.requestVideoBitrate(700000));
        QTest::qWait(40); const auto stats = publisher.stats();
        CHECK(stats.requestedVideoBitrate == 700000 && stats.encoderVideoBitrate == 0);
        CHECK(stats.queuedVideo == 16 && stats.queuedAudio == 50 && stats.peakQueuedVideo == 16 && stats.peakQueuedAudio == 50);
        CHECK(stats.videoQueueSpanMs == 533 && stats.audioQueueSpanMs == 500 && stats.oldestVideoWaitMs >= 20 && stats.oldestAudioWaitMs >= 20);
        CHECK(stats.droppedVideo == 14 && stats.droppedAudio == 10);
        QElapsedTimer responsiveness; responsiveness.start();
        for (int i = 0; i < 100; ++i) { publisher.stats(); CHECK(publisher.requestVideoBitrate(600000 + i * 1000)); }
        CHECK(responsiveness.elapsed() < 100);
        publisher.stop(); CHECK(!publisher.requestVideoBitrate(500000) && publisher.wait(2000));
        CHECK(publisher.stats().queuedVideo == 0 && publisher.stats().queuedAudio == 0 && !publisher.stats().writing);
    }
    QTemporaryDir temp("out/publish-audio-XXXXXX"); CHECK(temp.isValid()); settings.live = false; settings.video = false;
    settings.path = temp.filePath(QStringLiteral("统计音频.mp4")); CHECK(publisher.begin(settings)); CHECK(!publisher.requestVideoBitrate(500000));
    for (int i = 0; i < 30; ++i) publisher.pushAudio(audio(i * 480)); publisher.stop(); CHECK(publisher.wait(3000));
    const auto finished = publisher.stats(); CHECK(finished.audioSamples == 14400 && finished.writeCalls > 0 && finished.writeFailures == 0);
    CHECK(finished.totalWriteUs >= finished.maxWriteUs && finished.outputBytes == 0 && finished.outputBitrate == 0);
    qInfo("PASS publisher statistics: queue count/span/age/peak / loss accounting / blocked handshake GUI responsiveness / latest request / stop rejection / reset / audio-only / file writes");
    return true;
}
bool uiChecks()
{
    MainWindow view(ClientRole::Publisher); view.applyLoginState("root", false, true); view.resize(640, 480); view.show();
    auto *spin = view.findChild<QSpinBox *>("liveVideoBitrateSpin");
    auto *button = view.findChild<QPushButton *>("applyLiveBitrateButton");
    CHECK(spin && button && view.liveVideoBitrate() == 4000000); QSignalSpy requests(&view, &MainWindow::liveVideoBitrateRequested);
    view.applyLiveOutputState(false, true, QStringLiteral("推流统计验证\n请求/编码目标/实际输出\n视频队列16帧，音频50包\n最长写出与当前等待"));
    spin->setValue(750); CHECK(requests.isEmpty()); button->click();
    CHECK(requests.size() == 1 && requests.first().first().toInt() == 750000 && view.liveVideoBitrate() == 750000);
    auto *scroll = view.findChild<QScrollArea *>("workspaceScroll"); CHECK(scroll); QTest::qWait(60);
    scroll->ensureWidgetVisible(button); QTest::qWait(40);
    CHECK(scroll->viewport()->rect().contains(QRect(spin->mapTo(scroll->viewport(), QPoint()), spin->size())));
    CHECK(button->width() >= button->minimumSizeHint().width());
    QDir().mkpath("out/推流控制验证"); const auto suffix = qEnvironmentVariable("QT_SCALE_FACTOR") == "2" ? QStringLiteral("二倍缩放") : QString();
    CHECK(view.grab().save(QStringLiteral("out/推流控制验证/手动码率页面%1.png").arg(suffix)));
    view.applyLiveOutputState(false, false, "stopped"); CHECK(!button->isEnabled());
    qInfo("PASS manual bitrate UI: kbps->bit/s / explicit apply / no per-digit request / compact layout / stop lock"); return true;
}
struct Input {
    AVFormatContext *format = nullptr; ~Input() { avformat_close_input(&format); }
};
bool hardwareChecks()
{
    QTemporaryDir temp("out/publish-gpu-XXXXXX"); CHECK(temp.isValid());
    auto d3d = device(true); MediaPublisher publisher; RecordingSettings settings;
    settings.path = temp.filePath(QStringLiteral("动态码率.mp4")); settings.width = 640; settings.height = 360; settings.audio = false; settings.videoBitrate = 3000000;
    QSignalSpy errors(&publisher, &MediaPublisher::failed), opened(&publisher, &MediaPublisher::opened);
    CHECK(publisher.begin(settings)); std::array<quint64, 3> windows{};
    const std::array<int, 3> targets{3000000, 400000, 2000000};
    for (int i = 0; i < 270; ++i) {
        if (i == 90 || i == 180) {
            CHECK(publisher.requestVideoBitrate(1000000)); CHECK(publisher.requestVideoBitrate(targets[i / 90]));
            CHECK(publisher.stats().requestedVideoBitrate == targets[i / 90]);
        }
        publisher.pushVideo(video(d3d.Get(), i, true));
        CHECK(until([&] { return publisher.stats().videoFrames == quint64(i + 1) || !errors.isEmpty(); }));
        if (!errors.isEmpty()) qWarning().noquote() << errors.first().first().toString(); CHECK(errors.isEmpty());
        if (i % 90 == 89) {
            const auto stats = publisher.stats(); windows[i / 90] = stats.videoPayloadBitrate;
            CHECK(stats.encoderVideoBitrate == targets[i / 90] && stats.videoRateWindowMs == 1000);
        }
    }
    publisher.stop(); CHECK(!publisher.requestVideoBitrate(500000)); CHECK(publisher.wait(5000));
    QCoreApplication::processEvents(); CHECK(errors.isEmpty() && opened.size() == 1);
    const auto final = publisher.stats(); CHECK(final.videoFrames == 270 && final.droppedVideo == 0 && final.queuedVideo == 0 && final.writeCalls > 0);
    qInfo("NVENC measured windows: %llu / %llu / %llu kbps", windows[0] / 1000, windows[1] / 1000, windows[2] / 1000);
    CHECK(windows[0] > windows[1] * 3 && windows[2] > windows[1] * 2);
    // 文件中独立核对各段末秒的真实压缩包字节、PTS/DTS及改码率后的关键帧，不只检查上下文字段。
    Input input; CHECK(avformat_open_input(&input.format, settings.path.toUtf8().constData(), nullptr, nullptr) >= 0);
    CHECK(avformat_find_stream_info(input.format, nullptr) >= 0); const int index = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0); CHECK(index >= 0);
    const auto *stream = input.format->streams[index]; auto *packet = av_packet_alloc(); CHECK(packet);
    std::array<quint64, 3> bytes{}; std::array<bool, 2> key{}; qint64 lastDts = AV_NOPTS_VALUE; int packets = 0; bool valid = true;
    while (av_read_frame(input.format, packet) >= 0) {
        if (packet->stream_index == index) {
            const auto pts = av_rescale_q(packet->pts, stream->time_base, {1, 30});
            valid = valid && (lastDts == AV_NOPTS_VALUE || packet->dts > lastDts); lastDts = packet->dts;
            valid = valid && pts == packets; ++packets;
            for (int phase = 0; phase < 3; ++phase) if (pts >= phase * 90 + 60 && pts < phase * 90 + 90) bytes[phase] += packet->size;
            if (pts == 90 || pts == 180) key[(pts - 90) / 90] = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet); CHECK(valid && packets == 270 && key[0] && key[1]);
    CHECK(bytes[0] * 8 == windows[0] && bytes[1] * 8 == windows[1] && bytes[2] * 8 == windows[2]);
    qInfo("PASS real NVENC bitrate change: target=3000->400->2000 kbps, encoded=%llu->%llu->%llu kbps / one session / 270 continuous frames / IDR / file recheck / stats separation",
        windows[0] / 1000, windows[1] / 1000, windows[2] / 1000);
    return true;
}
}
int main(int argc, char **argv)
{
    SetErrorMode(0x8003); qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData()); std::fflush(stderr);
    });
    QApplication app(argc, argv); QDir().mkpath("out"); const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).constFirst(), 10));
    try { return telemetryChecks() && uiChecks() && (!app.arguments().contains("--gpu") || hardwareChecks()) ? 0 : 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
    catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
