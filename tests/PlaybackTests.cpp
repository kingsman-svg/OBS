#include "ffmpeg/MediaDecoder.h"
#include "ffmpeg/MediaPlayer.h"
#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "LoginModel.h"
#include "mainwindow.h"
#include "PreviewWindow.h"
#include "VideoRenderer.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <windows.h>
#include <winrt/base.h>
#include <cmath>
#include <cstring>
#include <cstdio>

using namespace csn;
#define CHECK(condition) do { if (!(condition)) { qCritical("FAIL line %d: %s", __LINE__, #condition); return false; } } while (0)
namespace {
bool until(const std::function<bool()> &condition, int timeout = 8000)
{
    QElapsedTimer time; time.start();
    while (!condition() && time.elapsed() < timeout) QTest::qWait(5);
    return condition();
}
bool ffmpeg(const QStringList &arguments)
{
    QProcess process; process.start(QString::fromUtf8(OBS_TEST_FFMPEG), QStringList{"-hide_banner", "-loglevel", "error", "-y"} + arguments);
    if (!process.waitForFinished(20000) || process.exitStatus() != QProcess::NormalExit || process.exitCode()) {
        qCritical().noquote() << process.readAllStandardError(); return false;
    }
    return true;
}
bool fixtures(const QString &main, const QString &directory)
{
    // 全部由lavfi生成，不读用户文件、摄像头、麦克风或系统回环。
    CHECK(ffmpeg({"-f", "lavfi", "-i", "color=c=blue:s=160x90:r=25:d=1.04", "-f", "lavfi", "-i",
        "aevalsrc=0.15*sin(2*PI*440*t)|0.15*sin(2*PI*880*t):s=44100:d=1.04",
        "-c:v", "mpeg4", "-bf", "2", "-q:v", "2", "-c:a", "aac", "-movflags", "+faststart", main}));
    CHECK(ffmpeg({"-i", main, "-an", "-c:v", "copy", QDir(directory).filePath("video.mp4")}));
    CHECK(ffmpeg({"-i", main, "-vn", "-c:a", "copy", QDir(directory).filePath("audio.m4a")}));
    CHECK(ffmpeg({"-i", main, "-itsoffset", "0.8", "-i", main, "-map", "0:v", "-map", "1:a", "-c", "copy", QDir(directory).filePath("delayed.mp4")}));
    // 音频先到超过有界PCM队列容量，验证等首张视频时仍能解除背压。
    CHECK(ffmpeg({"-i", main, "-itsoffset", "1.2", "-i", main, "-map", "1:v", "-map", "0:a", "-c", "copy", QDir(directory).filePath("delayed-video.mp4")}));
    CHECK(ffmpeg({"-f", "lavfi", "-i", "color=c=blue:s=160x90:r=25:d=2.04", "-i", main,
        "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4", "-c:a", "copy", QDir(directory).filePath("long-video.mp4")}));
    return true;
}
bool decoderChecks(const QString &path)
{
    MediaDecoder decoder; int videos = 0; qint64 samples = 0, lastVideo = -1, lastAudio = -1;
    double left440 = 0, left880 = 0, right440 = 0, right880 = 0;
    bool valid = true; PlaybackVideo first;
    const auto info = decoder.open(path, [] { return false; }, [&](const PlaybackVideo &frame) {
        valid = valid && frame.ptsUs > lastVideo && frame.durationUs > 0 && frame.source.texture;
        lastVideo = frame.ptsUs; ++videos; if (!first.source.texture) first = frame;
    }, [&](const PlaybackAudio &packet) {
        valid = valid && packet.pts >= 0 && packet.pts > lastAudio && packet.pcm.size() % 8 == 0;
        lastAudio = packet.pts;
        for (qsizetype i = 0; i < packet.pcm.size() / 8; ++i) {
            float left, right; std::memcpy(&left, packet.pcm.constData() + i * 8, 4); std::memcpy(&right, packet.pcm.constData() + i * 8 + 4, 4);
            const auto sample = packet.pts + i; samples++;
            left440 += left * std::sin(2 * 3.141592653589793 * 440 * sample / 48000);
            left880 += left * std::sin(2 * 3.141592653589793 * 880 * sample / 48000);
            right440 += right * std::sin(2 * 3.141592653589793 * 440 * sample / 48000);
            right880 += right * std::sin(2 * 3.141592653589793 * 880 * sample / 48000);
        }
    });
    while (decoder.readNext()) {} decoder.finish(); decoder.finish();
    CHECK(valid && info.video && info.audio && info.width == 160 && info.height == 90 && videos == 26);
    CHECK(std::abs(samples - 49920) <= 48);
    CHECK(std::abs(left440) > std::abs(left880) * 5 && std::abs(right880) > std::abs(right440) * 5);
    Microsoft::WRL::ComPtr<ID3D11Device> device; first.source.texture->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; first.source.texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &staging));
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context); context->CopyResource(staging.Get(), first.source.texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    const auto *pixel = static_cast<const uchar *>(map.pData); const bool blue = pixel[0] > 180 && pixel[1] < 40 && pixel[2] < 40;
    context->Unmap(staging.Get(), 0); CHECK(blue);
    qInfo("PASS decoder: B-frame EOF drain / 26 frames / BGRA blue / 44.1kHz stereo -> 48kHz / AAC actual tail %lld", samples);
    return true;
}
class HttpFixture final : public QTcpServer {
public:
    QByteArray body; bool stall = false; int requests = 0;
    HttpFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto request = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) return;
                    socket->setProperty("sent", true); ++requests; if (stall) return;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1/test.mp4").arg(serverPort()); }
};
bool playWorker(const QString &path, bool audioOutput, qint64 expectedEndUs, bool requireVideo = true)
{
    MediaPlayer player; player.setVolume(0); // 硬件模式仍真正写入WASAPI，但测试输出无声。
    QSignalSpy errors(&player, &MediaPlayer::failed), completed(&player, &MediaPlayer::completed);
    CHECK(player.begin(path, audioOutput)); QElapsedTimer wall; wall.start();
    qint64 lastPts = -1, lastClock = -1; int frames = 0; bool valid = true;
    const bool ended = until([&] {
        const auto frame = player.takeVideo(); const qint64 clock = player.clockUs();
        valid = valid && clock >= lastClock; lastClock = clock;
        if (frame.source.texture) { valid = valid && frame.ptsUs > lastPts && frame.ptsUs <= clock + 3000; lastPts = frame.ptsUs; ++frames; }
        return !player.isRunning();
    }, 8000);
    player.stop(); CHECK(player.wait(5000)); QCoreApplication::processEvents();
    for (const auto &error : errors) qWarning().noquote() << error.first().toString();
    if (!ended || !valid || completed.size() != 1) qWarning("EOF details: ended=%d valid=%d complete=%lld frames=%d clock=%lld pos=%lld", ended, valid, completed.size(), frames, lastClock, player.stats().positionUs);
    CHECK(ended && valid && errors.isEmpty() && completed.size() == 1 && completed.first().first().toBool());
    CHECK(!requireVideo || frames > 5);
    CHECK(wall.elapsed() >= expectedEndUs / 1000 - 50 && wall.elapsed() < expectedEndUs / 1000 + 1000);
    CHECK(std::abs(player.stats().positionUs - expectedEndUs) < 150000);
    CHECK(!player.takeVideo().source.texture);
    qInfo("PASS player EOF: audio=%d frames=%d position=%.3f wall=%lldms dropped=%llu underruns=%llu", audioOutput, frames,
        player.stats().positionUs / 1000000.0, wall.elapsed(), player.stats().droppedVideo, player.stats().audioUnderruns);
    return true;
}
bool workerLifecycle(const QString &path)
{
    MediaPlayer player; QSignalSpy failed(&player, &MediaPlayer::failed), completed(&player, &MediaPlayer::completed);
    CHECK(player.begin(path, false)); CHECK(until([&] { return player.stats().decodedVideo >= 16; }));
    QTest::qWait(70); CHECK(player.stats().decodedVideo <= 16); // 不消费时明确背压，不无限解码/分配纹理。
    player.stop(); CHECK(player.wait(3000)); QCoreApplication::processEvents();
    CHECK(failed.isEmpty() && !player.takeVideo().source.texture && completed.size() == 1 && !completed.first().first().toBool());
    CHECK(player.begin("missing-playback-test.mp4", false)); CHECK(player.wait(3000)); QCoreApplication::processEvents(); CHECK(failed.size() == 1);
    qInfo("PASS worker: bounded video queue / stop wake / no late frame / invalid input / restart"); return true;
}
bool httpChecks(const QString &path)
{
    HttpFixture server; QFile file(path); CHECK(file.open(QIODevice::ReadOnly)); server.body = file.readAll(); CHECK(server.listen(QHostAddress::LocalHost));
    CHECK(playWorker(server.url(), false, 1040000)); CHECK(server.requests > 0);
    server.stall = true;
    MediaPlayer player; QSignalSpy errors(&player, &MediaPlayer::failed); const int previous = server.requests;
    CHECK(player.begin(server.url(), false)); CHECK(until([&] { return server.requests > previous; }));
    QElapsedTimer cancel; cancel.start(); player.stop(); CHECK(player.wait(3000)); QCoreApplication::processEvents();
    CHECK(errors.isEmpty() && cancel.elapsed() < 3000);
    qInfo("PASS HTTP MP4 / cancellation during unanswered network read: %lldms", cancel.elapsed()); return true;
}
bool mvcChecks(const QString &path)
{
    MainWindow view(ClientRole::Player); view.show();
    LoginModel login; SessionModel session; SignalClient signal; SessionController controller(&view, &login, &session, &signal);
    auto *mode = view.findChild<QComboBox *>("modeCombo"); mode->setCurrentIndex(1);
    auto *play = view.findChild<QPushButton *>("playMediaButton"), *stop = view.findChild<QPushButton *>("stopMediaButton");
    auto *edit = view.findChild<QLineEdit *>("localMediaEdit"); auto *audio = view.findChild<QCheckBox *>("playAudioCheck");
    auto *preview = view.findChild<PreviewWindow *>(); auto *renderer = preview->findChild<VideoRenderer *>();
    auto *status = view.findChild<QLabel *>("playbackStatusLabel");
    CHECK(mode && play && stop && edit && audio && !play->isEnabled());
    login.completeLogin("test", "root", "root", 60); view.applyLoginState("root", false, true);
    edit->setText(path); audio->setChecked(false); int shown = 0;
    QObject::connect(renderer, &VideoRenderer::framePresented, &view, [&](quint64) { ++shown; });
    CHECK(play->isEnabled()); play->click(); CHECK(stop->isEnabled() && !edit->isEnabled());
    CHECK(until([&] { return preview->isVisible() && shown > 2; })); preview->close(); QTest::qWait(80); CHECK(!preview->isVisible());
    view.findChild<QPushButton *>("showPreviewButton")->click(); CHECK(preview->isVisible());
    CHECK(until([&] { return !stop->isEnabled(); })); CHECK(status->text().contains(QStringLiteral("播放结束")) && play->isEnabled());
    play->click(); CHECK(until([&] { return shown > 25 || preview->isVisible(); })); stop->click();
    CHECK(until([&] { return !stop->isEnabled(); })); CHECK(!preview->isVisible());
    play->click(); CHECK(until([&] { return preview->isVisible(); })); mode->setCurrentIndex(0);
    CHECK(until([&] { return !stop->isEnabled(); })); CHECK(!preview->isVisible());
    mode->setCurrentIndex(1); play->click(); CHECK(until([&] { return preview->isVisible(); })); login.logout(); view.applyLoginState("logout", false, false);
    CHECK(until([&] { return !stop->isEnabled(); })); QTest::qWait(80); CHECK(!preview->isVisible() && !play->isEnabled());
    login.completeLogin("test", "root", "root", 60); view.applyLoginState("root", false, true);
    edit->setText("missing-playback-test.mp4"); play->click(); CHECK(until([&] { return play->isEnabled(); })); CHECK(status->text().contains(QStringLiteral("失败")));
    edit->setText(path); view.resize(640, 480);
    view.applyPlaybackState(false, 500000, 1040000, QStringLiteral("点播错误恢复：") + QString(120, QChar(0x6d4b)));
    QTest::qWait(50); CHECK(status->height() >= status->heightForWidth(status->width()));
    auto *progress = view.findChild<QProgressBar *>("playbackProgress");
    CHECK(progress && progress->height() >= progress->fontMetrics().lineSpacing() + 4);
    auto *scroll = view.findChild<QScrollArea *>("workspaceScroll"); CHECK(scroll->widget()->width() <= scroll->viewport()->width());
    const auto directory = qEnvironmentVariable("OBS_UI_SCREENSHOTS");
    if (!directory.isEmpty()) { QDir().mkpath(directory); scroll->ensureWidgetVisible(status); QTest::qWait(30); CHECK(view.grab().save(directory + QStringLiteral("/播放端点播紧凑布局.png"))); }
    qInfo("PASS Player MVC: EOF / repeat / hidden preview / stop / mode switch / logout / failure recovery / compact layout"); return true;
}
}
int main(int argc, char **argv)
{
    SetErrorMode(0x8003);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto text = message.toUtf8(); std::fprintf(stderr, "%s\n", text.constData()); std::fflush(stderr);
    });
    QApplication app(argc, argv);
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (fontId >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).constFirst(), 10));
    QDir().mkpath("out"); QTemporaryDir directory(QDir::current().filePath("out/player-test-XXXXXX"));
    if (!directory.isValid()) return 1;
    const QString media = directory.filePath(QStringLiteral("点播 中文.mp4"));
    try {
        const int sourceIndex = app.arguments().indexOf("--media-file");
        if (sourceIndex >= 0 && sourceIndex + 1 < app.arguments().size()) {
            const auto path = app.arguments()[sourceIndex + 1]; MediaDecoder probe;
            const auto info = probe.open(path, [] { return false; }, [](const PlaybackVideo &) {}, [](const PlaybackAudio &) {});
            return playWorker(path, app.arguments().contains("--audio-device"), info.durationUs, info.video) ? 0 : 1;
        }
        if (!fixtures(media, directory.path())) return 1;
        if (app.arguments().contains("--mvc-only")) return mvcChecks(media) ? 0 : 1;
        if (app.arguments().contains("--audio-device")) {
            if (!playWorker(media, true, 1040000) || !playWorker(directory.filePath("delayed.mp4"), true, 1840000)
                || !playWorker(directory.filePath("delayed-video.mp4"), true, 2240000)
                || !playWorker(directory.filePath("long-video.mp4"), true, 2040000)) return 1;
        } else {
            if (!decoderChecks(media) || !workerLifecycle(media) || !playWorker(media, false, 1040000)
                || !playWorker(directory.filePath("video.mp4"), false, 1040000)
                || !playWorker(directory.filePath("audio.m4a"), false, 1040000, false)
                || !httpChecks(media) || !mvcChecks(media)) return 1;
        }
        qInfo("PASS playback checks (only generated media; hardware output volume=0)"); return 0;
    } catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
      catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
