#include "MediaTimeline.h"
#include <QCoreApplication>
#include <QDebug>
#include <winrt/base.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace csn;
#define CHECK(condition) do { if (!(condition)) { qCritical("Check failed at line %d: %s", __LINE__, #condition); return false; } } while (false)
namespace {
constexpr qint64 Base = 100000000;
Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
FaceFrame video(qint64 time, quint64 sequence = 1)
{
    FaceFrame frame;
    frame.video.texture = texture; frame.video.timestamp100ns = time; frame.video.sequence = sequence;
    FaceDetection face; face.box = QRectF(double(sequence), 0, 1, 1); frame.faces.append(face);
    return frame;
}
AudioPacket audio(qint64 time)
{
    AudioPacket packet;
    packet.sampleRate = 48000; packet.channels = 2; packet.channelMask = 3;
    packet.timestamp100ns = time; packet.pcm = QByteArray(480 * 8, '\0');
    return packet;
}
bool rejects(const std::function<void()> &action)
{
    try { action(); } catch (const std::exception &) { return true; }
    return false;
}
bool commonClock()
{
    MediaTimeline clock; clock.begin(Base, true, true);
    CHECK(clock.pushVideo(video(Base + 1000000))); // GPU结果晚到，仍代表100ms处的内容。
    CHECK(clock.pushAudio(audio(Base + 1000000)));
    CHECK(clock.takeFrames(Base + 1199999).video.isEmpty());
    auto output = clock.takeFrames(Base + 2300000);
    CHECK(output.video.size() == 1 && output.audio.size() == 1);
    CHECK(output.video[0].pts == 3 && output.audio[0].pts == 4800);
    CHECK(output.video[0].pts * 48000 == output.audio[0].pts * 30); // 不同时间基，同一个100ms时刻。
    CHECK(output.video[0].source.video.timestamp100ns == Base + 1000000);
    CHECK(output.video[0].source.faces[0].box.x() == 1);
    CHECK(clock.takeFrames(Base + 2200000).video.isEmpty()); // 时钟回退不重复交付。
    output = clock.takeFrames(Base + 3200000);
    CHECK(!output.video.isEmpty() && output.video.back().repeated);
    CHECK(output.video.back().pts == 6 && output.video.back().source.video.timestamp100ns == Base + 1000000);
    CHECK(!clock.pushVideo(video(Base + 900000, 2))); // 已发布时刻之前的迟到结果不能覆盖新画面。
    CHECK(!clock.pushAudio(audio(Base + 1000000))); // 重复音频不能叠加或重放。
    qInfo("PASS common origin / distinct PTS bases / 120ms total wait / delayed GPU / repeated static frame");
    return true;
}
bool steadyTenMinutes()
{
    MediaTimeline clock; clock.begin(Base, true, true);
    qint64 lastAudio = -480, lastVideo = -1;
    quint64 lastSequence = 0;
    int flashes = 0, pulses = 0;
    double worstOffset = 0;
    const auto inspect = [&](const MediaBatch &output) {
        for (const auto &frame : output.video) {
            if (frame.pts != lastVideo + 1) return false;
            lastVideo = frame.pts;
            if (frame.source.video.sequence != lastSequence) {
                ++flashes; lastSequence = frame.source.video.sequence;
                const double offset = double(frame.pts) / 30 - double(frame.source.video.timestamp100ns - Base) / 10000000;
                if (offset < -0.000001 || offset > 1.0 / 30 + 0.000001) return false;
                worstOffset = std::max(worstOffset, std::abs(offset));
            }
        }
        for (const auto &packet : output.audio) {
            if (packet.pts != lastAudio + 480 || packet.source.discontinuity) return false;
            lastAudio = packet.pts;
            float sample = 0; std::memcpy(&sample, packet.source.pcm.constData(), sizeof(sample));
            if (sample > 0) { if (packet.pts % 4800) return false; ++pulses; }
        }
        return true;
    };
    // 人工10分钟；每100ms同时闪屏/脉冲，GPU延迟50ms，GUI每30ms取批次，不等待真实时间。
    for (int tick = 0; tick < 60000; ++tick) {
        auto packet = audio(Base + qint64(tick) * 100000);
        if (tick % 10 == 0) { const float pulse = 0.5f; std::memcpy(packet.pcm.data(), &pulse, sizeof(pulse)); }
        CHECK(clock.pushAudio(packet));
        if (tick >= 5 && (tick - 5) % 10 == 0)
            CHECK(clock.pushVideo(video(Base + qint64(tick - 5) * 100000, quint64((tick - 5) / 10 + 1))));
        if (tick % 3 == 0) CHECK(inspect(clock.takeFrames(Base + qint64(tick) * 100000)));
    }
    CHECK(inspect(clock.finish()));
    CHECK(lastAudio == 59999 * qint64(480) && flashes == 6000 && pulses == 6000);
    CHECK(clock.stats().droppedAudio == 0 && clock.stats().droppedVideo == 0);
    CHECK(worstOffset < 0.000001); // 100ms整点的闪屏/声音PTS对齐，10分钟不随回调次数漂移。
    qInfo("PASS synthetic 10-minute flash/pulse alignment: %d events, max PTS offset %.6f ms", flashes, worstOffset * 1000);
    return true;
}
bool boundsAndStops()
{
    MediaTimeline clock; clock.begin(Base, true, true);
    for (int i = 0; i < 80; ++i) CHECK(clock.pushAudio(audio(Base + qint64(i) * 100000)));
    for (int i = 0; i < 25; ++i) CHECK(clock.pushVideo(video(Base + qint64(i) * 333334, quint64(i + 1))));
    auto tail = clock.finish();
    CHECK(tail.audio.size() == 50 && tail.video.size() <= 16 && !tail.video.isEmpty());
    CHECK(tail.audio.front().source.discontinuity && tail.video.front().discontinuity);
    CHECK(clock.stats().droppedAudio == 30 && clock.stats().droppedVideo > 0);
    CHECK(!clock.active() && clock.finish().audio.isEmpty() && !clock.pushAudio(audio(Base + 9000000)));
    clock.begin(Base, true, true);
    CHECK(clock.pushVideo(video(Base))); CHECK(clock.pushAudio(audio(Base)));
    auto late = clock.takeFrames(Base + 20000000 + MediaTimeline::Delay100ns);
    CHECK(late.video.size() <= 6 && late.audio.isEmpty() && late.video.front().discontinuity);
    CHECK(clock.pushAudio(audio(Base + 20000000)));
    late = clock.takeFrames(Base + 20200000 + MediaTimeline::Delay100ns);
    CHECK(late.audio.size() == 1 && late.audio[0].source.discontinuity && late.audio[0].pts == 96000);
    clock.clear(); CHECK(clock.takeFrames(Base + 30000000).video.isEmpty());
    CHECK(!clock.pushVideo(video(Base + 30000000)));
    clock.begin(Base + 40000000, true, false);
    CHECK(clock.stats().videoFrames == 0 && clock.pushVideo(video(Base + 40010000)));
    tail = clock.finish(); CHECK(tail.video.size() == 1 && tail.video[0].pts == 1); // 不丢最后一个非整格采集帧。
    clock.begin(Base, false, true); CHECK(clock.pushAudio(audio(Base)));
    tail = clock.takeFrames(Base + 1300000); CHECK(tail.video.isEmpty() && tail.audio.size() == 1);
    CHECK(!clock.pushVideo(video(Base))); clock.disableAudio();
    CHECK(!clock.pushAudio(audio(Base + 100000)));
    clock.begin(Base, true, true); clock.disableVideo(); CHECK(clock.pushAudio(audio(Base)));
    tail = clock.takeFrames(Base + 1300000); CHECK(tail.video.isEmpty() && tail.audio.size() == 1);
    qInfo("PASS bounded catch-up / stop drain / logout discard / restart / last frame / single-track failure");
    return true;
}
bool validation()
{
    MediaTimeline clock;
    CHECK(rejects([&] { clock.begin(0, true, true); }));
    clock.begin(Base, true, true);
    auto invalid = audio(Base); invalid.sampleRate = 44100;
    CHECK(rejects([&] { clock.pushAudio(invalid); }));
    CHECK(!clock.pushAudio(audio(Base - 1)) && !clock.pushVideo(video(Base - 1)));
    auto estimated = audio(Base); estimated.timestampEstimated = true;
    CHECK(clock.pushAudio(estimated)); CHECK(clock.finish().audio[0].source.timestampEstimated);
    qInfo("PASS input validation / pre-origin reject / estimated timestamp propagation");
    return true;
}
}
int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toUtf8(); std::fprintf(stderr, "%s\n", bytes.constData()); std::fflush(stderr);
    });
    QCoreApplication app(argc, argv);
    try {
        // 仅WARP小纹理，验证引用寿命；不使用采集设备、不访问模型、不读回视频像素。
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 4; desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &texture));
        return commonClock() && steadyTenMinutes() && boundsAndStops() && validation() ? 0 : 1;
    } catch (const std::exception &error) { qCritical() << error.what(); return 1; }
    catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); return 1; }
}
