#include "ffmpeg/MediaPublisher.h"
#include "ffmpeg/MediaDecoder.h"
#include "mainwindow.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QProcess>
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
TimedVideoFrame video(ID3D11Device *device, qint64 pts, bool noise = false, bool stress = false)
{
    // 人工纹理，无采集源；每帧不同的确定性噪声让CBR在不同目标下都有足够编码负载。
    const UINT width = stress ? 1280 : noise ? 640 : 64, height = stress ? 720 : noise ? 360 : 64;
    std::vector<quint32> pixels(size_t(width) * height); quint32 random = quint32(pts + 1) * 2654435761U;
    for (auto &pixel : pixels) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        // 限幅噪声仍有足够复杂度区分目标，避免全幅随机噪声在最高QP时也高于400kbps。
        pixel = stress ? 0xff000000U | (random & 0xffffffU) : noise ? 0xff606060U + (random & 0x3f3f3fU) : 0xff203040U;
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
    settings.liveQueueLimitMs = 2000; settings.liveQueueTargetMs = 1000; // 本用例专门保留原容量上限；默认时间策略另测。
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
bool queueExpiryChecks()
{
    // 不回答RTMP握手，使真实Publisher工作线程停在IO；WARP纹理从不进入NVENC。
    QTcpServer stalled; CHECK(stalled.listen(QHostAddress::LocalHost));
    QObject::connect(&stalled, &QTcpServer::newConnection, &stalled, [&] {
        while (stalled.hasPendingConnections()) {
            auto *socket = stalled.nextPendingConnection(); socket->setParent(&stalled);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->readAll(); });
        }
    });
    MediaPublisher publisher; RecordingSettings settings; settings.live = true; settings.width = settings.height = 64;
    settings.path = QString("rtmp://127.0.0.1:%1/live/expiry").arg(stalled.serverPort());
    const auto source = video(device(false).Get(), 0);
    auto push = [&](qint64 pts) { auto frame = source; frame.pts = pts; publisher.pushVideo(frame); };
    auto finish = [&] { publisher.stop(); return publisher.wait(2000); };
    settings.liveQueueTargetMs = settings.liveQueueLimitMs; CHECK(!publisher.begin(settings)); settings.liveQueueTargetMs = 100;
    CHECK(publisher.begin(settings)); push(0);
    for (int i = 1; i < 10; ++i) push(i);
    auto stats = publisher.stats();
    CHECK(stats.queuedVideo == 3 && stats.videoQueueSpanMs == 100 && stats.expiredVideo == 7 && stats.liveCatchups == 1);
    push(0); CHECK(publisher.stats().expiredVideo == 8 && publisher.stats().queuedVideo == 3); // 截止点前的迟到帧不再入队。
    CHECK(finish());

    // 音频超过300ms后保留100ms；同一个截止点裁掉旧视频，避免画面停留在旧声音区间。
    CHECK(publisher.begin(settings)); push(0);
    CHECK(publisher.stats().liveCatchups == 0 && publisher.stats().expiredVideo == 0);
    for (int i = 1; i < 6; ++i) push(i);
    for (int i = 0; i < 31; ++i) publisher.pushAudio(audio(i * 480));
    stats = publisher.stats();
    CHECK(stats.queuedVideo == 0 && stats.expiredVideo == 6 && stats.queuedAudio == 10 && stats.expiredAudio == 21);
    CHECK(stats.audioQueueSpanMs == 100 && stats.liveCatchups == 1);
    publisher.pushAudio(audio(0)); CHECK(publisher.stats().expiredAudio == 22 && publisher.stats().queuedAudio == 10);
    QTest::qWait(340); push(30); // 新视频到达时重新检查年龄，整批过期PCM清空而非强留最后一包。
    stats = publisher.stats(); CHECK(stats.queuedAudio == 0 && stats.expiredAudio == 32 && stats.queuedVideo == 1);
    CHECK(stats.oldestVideoWaitMs < 100 && stats.audioResyncs == 0); // 尚在握手，未谎称编码器已追赶。
    CHECK(finish());

    settings.video = false; CHECK(publisher.begin(settings));
    for (int i = 0; i < 31; ++i) publisher.pushAudio(audio(i * 480));
    CHECK(publisher.stats().queuedAudio == 10 && publisher.stats().expiredAudio == 21 && publisher.stats().expiredVideo == 0);
    CHECK(finish());
    qInfo("PASS live queue expiry: media span / high-low watermarks / shared cutoff / stale rejection / wall age / empty queue / audio-only / restart / invalid budget");
    return true;
}
bool audioCatchupChecks()
{
    struct Packets {
        std::vector<AVPacket *> values;
        ~Packets() { for (auto *packet : values) av_packet_free(&packet); }
    } packets;
    MediaEncoder encoder; RecordingSettings settings; settings.live = true; settings.video = false;
    encoder.open(settings, nullptr, true, [&](AVPacket *packet, const AVCodecContext *) {
        auto *copy = av_packet_clone(packet); if (!copy) throw std::bad_alloc(); packets.values.push_back(copy);
    });
    for (int i = 0; i < 5; ++i) encoder.pushAudio(audio(i * 480));
    encoder.pushAudio(audio(4800)); // 普通50ms缺口仍补2400样本静音。
    for (int i = 0; i < 8; ++i) encoder.pushAudio(audio(240000 + i * 480), i == 0); // 主动跳到5秒，不补这段静音。
    const auto stats = encoder.stats();
    CHECK(stats.audioResyncs == 1 && stats.skippedAudioSamples == 234880 && stats.silentSamples == 2400);
    encoder.finish(); CHECK(packets.values.size() < 20);
    struct Decoder { AVCodecContext *value = nullptr; ~Decoder() { avcodec_free_context(&value); } } decoder;
    const auto *codec = avcodec_find_decoder(AV_CODEC_ID_AAC); CHECK(codec);
    decoder.value = avcodec_alloc_context3(codec); CHECK(decoder.value);
    auto *parameters = avcodec_parameters_alloc(); CHECK(parameters);
    const auto copied = avcodec_parameters_from_context(parameters, encoder.audioContext());
    const auto configured = avcodec_parameters_to_context(decoder.value, parameters); avcodec_parameters_free(&parameters);
    CHECK(copied >= 0 && configured >= 0 && avcodec_open2(decoder.value, codec, nullptr) >= 0);
    auto *frame = av_frame_alloc(); CHECK(frame); qint64 lastPts = AV_NOPTS_VALUE, largestGap = 0; int samples = 0; bool valid = true;
    auto receive = [&] {
        int result;
        while ((result = avcodec_receive_frame(decoder.value, frame)) >= 0) {
            if (lastPts != AV_NOPTS_VALUE) { valid = valid && frame->pts > lastPts; largestGap = std::max(largestGap, frame->pts - lastPts); }
            lastPts = frame->pts; samples += frame->nb_samples; av_frame_unref(frame);
        }
        valid = valid && (result == AVERROR(EAGAIN) || result == AVERROR_EOF);
    };
    qint64 dts = AV_NOPTS_VALUE;
    for (auto *packet : packets.values) {
        valid = valid && (dts == AV_NOPTS_VALUE || packet->dts > dts); dts = packet->dts;
        valid = valid && avcodec_send_packet(decoder.value, packet) >= 0; receive();
    }
    valid = valid && avcodec_send_packet(decoder.value, nullptr) >= 0; receive(); av_frame_free(&frame);
    CHECK(valid && samples < 12000 && samples >= 8000 && largestGap > 200000);
    qInfo("PASS AAC catchup: ordinary gap=2400 silence / intentional skip=%llu samples including FIFO / packets=%zu decoded=%d / monotonic DTS and decoded PTS gap=%lld",
        stats.skippedAudioSamples, packets.values.size(), samples, largestGap);
    return true;
}
bool uiChecks()
{
    MainWindow view(ClientRole::Publisher); view.applyLoginState("root", false, true); view.resize(640, 480); view.show();
    auto *spin = view.findChild<QSpinBox *>("liveVideoBitrateSpin");
    auto *button = view.findChild<QPushButton *>("applyLiveBitrateButton");
    CHECK(spin && button && view.liveVideoBitrate() == 4000000); QSignalSpy requests(&view, &MainWindow::liveVideoBitrateRequested);
    view.applyLiveOutputState(false, true, QStringLiteral("正在推流\n视频120帧，音频4秒\n请求600kbps，编码器目标600kbps\n视频压缩码率590kbps，本机发送650kbps\n队列视频3/音频10，跨度100/100ms，队头等待20/30ms\n视频处理3ms，写出最近2ms/最长986ms/正在等待0ms\n过期丢弃视频51/音频168，队列追赶8次，音频跳过4次/1.685秒"));
    spin->setValue(750); CHECK(requests.isEmpty()); button->click();
    CHECK(requests.size() == 1 && requests.first().first().toInt() == 750000 && view.liveVideoBitrate() == 750000);
    auto *scroll = view.findChild<QScrollArea *>("workspaceScroll"); CHECK(scroll); QTest::qWait(60);
    scroll->ensureWidgetVisible(button); QTest::qWait(40);
    CHECK(scroll->viewport()->rect().contains(QRect(spin->mapTo(scroll->viewport(), QPoint()), spin->size())));
    CHECK(button->width() >= button->minimumSizeHint().width());
    QDir().mkpath("out/推流控制验证"); const auto suffix = qEnvironmentVariable("QT_SCALE_FACTOR") == "2" ? QStringLiteral("二倍缩放") : QString();
    CHECK(view.grab().save(QStringLiteral("out/推流控制验证/手动码率页面%1.png").arg(suffix)));
    auto *status = view.findChild<QLabel *>("liveOutputStatusLabel"); CHECK(status);
    scroll->ensureWidgetVisible(status); QTest::qWait(40);
    CHECK(scroll->viewport()->rect().contains(QRect(status->mapTo(scroll->viewport(), QPoint()), status->size())));
    QDir().mkpath("out/队列恢复验证"); CHECK(view.grab().save(QStringLiteral("out/队列恢复验证/直播追赶统计页面%1.png").arg(suffix)));
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
bool blockedWriteChecks()
{
    // 独立本机RTMP收流进程+可暂停读取的TCP代理，真实制造发送背压；不依赖业务服务/SRS或私有采集源。
    QTemporaryDir temp("out/publish-network-XXXXXX"); CHECK(temp.isValid());
    QTcpServer reservation; CHECK(reservation.listen(QHostAddress::LocalHost));
    const auto port = reservation.serverPort(); reservation.close();
    const auto path = temp.filePath(QStringLiteral("受阻恢复.flv"));
    struct Receiver : QProcess {
        ~Receiver() { if (state() != NotRunning) { kill(); waitForFinished(2000); } }
    } receiver;
    receiver.setProcessChannelMode(QProcess::MergedChannels);
    receiver.start(QString::fromUtf8(OBS_TEST_FFMPEG), {"-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
        "-analyzeduration", "200000", "-probesize", "1000000", "-i", QString("rtmp://127.0.0.1:%1/live/queue").arg(port),
        "-map", "0", "-c", "copy", "-f", "flv", path});
    CHECK(receiver.waitForStarted(2000)); QTest::qWait(400);
    QTcpServer relay; CHECK(relay.listen(QHostAddress::LocalHost));
    QTcpSocket upstream; QTcpSocket *downstream = nullptr; bool paused = false;
    auto forward = [&] {
        if (!downstream && relay.hasPendingConnections()) {
            downstream = relay.nextPendingConnection(); upstream.connectToHost(QHostAddress::LocalHost, port);
        }
        if (!downstream) return;
        if (!paused && upstream.state() == QAbstractSocket::ConnectedState && upstream.bytesToWrite() < 65536) {
            const auto count = std::min<qint64>(65536 - upstream.bytesToWrite(), downstream->bytesAvailable());
            if (count > 0) upstream.write(downstream->read(count));
        }
        if (upstream.bytesAvailable()) downstream->write(upstream.readAll());
    };
    MediaPublisher publisher; RecordingSettings settings; settings.live = true;
    settings.path = QString("rtmp://127.0.0.1:%1/live/queue").arg(relay.serverPort());
    settings.width = 1280; settings.height = 720; settings.videoBitrate = 50000000;
    auto d3d = device(true);
    std::array<TimedVideoFrame, 12> stressFrames;
    for (int i = 0; i < int(stressFrames.size()); ++i) stressFrames[i] = video(d3d.Get(), i, false, true);
    const auto recoveredFrame = video(d3d.Get(), 0, true);
    QSignalSpy errors(&publisher, &MediaPublisher::failed), opened(&publisher, &MediaPublisher::opened);
    QElapsedTimer timer; timer.start(); paused = true;
    CHECK(publisher.begin(settings)); publisher.pushVideo(stressFrames[0]); publisher.pushAudio(audio(0));
    CHECK(until([&] { forward(); return downstream != nullptr; })); QTest::qWait(340);
    paused = false;
    CHECK(until([&] { forward(); return !opened.isEmpty() || !errors.isEmpty(); }));
    CHECK(errors.isEmpty());
    CHECK(until([&] { return publisher.stats().expiredAudio == 1; }));
    CHECK(publisher.stats().videoFrames == 0 && publisher.stats().audioSamples == 0);
    CHECK(publisher.stats().expiredVideo == 1 && publisher.stats().expiredAudio == 1); // 无新入队，出队检查仍剔除整批旧数据。
    int videos = int(timer.elapsed() * 30 / 1000) + 1, audios = int(timer.elapsed() / 10) + 1;
    qint64 pausedAt = -1, resumedAt = -1;
    bool blocked = false; quint64 beforeRecovery = 0;
    while (timer.elapsed() < 10000 && (resumedAt < 0 || timer.elapsed() - resumedAt < 1800)) {
        forward();
        const auto time = timer.elapsed();
        if (time * 30 >= videos * 1000) {
            // 预生成纹理，避免CPU造噪声本身挤占GUI时间，使注入主要作用于网络写出。
            auto frame = resumedAt >= 0 ? recoveredFrame : stressFrames[size_t(videos) % stressFrames.size()];
            frame.pts = videos; frame.source.video.sequence = quint64(++videos); publisher.pushVideo(frame);
        }
        while (audios * 10 <= time) publisher.pushAudio(audio(audios++ * 480));
        const auto stats = publisher.stats();
        if (pausedAt < 0 && time > 1200 && stats.videoFrames >= 10 && downstream) {
            paused = true; pausedAt = time;
            downstream->setReadBufferSize(4096); downstream->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        }
        if (paused) {
            if (stats.writing && stats.currentWriteUs > 350000) blocked = true;
            if ((blocked && time - pausedAt >= 900) || time - pausedAt >= 3000) {
                paused = false; resumedAt = time; beforeRecovery = stats.videoFrames;
                downstream->setReadBufferSize(0); downstream->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 262144);
                CHECK(publisher.requestVideoBitrate(5000000));
                qInfo("RTMP proxy resume: paused=%lldms blocked=%d queues=%llu/%llu expiry=%llu/%llu currentWrite=%.1fms",
                    resumedAt - pausedAt, blocked, stats.queuedVideo, stats.queuedAudio, stats.expiredVideo, stats.expiredAudio, stats.currentWriteUs / 1000.0);
            }
        }
        QTest::qWait(3);
        if (!errors.isEmpty()) {
            qCritical().noquote() << errors.first().first().toString();
            qCritical().noquote() << "RTMP test peer:" << receiver.readAll() << "upstream:" << upstream.errorString();
        }
        CHECK(errors.isEmpty());
    }
    const auto stats = publisher.stats();
    qInfo("RTMP recovery: blocked=%d maxWrite=%.1fms frames=%llu->%llu expiry=%llu/%llu resync=%llu skip=%llu silence=%llu wait=%lld/%lldms",
        blocked, stats.maxWriteUs / 1000.0, beforeRecovery, stats.videoFrames, stats.expiredVideo, stats.expiredAudio,
        stats.audioResyncs, stats.skippedAudioSamples, stats.silentSamples, stats.oldestVideoWaitMs, stats.oldestAudioWaitMs);
    CHECK(blocked && stats.maxWriteUs >= 350000 && stats.videoFrames > beforeRecovery + 10 && opened.size() == 1);
    CHECK(stats.expiredVideo > 0 && stats.expiredAudio > 0 && stats.audioResyncs > 0 && stats.skippedAudioSamples > 0 && stats.silentSamples == 0);
    CHECK(stats.oldestVideoWaitMs <= 300 && stats.oldestAudioWaitMs <= 300 && stats.encoderVideoBitrate == 5000000);
    QElapsedTimer stopping; stopping.start(); publisher.stop(); CHECK(publisher.wait(2000));
    const auto stopMs = stopping.elapsed();
    QElapsedTimer draining; draining.start();
    while (receiver.state() != QProcess::NotRunning && draining.elapsed() < 4000) {
        forward();
        if (downstream && downstream->state() == QAbstractSocket::UnconnectedState && !downstream->bytesAvailable() && !upstream.bytesToWrite())
            upstream.disconnectFromHost();
        QTest::qWait(3);
    }
    if (receiver.state() != QProcess::NotRunning || !QFileInfo::exists(path)) qCritical().noquote() << receiver.readAll();
    CHECK(receiver.state() == QProcess::NotRunning && QFileInfo(path).size() > 0);
    // 独立重读实际收到的FLV，检验保留时间缺口、两轨单调及恢复后的GPU解码，而非只看队列计数。
    MediaDecoder decoder; qint64 lastVideo = -1, lastAudio = -1, audioEnd = -1, largestAudioGap = 0;
    int decoded = 0; bool valid = true;
    decoder.open(path, [] { return false; }, [&](const PlaybackVideo &frame) {
        valid = valid && frame.ptsUs > lastVideo && frame.decodePath == VideoDecodePath::D3D11Gpu;
        lastVideo = frame.ptsUs; ++decoded;
    }, [&](const PlaybackAudio &packet) {
        valid = valid && packet.pts > lastAudio;
        if (audioEnd >= 0) largestAudioGap = std::max(largestAudioGap, packet.pts - audioEnd);
        lastAudio = packet.pts; audioEnd = packet.pts + packet.pcm.size() / 8;
    }, false, VideoDecodeMode::RequireHardware);
    while (decoder.readNext()) {} decoder.finish();
    const auto skew = std::abs(lastVideo - audioEnd * 1000000 / 48000);
    qInfo("RTMP received: GPU=%d lastVideo=%.3fs audioEnd=%.3fs largestAudioGap=%.1fms tailSkew=%.1fms stop=%lldms",
        decoded, lastVideo / 1000000.0, audioEnd / 48000.0, largestAudioGap / 48.0, skew / 1000.0, stopMs);
    CHECK(valid && decoded >= 20 && largestAudioGap > 9600 && skew < 200000);
    qInfo("PASS real blocked RTMP write / bounded fresh queues / shared audio recovery without silence refill / GPU decoding and timestamps / single session / stop");
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
    try { return telemetryChecks() && queueExpiryChecks() && audioCatchupChecks() && uiChecks()
        && (!app.arguments().contains("--gpu") || hardwareChecks())
        && (!app.arguments().contains("--weak-network") || blockedWriteChecks()) ? 0 : 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
    catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
