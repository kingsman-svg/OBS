#include "ffmpeg/MediaPublisher.h"
#include "ffmpeg/GpuVideoOutput.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <windows.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <QFileInfo>

using namespace csn;
using Microsoft::WRL::ComPtr;
#define CHECK(condition) do { if (!(condition)) { qCritical("FAIL line %d: %s", __LINE__, #condition); return false; } } while (0)
namespace {
bool until(const std::function<bool()> &condition, int timeout = 10000)
{
    QElapsedTimer time; time.start();
    while (!condition() && time.elapsed() < timeout) QTest::qWait(5);
    return condition();
}
TimedAudioPacket audio(qint64 pts)
{
    TimedAudioPacket packet;
    packet.pts = pts; packet.source.sampleRate = 48000; packet.source.channels = 2; packet.source.channelMask = 3;
    packet.source.pcm.resize(480 * 8);
    for (int i = 0; i < 480; ++i) {
        const float left = float(0.25 * std::sin(2 * 3.141592653589793 * 440 * (pts + i) / 48000));
        const float right = float(0.25 * std::sin(2 * 3.141592653589793 * 880 * (pts + i) / 48000));
        std::memcpy(packet.source.pcm.data() + i * 8, &left, 4); std::memcpy(packet.source.pcm.data() + i * 8 + 4, &right, 4);
    }
    return packet;
}
struct ReadFile {
    AVFormatContext *format = nullptr;
    ~ReadFile() { avformat_close_input(&format); }
    bool open(const QString &path) {
        return avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) >= 0
            && avformat_find_stream_info(format, nullptr) >= 0;
    }
};
struct Decode {
    AVCodecContext *context = nullptr;
    AVFrame *frame = av_frame_alloc();
    ~Decode() { av_frame_free(&frame); avcodec_free_context(&context); }
    bool open(const AVStream *stream) {
        const auto *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec || !frame) return false;
        context = avcodec_alloc_context3(codec);
        return context && avcodec_parameters_to_context(context, stream->codecpar) >= 0
            && (context->pkt_timebase = stream->time_base, avcodec_open2(context, codec, nullptr) >= 0);
    }
};
ComPtr<ID3D11Device> device(bool gpu)
{
    ComPtr<IDXGIAdapter> selected;
    if (gpu) {
        ComPtr<IDXGIFactory1> factory; winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; winrt::check_hresult(adapter->GetDesc1(&desc));
            if (desc.VendorId == 0x10de) { selected = adapter; break; }
        }
        if (!selected) throw std::runtime_error("GPU测试未找到NVIDIA适配器");
    }
    ComPtr<ID3D11Device> result;
    winrt::check_hresult(D3D11CreateDevice(selected.Get(), gpu ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &result, nullptr, nullptr));
    return result;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device *device, int width, int height, uint32_t color, UINT flags)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = flags;
    std::vector<uint32_t> pixels(size_t(width) * height, color);
    D3D11_SUBRESOURCE_DATA data{pixels.data(), UINT(width * 4), 0};
    ComPtr<ID3D11Texture2D> result; winrt::check_hresult(device->CreateTexture2D(&desc, &data, &result)); return result;
}
FaceFrame source(const ComPtr<ID3D11Texture2D> &texture, bool overlay)
{
    FaceFrame frame; frame.video.texture = texture;
    if (overlay) {
        FaceDetection face; face.box = QRectF(40, 30, 120, 100);
        face.landmarks = {QPointF(75, 60), QPointF(125, 60), QPointF(100, 85), QPointF(80, 105), QPointF(120, 105)};
        frame.faces.append(face);
    }
    return frame;
}
std::array<int, 3> pixel(ID3D11Device *device, ID3D11Texture2D *texture, int x, int y)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &staging));
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context); context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map{}; winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    const auto *p = static_cast<uint8_t *>(map.pData) + y * map.RowPitch + x * 4;
    const std::array<int, 3> value{p[2], p[1], p[0]}; context->Unmap(staging.Get(), 0); return value;
}
bool gpuOutput()
{
    auto d3d = device(false);
    auto input = texture(d3d.Get(), 320, 180, 0xff203040, D3D11_BIND_SHADER_RESOURCE);
    auto output = texture(d3d.Get(), 320, 320, 0xffeeeeee, D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11DeviceContext> context; d3d->GetImmediateContext(&context);
    const D3D11_VIEWPORT original{1, 2, 3, 4, 0, 1}; context->RSSetViewports(1, &original);
    GpuVideoOutput render(d3d.Get()); render.render(source(input, true), output.Get());
    CHECK(pixel(d3d.Get(), output.Get(), 10, 10) == (std::array<int, 3>{0, 0, 0}));
    const auto body = pixel(d3d.Get(), output.Get(), 200, 150);
    CHECK(body == (std::array<int, 3>{32, 48, 64}));
    const auto box = pixel(d3d.Get(), output.Get(), 40, 120);
    CHECK(box[1] > 240 && box[0] < 40);
    CHECK(pixel(d3d.Get(), input.Get(), 40, 50) == body); // 采集快照始终只读。
    UINT count = 1; D3D11_VIEWPORT restored{}; context->RSGetViewports(&count, &restored);
    CHECK(restored.TopLeftX == original.TopLeftX && restored.Width == original.Width);
    auto resized = texture(d3d.Get(), 180, 320, 0xff123456, D3D11_BIND_SHADER_RESOURCE);
    render.render(source(resized, false), output.Get());
    CHECK(pixel(d3d.Get(), output.Get(), 10, 160) == (std::array<int, 3>{0, 0, 0}));
    qInfo("PASS GPU output: overlay / letterbox / resize / read-only source / context restoration"); return true;
}
bool audioRecording()
{
    QTemporaryDir directory(QDir::current().filePath("out/encode-test-XXXXXX")); CHECK(directory.isValid());
    RecordingSettings settings; settings.video = false;
    settings.path = directory.filePath(QStringLiteral("音频 空格.mp4"));
    MediaPublisher publisher;
    QSignalSpy opened(&publisher, &MediaPublisher::opened), errors(&publisher, &MediaPublisher::failed), completed(&publisher, &MediaPublisher::completed);
    CHECK(publisher.begin(settings)); CHECK(until([&] { return !opened.isEmpty() || !errors.isEmpty(); })); CHECK(errors.isEmpty());
    // 207个480样本包，插入100ms时间缺口；每批等编码统计，避免人为队列溢出。
    for (int i = 0; i < 207; ++i) {
        publisher.pushAudio(audio(2400 + i * 480 + (i >= 100 ? 4800 : 0)));
        if (i % 10 == 9) CHECK(until([&] { return publisher.stats().audioSamples >= quint64(i + 1) * 480 || !errors.isEmpty(); }));
    }
    publisher.stop(); publisher.pushAudio(audio(999999)); CHECK(publisher.wait(10000)); QCoreApplication::processEvents();
    for (const auto &failure : errors) qWarning().noquote() << failure.first().toString();
    qInfo("Recording result: completed=%lld exists=%d samples=%llu", completed.size(), QFileInfo::exists(settings.path), publisher.stats().audioSamples);
    CHECK(errors.isEmpty() && completed.size() == 1 && QFileInfo::exists(settings.path));
    const auto stats = publisher.stats();
    CHECK(stats.audioSamples == 207 * 480 && stats.silentSamples == 4800 && stats.droppedAudio == 0);
    ReadFile file; CHECK(file.open(settings.path)); CHECK(file.format->nb_streams == 1);
    auto *stream = file.format->streams[0];
    CHECK(stream->codecpar->codec_id == AV_CODEC_ID_AAC && stream->codecpar->sample_rate == 48000 && stream->codecpar->ch_layout.nb_channels == 2);
    Decode decoder; CHECK(decoder.open(stream));
    AVPacket *packet = av_packet_alloc(); CHECK(packet);
    qint64 previous = INT64_MIN, end = -1; int packets = 0, samples = 0;
    double left440 = 0, left880 = 0, right440 = 0, right880 = 0; int nearSilence = 0;
    bool valid = true;
    auto receive = [&] {
        int result;
        while ((result = avcodec_receive_frame(decoder.context, decoder.frame)) >= 0) {
            const auto *left = reinterpret_cast<const float *>(decoder.frame->extended_data[0]);
            const auto *right = reinterpret_cast<const float *>(decoder.frame->extended_data[1]);
            valid = valid && decoder.frame->format == AV_SAMPLE_FMT_FLTP;
            for (int j = 0; j < decoder.frame->nb_samples; ++j, ++samples) {
                left440 += left[j] * std::sin(2 * 3.141592653589793 * 440 * samples / 48000);
                left880 += left[j] * std::sin(2 * 3.141592653589793 * 880 * samples / 48000);
                right440 += right[j] * std::sin(2 * 3.141592653589793 * 440 * samples / 48000);
                right880 += right[j] * std::sin(2 * 3.141592653589793 * 880 * samples / 48000);
                if (samples > 50000 && samples < 53000 && std::abs(left[j]) < 0.001 && std::abs(right[j]) < 0.001) ++nearSilence;
            }
        }
        valid = valid && (result == AVERROR(EAGAIN) || result == AVERROR_EOF);
    };
    while (av_read_frame(file.format, packet) >= 0) {
        valid = valid && packet->dts > previous; previous = packet->dts; end = packet->pts + packet->duration; ++packets;
        const int sent = avcodec_send_packet(decoder.context, packet);
        valid = valid && sent >= 0; receive(); av_packet_unref(packet);
    }
    const int flushed = avcodec_send_packet(decoder.context, nullptr);
    valid = valid && flushed >= 0; receive(); av_packet_free(&packet);
    const auto endSamples = av_rescale_q(end, stream->time_base, {1, 48000});
    qInfo("AAC timestamps: end=%lld start=%lld duration=%lld packets=%d decoded=%d", endSamples, stream->start_time, stream->duration, packets, samples);
    CHECK(valid && packets > 100 && samples >= 207 * 480);
    const auto trackEnd = av_rescale_q(stream->start_time + stream->duration, stream->time_base, {1, 48000});
    CHECK(std::abs(trackEnd - (2400 + 207 * 480 + 4800)) <= 48); // 容器编辑列表保存实际尾长，AAC解码帧仍可含补齐样本。
    CHECK(std::abs(left440) > std::abs(left880) * 5 && std::abs(right880) > std::abs(right440) * 5 && nearSilence > 2000);
    qInfo("PASS AAC/MP4: input=%llu gap=%llu packets=%d decoded=%d end=%lld; independent channels, actual tail, Unicode path",
        stats.audioSamples, stats.silentSamples, packets, samples, endSamples);
    // 同名文件拒绝覆盖，重新begin可恢复；重叠PTS失败删除临时文件，abort也不提交文件。
    const auto originalSize = QFileInfo(settings.path).size();
    errors.clear(); CHECK(publisher.begin(settings)); publisher.stop(); CHECK(publisher.wait(10000)); QCoreApplication::processEvents();
    CHECK(errors.size() == 1 && QFileInfo(settings.path).size() == originalSize);
    settings.path = directory.filePath("overlap.mp4"); errors.clear(); opened.clear();
    CHECK(publisher.begin(settings)); publisher.pushAudio(audio(0)); publisher.pushAudio(audio(0));
    CHECK(publisher.wait(10000)); QCoreApplication::processEvents(); CHECK(errors.size() == 1 && !QFileInfo::exists(settings.path));
    settings.path = directory.filePath("abort.mp4"); errors.clear(); opened.clear();
    CHECK(publisher.begin(settings)); CHECK(until([&] { return !opened.isEmpty(); })); publisher.pushAudio(audio(0)); publisher.abort();
    CHECK(publisher.wait(10000)); QCoreApplication::processEvents(); CHECK(errors.isEmpty() && !QFileInfo::exists(settings.path));
    settings.path = directory.filePath("restart.mp4"); opened.clear(); CHECK(publisher.begin(settings)); publisher.pushAudio(audio(0));
    publisher.stop(); CHECK(publisher.wait(10000)); QCoreApplication::processEvents(); CHECK(QFileInfo::exists(settings.path));
    CHECK(QDir(directory.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
    settings.video = true; settings.path = directory.filePath("no-video.mp4"); completed.clear(); errors.clear();
    CHECK(publisher.begin(settings));
    for (int i = 0; i < 200; ++i) publisher.pushAudio(audio(i * 480)); // 首帧未到，不初始化GPU，音频入口仍有界。
    CHECK(publisher.stats().droppedAudio == 150);
    publisher.stop(); publisher.pushAudio(audio(200 * 480)); CHECK(publisher.wait(10000)); QCoreApplication::processEvents();
    CHECK(errors.isEmpty() && completed.size() == 1 && completed.first().first().toString().isEmpty() && !QFileInfo::exists(settings.path));
    CHECK(publisher.stats().droppedAudio == 150);
    qInfo("PASS recording lifecycle: late push / no overwrite / overlap failure / abort / restart / temporary cleanup");
    return true;
}
bool hardwareRecording()
{
    auto d3d = device(true);
    auto bright = texture(d3d.Get(), 320, 180, 0xffeeeeee, D3D11_BIND_SHADER_RESOURCE);
    auto dark = texture(d3d.Get(), 180, 320, 0xff101010, D3D11_BIND_SHADER_RESOURCE);
    RecordingSettings settings; settings.width = 640; settings.height = 360;
    settings.path = QDir::current().filePath(QStringLiteral("out/编码验证.mp4"));
    QFile::remove(settings.path); // 仅移除本测试固定的可重复生成输出。
    MediaPublisher publisher;
    QSignalSpy errors(&publisher, &MediaPublisher::failed), opened(&publisher, &MediaPublisher::opened), completed(&publisher, &MediaPublisher::completed);
    CHECK(publisher.begin(settings));
    int nextAudio = 0;
    for (int i = 0; i < 90; ++i) {
        publisher.pushVideo({source((i / 15) % 2 ? dark : bright, i == 0), i});
        while (nextAudio * 480 < (i + 1) * 1600) {
            auto packet = audio(nextAudio++ * 480);
            if (packet.pts % 48000 >= 4800) packet.source.pcm.fill('\0'); // 每秒亮起时同步100ms音频脉冲。
            publisher.pushAudio(packet);
        }
        CHECK(until([&] { return publisher.stats().videoFrames >= quint64(i + 1) || !errors.isEmpty(); }));
        if (!errors.isEmpty()) qCritical().noquote() << errors.first().first().toString();
        CHECK(errors.isEmpty());
    }
    publisher.stop(); CHECK(publisher.wait(10000)); QCoreApplication::processEvents();
    if (!errors.isEmpty()) qCritical().noquote() << errors.first().first().toString();
    CHECK(errors.isEmpty() && completed.size() == 1 && publisher.stats().videoFrames == 90);
    ReadFile file; CHECK(file.open(settings.path)); CHECK(file.format->nb_streams == 2);
    int video = -1, audioIndex = -1;
    for (unsigned i = 0; i < file.format->nb_streams; ++i) {
        if (file.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) video = int(i); else audioIndex = int(i);
    }
    CHECK(video >= 0 && audioIndex >= 0);
    auto *stream = file.format->streams[video]; CHECK(stream->codecpar->codec_id == AV_CODEC_ID_H264);
    CHECK(stream->codecpar->width == 640 && stream->codecpar->height == 360);
    Decode decoder, audioDecoder; CHECK(decoder.open(stream)); CHECK(audioDecoder.open(file.format->streams[audioIndex]));
    AVPacket *packet = av_packet_alloc(); CHECK(packet);
    std::array<qint64, 2> last{INT64_MIN, INT64_MIN}, ends{};
    int frames = 0; bool valid = true, overlay = false, letterbox = false;
    std::array<qint64, 3> videoOnsets{INT64_MIN, INT64_MIN, INT64_MIN}, audioOnsets{INT64_MIN, INT64_MIN, INT64_MIN};
    auto receive = [&] {
        int result;
        while ((result = avcodec_receive_frame(decoder.context, decoder.frame)) >= 0) {
            const auto *decoded = decoder.frame;
            valid = valid && decoded->format == AV_PIX_FMT_YUV420P;
            if (frames == 0) {
                const int x = 80, y = 100;
                const int u = decoded->data[1][(y / 2) * decoded->linesize[1] + x / 2];
                const int v = decoded->data[2][(y / 2) * decoded->linesize[2] + x / 2];
                overlay = u < 110 && v < 110; // 编码后仍存在绿色框，原灰色画面没有此色度。
            }
            if (frames == 15) letterbox = decoded->data[0][180 * decoded->linesize[0] + 10] < 25;
            const int center = decoded->data[0][300 * decoded->linesize[0] + 320]; // 避开首帧框线。
            valid = valid && ((frames / 15) % 2 ? center < 45 : center > 200);
            if (frames % 30 == 0 && frames / 30 < 3) videoOnsets[frames / 30] = av_rescale_q(decoded->best_effort_timestamp, stream->time_base, {1, 48000});
            ++frames;
        }
        valid = valid && (result == AVERROR(EAGAIN) || result == AVERROR_EOF);
    };
    auto receiveAudio = [&] {
        int result;
        while ((result = avcodec_receive_frame(audioDecoder.context, audioDecoder.frame)) >= 0) {
            const auto *decoded = audioDecoder.frame;
            valid = valid && decoded->format == AV_SAMPLE_FMT_FLTP;
            const auto pts = av_rescale_q(decoded->pts, audioDecoder.context->pkt_timebase, {1, 48000});
            const auto *left = reinterpret_cast<const float *>(decoded->extended_data[0]);
            for (int j = 0; j < decoded->nb_samples; ++j) {
                const auto sample = pts + j; const int slot = int(sample / 48000);
                if (sample >= 0 && slot < 3 && audioOnsets[slot] == INT64_MIN && std::abs(left[j]) > 0.1) audioOnsets[slot] = sample;
            }
        }
        valid = valid && (result == AVERROR(EAGAIN) || result == AVERROR_EOF);
    };
    while (av_read_frame(file.format, packet) >= 0) {
        const int index = packet->stream_index;
        valid = valid && packet->dts > last[index]; last[index] = packet->dts; ends[index] = packet->pts + packet->duration;
        if (index == video) { const int sent = avcodec_send_packet(decoder.context, packet); valid = valid && sent >= 0; receive(); }
        else { const int sent = avcodec_send_packet(audioDecoder.context, packet); valid = valid && sent >= 0; receiveAudio(); }
        av_packet_unref(packet);
    }
    const int flushed = avcodec_send_packet(decoder.context, nullptr);
    valid = valid && flushed >= 0; receive(); av_packet_free(&packet);
    const int audioFlushed = avcodec_send_packet(audioDecoder.context, nullptr); valid = valid && audioFlushed >= 0; receiveAudio();
    const double videoEnd = ends[video] * av_q2d(stream->time_base);
    const double audioEnd = ends[audioIndex] * av_q2d(file.format->streams[audioIndex]->time_base);
    qInfo("GPU decoded: valid=%d frames=%d overlay=%d letterbox=%d ends=%.6f / %.6f", valid, frames, overlay, letterbox, videoEnd, audioEnd);
    CHECK(valid && frames == 90 && overlay && letterbox && std::abs(videoEnd - 3) < .002 && std::abs(audioEnd - videoEnd) < .035);
    qint64 maxOffset = 0;
    for (int i = 0; i < 3; ++i) {
        CHECK(videoOnsets[i] != INT64_MIN && audioOnsets[i] != INT64_MIN);
        maxOffset = std::max(maxOffset, std::abs(videoOnsets[i] - audioOnsets[i]));
    }
    CHECK(maxOffset < 480); // AAC解码后光/音脉冲偏差小于10ms，覆盖编码延迟和容器skip-samples。
    qInfo("PASS RTX NVENC D3D11->H264/AAC MP4: %d decoded frames, overlay, resize, DTS monotonic, ends %.6f / %.6f", frames, videoEnd, audioEnd);
    qInfo("PASS decoded flash/tone alignment: 3 events, max offset %.3f ms", maxOffset / 48.0);
    qInfo().noquote() << "Output:" << settings.path; return true;
}
}
int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toUtf8(); std::fprintf(stderr, "%s\n", bytes.constData()); std::fflush(stderr);
    });
    QCoreApplication app(argc, argv); QDir().mkpath("out");
    try {
        if (app.arguments().contains("--gpu")) return hardwareRecording() ? 0 : 1;
        return audioRecording() && gpuOutput() ? 0 : 1;
    } catch (const winrt::hresult_error &error) { qCritical().noquote() << QString::fromStdWString(error.message().c_str()); }
      catch (const std::exception &error) { qCritical().noquote() << error.what(); }
    return 1;
}
