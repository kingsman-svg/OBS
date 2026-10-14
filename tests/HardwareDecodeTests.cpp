#include "ffmpeg/MediaDecoder.h"
#include "ffmpeg/MediaPlayer.h"
#include "ffmpeg/GpuVideoConverter.h"
#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "LoginModel.h"
#include "mainwindow.h"
#include "PreviewWindow.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <winrt/base.h>
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
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
bool ffmpeg(const QStringList &args)
{
    QProcess process; process.start(QString::fromUtf8(OBS_TEST_FFMPEG), QStringList{"-hide_banner", "-loglevel", "error", "-y"} + args);
    CHECK(process.waitForFinished(20000));
    if (process.exitCode()) qWarning().noquote() << process.readAllStandardError();
    CHECK(process.exitStatus() == QProcess::NormalExit && !process.exitCode()); return true;
}
QByteArray pixels(ID3D11Texture2D *texture)
{
    // 只在验证中读回像素；业务硬件路径没有Map/CPU像素下载。
    ComPtr<ID3D11Device> device; texture->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc); const UINT width = desc.Width, height = desc.Height;
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy; winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &copy));
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context); context->CopyResource(copy.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map{}; winrt::check_hresult(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &map));
    QByteArray result(qsizetype(width) * height * 4, '\0');
    for (UINT y = 0; y < height; ++y) std::memcpy(result.data() + qsizetype(y) * width * 4,
        static_cast<const char *>(map.pData) + y * map.RowPitch, width * 4);
    context->Unmap(copy.Get(), 0); return result;
}
struct Snapshot { QByteArray bytes; qint64 pts = 0; int width = 0, height = 0; VideoDecodePath path{}; };
bool compare(const QString &path)
{
    std::vector<Snapshot> reference, gpu; VideoFrame retained; QByteArray retainedPixels;
    const auto decode = [&](VideoDecodeMode mode, std::vector<Snapshot> &frames) {
        MediaDecoder decoder;
        decoder.open(path, [] { return false; }, [&](const PlaybackVideo &frame) {
            D3D11_TEXTURE2D_DESC desc{}; frame.source.texture->GetDesc(&desc);
            frames.push_back({pixels(frame.source.texture.Get()), frame.ptsUs, int(desc.Width), int(desc.Height), frame.decodePath});
            if (mode == VideoDecodeMode::RequireHardware && frames.size() == 1) { retained = frame.source; retainedPixels = frames.back().bytes; }
        }, [](const PlaybackAudio &) {}, false, mode);
        while (decoder.readNext()) {} decoder.finish(); decoder.finish();
    };
    decode(VideoDecodeMode::Software, reference); decode(VideoDecodeMode::RequireHardware, gpu);
    CHECK(reference.size() >= 20 && gpu.size() == reference.size());
    int worst = 0; qint64 difference = 0, channels = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
        const auto &a = reference[i], &b = gpu[i];
        CHECK(b.path == VideoDecodePath::D3D11Gpu && a.path == VideoDecodePath::SoftwareUpload);
        CHECK(a.pts == b.pts && a.width == b.width && a.height == b.height && a.bytes.size() == b.bytes.size());
        // 色块内部对照601/709、有限/全范围；边界处GPU和SWS的色度插值算法可不同。
        for (int y = 8; y < a.height - 8; y += 17) for (int x = 8; x < a.width - 8; x += 19) {
            bool flat = true;
            for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) for (int c = 0; c < 3; ++c)
                flat = flat && std::abs(int(uchar(a.bytes[((y + dy) * a.width + x + dx) * 4 + c])) - int(uchar(a.bytes[(y * a.width + x) * 4 + c]))) <= 2;
            if (!flat) continue; // 三个颜色通道都稳定，避免把单通道平坦的彩色边缘误当色块。
            for (int c = 0; c < 3; ++c) {
                const auto p = (y * a.width + x) * 4 + c;
                const int delta = std::abs(int(uchar(a.bytes[p])) - int(uchar(b.bytes[p])));
                worst = std::max(worst, delta); difference += delta; ++channels;
            }
        }
    }
    const double mean = double(difference) / std::max<qint64>(1, channels);
    qInfo("color comparison: frames=%zu max=%d mean=%.3f", gpu.size(), worst, mean);
    QDir().mkpath("out/GPU解码验证");
    QImage(reinterpret_cast<const uchar *>(reference[0].bytes.constData()), reference[0].width, reference[0].height,
        reference[0].width * 4, QImage::Format_ARGB32).save(QStringLiteral("out/GPU解码验证/软件解码参考.png"));
    QImage(reinterpret_cast<const uchar *>(gpu[0].bytes.constData()), gpu[0].width, gpu[0].height,
        gpu[0].width * 4, QImage::Format_ARGB32).save(QStringLiteral("out/GPU解码验证/硬件解码参考.png"));
    CHECK(channels > 500 && mean < 3.0 && worst < 16); // 多帧稳定色块验证矩阵/范围，边缘插值另做视觉检查。
    CHECK(pixels(retained.texture.Get()) == retainedPixels); // 经过整轮解码并销毁池后，第一张输出仍未改写。
    return true;
}
bool surfaceChecks()
{
    // 合成解码纹理验证数组层、对齐裁剪、8/10位转换和尺寸变化。
    ComPtr<ID3D11Device> device;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
    GpuVideoConverter converter(device.Get());
    for (const auto format : {DXGI_FORMAT_NV12, DXGI_FORMAT_P010}) {
        qInfo("BEGIN synthetic surface %s", format == DXGI_FORMAT_NV12 ? "NV12" : "P010");
        const int width = format == DXGI_FORMAT_NV12 ? 64 : 32, height = 32;
        const int stride = width * (format == DXGI_FORMAT_NV12 ? 1 : 2);
        QByteArray black(stride * height * 3 / 2, '\0'), white = black;
        for (int i = 0; i < width * height * 3 / 2; ++i) {
            const bool y = i < width * height;
            if (format == DXGI_FORMAT_NV12) { black[i] = char(y ? 16 : 128); white[i] = char(y ? 235 : 128); }
            else {
                const uint16_t a = uint16_t((y ? 64 : 512) << 6), b = uint16_t((y ? 940 : 512) << 6);
                std::memcpy(black.data() + i * 2, &a, 2); std::memcpy(white.data() + i * 2, &b, 2);
            }
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = 1;
        desc.ArraySize = 2; desc.SampleDesc.Count = 1; desc.Format = format; desc.BindFlags = D3D11_BIND_DECODER;
        const D3D11_SUBRESOURCE_DATA data[]{{black.constData(), UINT(stride), 0}, {white.constData(), UINT(stride), 0}};
        ComPtr<ID3D11Texture2D> source; winrt::check_hresult(device->CreateTexture2D(&desc, data, &source));
        AVFrame frame{}; frame.format = AV_PIX_FMT_D3D11; frame.data[0] = reinterpret_cast<uint8_t *>(source.Get());
        frame.width = width - 8; frame.height = height - 4; frame.colorspace = AVCOL_SPC_BT709; frame.color_range = AVCOL_RANGE_MPEG;
        const auto first = converter.convert(frame, [] { return false; }); const auto firstBytes = pixels(first.Get());
        frame.data[1] = reinterpret_cast<uint8_t *>(uintptr_t(1));
        const auto second = converter.convert(frame, [] { return false; }); const auto secondBytes = pixels(second.Get());
        D3D11_TEXTURE2D_DESC output{}; second->GetDesc(&output);
        CHECK(output.Width == UINT(frame.width) && output.Height == UINT(frame.height) && output.ArraySize == 1);
        CHECK(uchar(firstBytes[0]) < 3 && uchar(secondBytes[0]) > 252 && uchar(secondBytes[3]) == 255);
        CHECK(pixels(first.Get()) == firstBytes);
        bool cancelled = false, invalid = false;
        try { converter.convert(frame, [] { return true; }); } catch (const std::exception &) { cancelled = true; }
        frame.data[1] = reinterpret_cast<uint8_t *>(uintptr_t(2));
        try { converter.convert(frame, [] { return false; }); } catch (const std::exception &) { invalid = true; }
        CHECK(cancelled && invalid);
    }
    qInfo("PASS GPU conversion: NV12/P010 / array slice / padded crop / resize / independent output / cancel"); return true;
}
bool playerChecks(const QString &path)
{
    MediaPlayer player; player.setVolume(0); QSignalSpy error(&player, &MediaPlayer::failed), complete(&player, &MediaPlayer::completed);
    for (int i = 0; i < 2; ++i) {
        CHECK(player.begin(path, true, false, VideoDecodeMode::RequireHardware));
        CHECK(until([&] { player.takeVideo(); return !player.isRunning(); }));
        QCoreApplication::processEvents(); const auto stats = player.stats();
        if (!error.isEmpty()) qWarning().noquote() << error.first().first().toString();
        CHECK(error.isEmpty() && complete.size() == i + 1 && complete.last().first().toBool());
        CHECK(stats.gpuVideo == 48 && stats.softwareVideo == 0 && stats.readbackVideo == 0 && stats.audioSamples > 70000);
        qInfo("hardware audio playback: round=%d position=%lld samples=%llu drops=%llu underruns=%llu", i + 1,
            stats.positionUs, stats.audioSamples, stats.droppedVideo, stats.audioUnderruns);
        CHECK(stats.positionUs >= 1500000 && stats.positionUs <= 1700000 && stats.audioUnderruns == 0);
    }
    CHECK(player.begin(path, false, false, VideoDecodeMode::RequireHardware));
    CHECK(until([&] { return player.stats().gpuVideo >= 16; }));
    QElapsedTimer stop; stop.start(); player.stop(); CHECK(player.wait(2000));
    CHECK(!player.takeVideo().source.texture); qInfo("PASS hardware playback: audio master / EOF tail / two restarts / stop=%lldms", stop.elapsed());
    return true;
}
bool mvcChecks(const QString &path)
{
    MainWindow view(ClientRole::Player); LoginModel login; SessionModel session; SignalClient signal;
    SessionController controller(&view, &login, &session, &signal);
    view.findChild<QComboBox *>("modeCombo")->setCurrentIndex(1);
    view.findChild<QCheckBox *>("playAudioCheck")->setChecked(false);
    login.completeLogin("hardware-test", "root", "root", 60); view.applyLoginState("root", false, true);
    view.findChild<QLineEdit *>("localMediaEdit")->setText(path); view.resize(720, 640); view.show();
    view.findChild<QPushButton *>("playMediaButton")->click();
    auto *worker = controller.findChild<MediaPlayer *>(); CHECK(worker);
    CHECK(until([&] { return worker->stats().gpuVideo >= 5 && view.findChild<PreviewWindow *>()->isVisible(); }));
    CHECK(view.findChild<QLabel *>("playbackStatusLabel")->text().contains(QStringLiteral("GPU")));
    QDir().mkpath("out/GPU解码验证"); CHECK(view.grab().save(QStringLiteral("out/GPU解码验证/硬件解码状态页面.png")));
    view.findChild<QPushButton *>("stopMediaButton")->click();
    CHECK(until([&] { return !view.findChild<QPushButton *>("stopMediaButton")->isEnabled(); }));
    CHECK(!view.findChild<PreviewWindow *>()->isVisible()); qInfo("PASS actual Player MVC: automatic hardware / GPU preview / path counters / stop"); return true;
}
bool checks()
{
    QDir().mkpath("out"); QTemporaryDir temp(QDir::current().filePath("out/gpu-decode-XXXXXX")); CHECK(temp.isValid());
    const auto main = temp.filePath(QStringLiteral("硬件解码.mp4"));
    CHECK(ffmpeg({"-f", "lavfi", "-i", "testsrc2=s=192x108:r=30:d=1.6", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1.6",
        "-c:v", "libx264", "-preset", "fast", "-bf", "2", "-g", "12", "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_range", "tv", "-c:a", "aac", main}));
    if (qApp->arguments().contains("--player-only")) return playerChecks(main) && mvcChecks(main);
    CHECK(surfaceChecks() && compare(main));
    for (const auto matrix : {QString("bt709"), QString("smpte170m")}) for (const auto range : {QString("tv"), QString("pc")}) {
        const auto path = temp.filePath(matrix + range + ".mp4");
        CHECK(ffmpeg({"-f", "lavfi", "-i", "testsrc2=s=192x108:r=30:d=0.8", "-vf", range == "pc" ? "scale=in_range=tv:out_range=pc" : "null",
            "-c:v", "libx264", "-preset", "fast", "-bf", "2", "-g", "12", "-colorspace", matrix, "-color_range", range, path}));
        CHECK(compare(path));
    }
    const auto firstPart = temp.filePath("first.h264"), secondPart = temp.filePath("second.h264"), resized = temp.filePath("resize.h264");
    CHECK(ffmpeg({"-f", "lavfi", "-i", "testsrc2=s=192x108:r=30:d=0.8", "-c:v", "libx264", "-bf", "2", "-f", "h264", firstPart}));
    CHECK(ffmpeg({"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=0.8", "-c:v", "libx264", "-bf", "2", "-f", "h264", secondPart}));
    QFile a(firstPart), b(secondPart), output(resized);
    CHECK(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly));
    const auto bytes = a.readAll() + b.readAll(); CHECK(output.write(bytes) == bytes.size()); output.close();
    CHECK(compare(resized)); qInfo("PASS mid-stream H.264 resolution change: 192x108 -> 320x180 / B-frame drain");
    const auto bt2020 = temp.filePath("bt2020.mp4");
    CHECK(ffmpeg({"-f", "lavfi", "-i", "testsrc2=s=192x108:r=30:d=0.8", "-c:v", "libx264", "-colorspace", "bt2020nc", bt2020}));
    int readback = 0;
    { MediaDecoder decoder; decoder.open(bt2020, [] { return false; }, [&](const PlaybackVideo &frame) {
        if (frame.decodePath == VideoDecodePath::D3D11Readback && !frame.decoderNote.isEmpty()) ++readback;
      }, [](const PlaybackAudio &) {}); while (decoder.readNext()) {} decoder.finish(); }
    CHECK(readback == 24); qInfo("PASS unsupported GPU color space: hardware readback explicitly counted");
    // 10-bit H.264不受常见D3D11VA支持，自动模式应成功软件回退，严格模式明确失败。
    const auto high10 = temp.filePath("high10.mp4");
    CHECK(ffmpeg({"-f", "lavfi", "-i", "testsrc2=s=192x108:r=30:d=0.8", "-c:v", "libx264", "-pix_fmt", "yuv420p10le", high10}));
    int software = 0; bool note = false;
    { MediaDecoder decoder; decoder.open(high10, [] { return false; }, [&](const PlaybackVideo &frame) {
        if (frame.decodePath == VideoDecodePath::SoftwareUpload) ++software; note = note || !frame.decoderNote.isEmpty();
      }, [](const PlaybackAudio &) {}); while (decoder.readNext()) {} decoder.finish(); }
    CHECK(software == 24 && note);
    bool rejected = false;
    try { MediaDecoder decoder; decoder.open(high10, [] { return false; }, [](const PlaybackVideo &) {}, [](const PlaybackAudio &) {}, false, VideoDecodeMode::RequireHardware);
        while (decoder.readNext()) {} decoder.finish(); } catch (const std::exception &) { rejected = true; }
    CHECK(rejected); qInfo("PASS unsupported H.264 High10: software fallback / strict hardware rejection");
    CHECK(playerChecks(main) && mvcChecks(main)); return true;
}
}
int main(int argc, char **argv)
{
    SetErrorMode(0x8003);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) { std::fprintf(stderr, "%s\n", message.toUtf8().constData()); std::fflush(stderr); });
    QApplication app(argc, argv);
    const int font = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).constFirst(), 10));
    try { return checks() ? 0 : 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
    catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
