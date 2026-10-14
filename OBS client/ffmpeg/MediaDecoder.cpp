#include "MediaDecoder.h"
#include "GpuVideoConverter.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <QFileInfo>
#include <QUrl>
#include <QMutex>
#include <QMutexLocker>
#include <d3d11_4.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace csn {
namespace {
using Microsoft::WRL::ComPtr;
void check(int result, const char *operation)
{
    if (result >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(result, text, sizeof(text));
    throw std::runtime_error(std::string(operation) + ": " + text);
}
qint64 nowUs()
{
    LARGE_INTEGER tick{}, rate{}; QueryPerformanceCounter(&tick); QueryPerformanceFrequency(&rate);
    return tick.QuadPart / rate.QuadPart * 1000000 + tick.QuadPart % rate.QuadPart * 1000000 / rate.QuadPart;
}
struct PacketDelete { void operator()(AVPacket *packet) const { av_packet_free(&packet); } };
struct FrameDelete { void operator()(AVFrame *frame) const { av_frame_free(&frame); } };
ComPtr<ID3D11Device> playbackDevice()
{
    // Windows播放优先选NVIDIA适配器，普通硬件/WARP仍可用于软件解码纹理上传。
    ComPtr<IDXGIFactory1> factory; ComPtr<IDXGIAdapter1> selected;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10de) { selected = adapter; break; }
    }
    ComPtr<ID3D11Device> result;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT created = E_FAIL;
    if (selected) created = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &result, nullptr, nullptr);
    if (FAILED(created)) created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &result, nullptr, nullptr);
    if (FAILED(created)) created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &result, nullptr, nullptr);
    if (FAILED(created)) winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &result, nullptr, nullptr));
    ComPtr<ID3D11DeviceContext> context; result->GetImmediateContext(&context);
    ComPtr<ID3D11Multithread> protection; winrt::check_hresult(context.As(&protection)); protection->SetMultithreadProtected(TRUE);
    return result;
}
}

struct MediaDecoder::Resources {
    AVFormatContext *format = nullptr;           // 独占输入容器，包含AVStream和底层IO。
    AVCodecContext *video = nullptr, *audio = nullptr; // 视频优先D3D11VA，音频继续CPU解码。
    int videoIndex = -1, audioIndex = -1;        // 其余流直接忽略，不把字幕/封面当视频。
    SwsContext *sws = nullptr;                  // 解码像素格式转换成现有Renderer支持的BGRA。
    SwrContext *swr = nullptr;                  // 解码音频统一成48kHz立体声交错Float32。
    AVChannelLayout inputLayout{};             // 已配置SWR输入布局，重建时比较而非只看声道数。
    int inputRate = 0, inputFormat = -1;        // 运行中音频格式变化时重建SWR。
    ComPtr<ID3D11Device> device;                // 同轮硬解、GPU转换、上传与显示共用的设备。
    AVBufferRef *hardware = nullptr;           // FFmpeg持有设备引用，释放video后再归还本轮引用。
    std::unique_ptr<GpuVideoConverter> gpu;    // 普通转换对象，借用硬件AVFrame，输出独立BGRA。
    VideoDecodeMode mode = VideoDecodeMode::Automatic; // 本轮策略，不在读包过程中切换。
    QString decoderNote;                     // 未获硬件/转换失败的明确原因，随帧反馈界面。
    QMutex noteMutex;                         // FFmpeg的get_format可能在内部解码线程调用，保护提示文本。
    bool conversionFailed = false;           // GPU转换失败后锁存回读策略，不每帧重复初始化。
    VideoSink videoSink; AudioSink audioSink;   // 同播放线程调用，回调负责背压/停止检查。
    std::function<bool()> cancelled;           // 读取期间的线程停止判断；不访问GUI对象。
    qint64 deadlineUs = 0;                     // 打开/探测/单次读包最多10秒，停止随时可打断。
    qint64 originUs = 0, audioCursor = -1;      // 媒体共同原点与重采样实际输出样本PTS。
    qint64 audioInputEnd = -1, audioEnd = -1;   // 检查输入时间跳变、裁剪AAC尾部填充。
    qint64 nextVideoUs = 0, defaultFrameUs = 40000; // 缺失视频PTS/时长时的连续推算。
    quint64 sequence = 0;                      // 每轮视频纹理从1开始递增，GUI可去重。
    bool finished = false;                     // EOF排空只执行一次。
    bool live = false;                         // 直播缩短读包超时，来源由Controller明确指定。
    ~Resources() {
        avcodec_free_context(&video); avcodec_free_context(&audio); avformat_close_input(&format);
        gpu.reset(); av_buffer_unref(&hardware);
        sws_freeContext(sws); swr_free(&swr); av_channel_layout_uninit(&inputLayout);
    }
    static int interrupt(void *opaque) {
        const auto &r = *static_cast<Resources *>(opaque);
        return r.cancelled() || nowUs() >= r.deadlineUs;
    }
    void setNote(const QString &note) { QMutexLocker lock(&noteMutex); decoderNote = note; } // C回调与播放线程共享。
    static AVPixelFormat chooseFormat(AVCodecContext *context, const AVPixelFormat *formats) {
        // 2a. get_format由FFmpeg解码线程调用，不能向C栈抛异常；初始化或动态格式变化时均可触发。
        auto &r = *static_cast<Resources *>(context->opaque);
        try {
            if (r.hardware) for (auto *p = formats; *p != AV_PIX_FMT_NONE; ++p)
                if (*p == AV_PIX_FMT_D3D11) return *p;
            if (r.mode == VideoDecodeMode::RequireHardware) return AV_PIX_FMT_NONE;
            if (r.hardware) r.setNote(QStringLiteral("设备/视频格式未能启用D3D11VA，使用软件解码"));
            for (auto *p = formats; *p != AV_PIX_FMT_NONE; ++p) {
                const auto *description = av_pix_fmt_desc_get(*p);
                if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
            }
        } catch (...) { return AV_PIX_FMT_NONE; }
        return AV_PIX_FMT_NONE;
    }
    void prepareHardware(const AVCodec *codec) {
        if (mode == VideoDecodeMode::Software) { decoderNote = QStringLiteral("已选择软件解码"); return; }
        bool supported = false;
        for (int i = 0;; ++i) {
            const auto *config = avcodec_get_hw_config(codec, i); if (!config) break;
            if (config->device_type == AV_HWDEVICE_TYPE_D3D11VA && config->pix_fmt == AV_PIX_FMT_D3D11
                && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) { supported = true; break; }
        }
        if (!supported) {
            if (mode == VideoDecodeMode::RequireHardware) throw std::runtime_error("此视频解码器没有D3D11VA硬件配置");
            decoderNote = QStringLiteral("此编码格式未提供D3D11VA，使用软件解码"); return;
        }
        try {
            // FFmpeg与显示借用相同设备；AddRef使AVHWDevice的释放不会提前销毁显示纹理。
            gpu = std::make_unique<GpuVideoConverter>(device.Get());
            hardware = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA); if (!hardware) throw std::bad_alloc();
            auto *native = static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(hardware->data)->hwctx);
            native->device = device.Get(); native->device->AddRef();
            check(av_hwdevice_ctx_init(hardware), "initialize D3D11VA decoder device");
        } catch (const winrt::hresult_error &error) {
            if (mode == VideoDecodeMode::RequireHardware) throw;
            decoderNote = QStringLiteral("D3D11视频设备不可用，软件回退：") + QString::fromStdWString(error.message().c_str());
            gpu.reset(); av_buffer_unref(&hardware);
        } catch (const std::exception &error) {
            if (mode == VideoDecodeMode::RequireHardware) throw;
            decoderNote = QStringLiteral("硬件解码初始化失败，软件回退：") + QString::fromUtf8(error.what());
            gpu.reset(); av_buffer_unref(&hardware);
        }
    }
    qint64 ptsUs(const AVFrame *frame, int index, qint64 fallback) const {
        return frame->best_effort_timestamp == AV_NOPTS_VALUE ? fallback
            : av_rescale_q(frame->best_effort_timestamp, format->streams[index]->time_base, {1, 1000000}) - originUs;
    }
    ComPtr<ID3D11Texture2D> uploadSoftware(const AVFrame *frame) {
        // 4b. 软件或硬件回读回退才执行SWS/CPU上传；正常D3D11VA路径不经过此处。
        // YUVJ与对应YUV的平面布局相同，全范围由color_range描述，避免SWS每帧重建旧格式。
        auto pixelFormat = AVPixelFormat(frame->format);
        if (pixelFormat == AV_PIX_FMT_YUVJ420P) pixelFormat = AV_PIX_FMT_YUV420P;
        else if (pixelFormat == AV_PIX_FMT_YUVJ422P) pixelFormat = AV_PIX_FMT_YUV422P;
        else if (pixelFormat == AV_PIX_FMT_YUVJ444P) pixelFormat = AV_PIX_FMT_YUV444P;
        else if (pixelFormat == AV_PIX_FMT_YUVJ440P) pixelFormat = AV_PIX_FMT_YUV440P;
        sws = sws_getCachedContext(sws, frame->width, frame->height, pixelFormat,
            frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) throw std::bad_alloc();
        const int matrix = frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709
            : frame->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601;
        check(sws_setColorspaceDetails(sws, sws_getCoefficients(matrix), frame->color_range == AVCOL_RANGE_JPEG,
            sws_getCoefficients(matrix), 1, 0, 1 << 16, 1 << 16), "video color conversion");
        QByteArray bgra(qsizetype(frame->width) * frame->height * 4, '\0');
        uint8_t *planes[]{reinterpret_cast<uint8_t *>(bgra.data()), nullptr, nullptr, nullptr};
        int strides[]{frame->width * 4, 0, 0, 0};
        if (sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, strides) != frame->height)
            throw std::runtime_error("视频BGRA转换行数不足");
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = frame->width; desc.Height = frame->height;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.Usage = D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA data{bgra.constData(), UINT(strides[0]), 0}; ComPtr<ID3D11Texture2D> texture;
        winrt::check_hresult(device->CreateTexture2D(&desc, &data, &texture)); return texture;
    }
    void outputVideo(const AVFrame *frame) {
        // 4. 保留原PTS/时长；只替换解码像素的交付路径，不改变主时钟和EOF排空契约。
        if (frame->width <= 0 || frame->height <= 0 || frame->width > 3840 || frame->height > 2160)
            throw std::runtime_error("当前点播支持最大3840×2160视频");
        const qint64 pts = ptsUs(frame, videoIndex, nextVideoUs);
        const qint64 duration = frame->duration > 0
            ? av_rescale_q(frame->duration, format->streams[videoIndex]->time_base, {1, 1000000}) : defaultFrameUs;
        nextVideoUs = pts + std::max<qint64>(1, duration);
        if (pts < 0) return; // 不展示编辑列表/解码预卷之前的帧。
        PlaybackVideo output; output.ptsUs = pts; output.durationUs = std::max<qint64>(1, duration);
        output.source.sequence = ++sequence; output.source.timestamp100ns = nowUs() * 10;
        if (qEnvironmentVariableIsSet("OBS_DECODE_TRACE") && (sequence == 1 || sequence % 30 == 0))
            qInfo("decode frame: sequence=%llu pts=%lld best=%lld duration=%lld format=%d", sequence, pts,
                frame->best_effort_timestamp, duration, frame->format);
        if (frame->format == AV_PIX_FMT_D3D11) {
            if (!conversionFailed) try {
                output.source.texture = gpu->convert(*frame, cancelled); output.decodePath = VideoDecodePath::D3D11Gpu;
            } catch (const winrt::hresult_error &error) {
                if (mode == VideoDecodeMode::RequireHardware || cancelled() || FAILED(device->GetDeviceRemovedReason())
                    || error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) throw;
                conversionFailed = true; setNote(QStringLiteral("GPU转换不可用，硬件回读：") + QString::fromStdWString(error.message().c_str()));
            } catch (const std::exception &error) {
                if (mode == VideoDecodeMode::RequireHardware || cancelled()) throw;
                conversionFailed = true; setNote(QStringLiteral("GPU转换不可用，硬件回读：") + QString::fromUtf8(error.what()));
            }
            if (conversionFailed) {
                std::unique_ptr<AVFrame, FrameDelete> software(av_frame_alloc()); if (!software) throw std::bad_alloc();
                check(av_hwframe_transfer_data(software.get(), frame, 0), "readback hardware fallback");
                check(av_frame_copy_props(software.get(), frame), "copy decoded frame properties");
                output.source.texture = uploadSoftware(software.get()); output.decodePath = VideoDecodePath::D3D11Readback;
            }
        } else {
            if (mode == VideoDecodeMode::RequireHardware) throw std::runtime_error("实际输出未使用D3D11硬件解码");
            output.source.texture = uploadSoftware(frame);
        }
        { QMutexLocker lock(&noteMutex); output.decoderNote = decoderNote; }
        videoSink(output);
    }
    void emitAudio(QByteArray bytes, int count) {
        if (count <= 0) return;
        qint64 start = audioCursor; audioCursor += count;
        if (start < 0) {
            const int skip = int(std::min<qint64>(count, -start)); bytes.remove(0, skip * 8); start += skip; count -= skip;
        }
        if (audioEnd >= 0) count = int(std::max<qint64>(0, std::min<qint64>(count, audioEnd - start)));
        if (count > 0) { bytes.resize(count * 8); audioSink({std::move(bytes), start}); }
    }
    void drainAudio() {
        // 8. SWR尾部按实际输出样本推进；不把AAC补齐的整帧强行延长成文件时长。
        if (!swr || audioCursor < 0) return;
        for (;;) {
            QByteArray bytes(4096 * 8, '\0'); uint8_t *output[]{reinterpret_cast<uint8_t *>(bytes.data())};
            const int count = swr_convert(swr, output, 4096, nullptr, 0); check(count, "drain audio resampler");
            if (!count) break; emitAudio(std::move(bytes), count);
        }
    }
    void outputAudio(const AVFrame *frame) {
        // 5. 文件PTS使用媒体时间，采集侧AudioResampler的QPC状态不混入播放器。
        if (frame->sample_rate <= 0 || frame->sample_rate > 384000 || frame->nb_samples <= 0
            || frame->nb_samples > 192000 || frame->ch_layout.nb_channels <= 0 || frame->ch_layout.nb_channels > 32)
            throw std::runtime_error("点播音频格式/包长无效");
        const qint64 time = ptsUs(frame, audioIndex, audioInputEnd < 0 ? 0 : audioInputEnd * 1000000 / 48000);
        const qint64 start = av_rescale_q(time, {1, 1000000}, {1, 48000});
        const bool changed = !swr || frame->sample_rate != inputRate || frame->format != inputFormat
            || av_channel_layout_compare(&frame->ch_layout, &inputLayout) != 0;
        const bool jump = audioInputEnd >= 0 && std::abs(start - audioInputEnd) > 48;
        if (changed || jump) {
            drainAudio(); swr_free(&swr); av_channel_layout_uninit(&inputLayout);
            check(av_channel_layout_copy(&inputLayout, &frame->ch_layout), "decoder audio layout");
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            check(swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &inputLayout,
                AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr), "decoder resampler");
            check(swr_init(swr), "initialize decoder resampler");
            inputRate = frame->sample_rate; inputFormat = frame->format; audioCursor = start;
        }
        audioInputEnd = start + av_rescale_q(frame->nb_samples, {1, frame->sample_rate}, {1, 48000});
        const auto required = av_rescale_rnd(swr_get_delay(swr, inputRate) + frame->nb_samples, 48000, inputRate, AV_ROUND_UP);
        if (required <= 0 || required > 48000) throw std::runtime_error("点播重采样输出单包超过1秒");
        QByteArray bytes(qsizetype(required) * 8, '\0'); uint8_t *output[]{reinterpret_cast<uint8_t *>(bytes.data())};
        const int count = swr_convert(swr, output, int(required), const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
        check(count, "decode audio resample"); emitAudio(std::move(bytes), count);
        // 已到容器声明的音轨尾部就排掉SWR滤波尾，较长的视频不会让最后声音迟到。
        if (audioEnd >= 0 && audioInputEnd >= audioEnd) drainAudio();
    }
    void receive(AVCodecContext *codec, bool picture) {
        std::unique_ptr<AVFrame, FrameDelete> frame(av_frame_alloc()); if (!frame) throw std::bad_alloc();
        for (;;) {
            const int result = avcodec_receive_frame(codec, frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            check(result, "receive decoded frame");
            if (cancelled()) throw std::runtime_error("播放已停止");
            if (picture) outputVideo(frame.get()); else outputAudio(frame.get());
            av_frame_unref(frame.get());
        }
    }
    void send(AVCodecContext *codec, const AVPacket *packet, bool picture) {
        // 3. send_packet/receive_frame不是一进一出，EAGAIN先取帧，再重试当前包。
        int result = avcodec_send_packet(codec, packet);
        if (result == AVERROR(EAGAIN)) { receive(codec, picture); result = avcodec_send_packet(codec, packet); }
        if (result != AVERROR_EOF) check(result, "send decode packet"); receive(codec, picture);
    }
};
MediaDecoder::MediaDecoder() = default;
MediaDecoder::~MediaDecoder() = default;
MediaInfo MediaDecoder::open(const QString &source, std::function<bool()> cancelled, VideoSink videoSink, AudioSink audioSink, bool live, VideoDecodeMode mode)
{
    // 1. 本地文件与HTTP(S)共用输入；停止/10秒操作超时通过AVIO interrupt回调打断。
    if (m_resources || !cancelled || !videoSink || !audioSink) throw std::logic_error("解码器状态/回调无效");
    const QUrl url(source);
    const bool http = url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https");
    const bool remote = http || (live && url.scheme() == QStringLiteral("rtmp"));
    if (live && url.scheme() != QStringLiteral("rtmp")) throw std::invalid_argument("当前直播入口需要RTMP地址");
    if (remote) {
        if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) throw std::invalid_argument("在线点播需要有效的HTTP(S)媒体地址");
        static const int network = avformat_network_init(); check(network, "initialize media network");
    } else if (!QFileInfo(source).isFile()) throw std::invalid_argument("请选择存在的本地媒体文件");
    auto r = std::make_unique<Resources>(); r->cancelled = std::move(cancelled); r->live = live; r->mode = mode;
    r->videoSink = std::move(videoSink); r->audioSink = std::move(audioSink); r->deadlineUs = nowUs() + 10000000;
    r->format = avformat_alloc_context(); if (!r->format) throw std::bad_alloc();
    r->format->interrupt_callback = {Resources::interrupt, r.get()};
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", live ? "rtmp,tcp" : remote ? "http,https,tcp,tls" : "file", 0);
    av_dict_set(&options, "rw_timeout", live ? "4000000" : "10000000", 0);
    if (http) av_dict_set(&options, "tls_verify", "1", 0); // HTTPS及HTTP重定向到HTTPS均验证服务器证书。
    if (live) {
        // 1a. 直播减少探测和RTMP客户端缓冲；仍探测编码参数，不能盲目跳过解码初始化。
        r->format->probesize = 262144; r->format->max_analyze_duration = 300000;
        av_dict_set(&options, "rtmp_live", "live", 0); av_dict_set(&options, "rtmp_buffer", "100", 0);
    }
    const auto input = remote ? url.toEncoded() : QFileInfo(source).absoluteFilePath().toUtf8();
    const int opened = avformat_open_input(&r->format, input.constData(), nullptr, &options); av_dict_free(&options);
    check(opened, "open media"); check(avformat_find_stream_info(r->format, nullptr), "read media information");
    r->originUs = r->format->start_time == AV_NOPTS_VALUE ? 0 : r->format->start_time;
    MediaInfo info; info.live = live;
    info.durationUs = live || r->format->duration == AV_NOPTS_VALUE ? 0 : std::max<int64_t>(0, r->format->duration);
    // 2. 先为视频建立共用D3D11设备和硬件候选，实际get_format在首包/格式变化时由FFmpeg调用。
    auto openCodec = [&](int index) {
        const auto *stream = r->format->streams[index]; const auto *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) throw std::runtime_error("FFmpeg缺少此媒体的解码器");
        AVCodecContext *context = avcodec_alloc_context3(codec); if (!context) throw std::bad_alloc();
        try {
            check(avcodec_parameters_to_context(context, stream->codecpar), "decoder parameters");
            context->pkt_timebase = stream->time_base; context->thread_count = 2;
            if (index == r->videoIndex) {
                context->opaque = r.get(); context->get_format = Resources::chooseFormat;
                if (r->hardware) { context->hw_device_ctx = av_buffer_ref(r->hardware); if (!context->hw_device_ctx) throw std::bad_alloc(); }
            }
            check(avcodec_open2(context, codec, nullptr), "open decoder");
        } catch (...) { avcodec_free_context(&context); throw; }
        return context;
    };
    for (unsigned i = 0; i < r->format->nb_streams; ++i)
        if (r->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(r->format->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) { r->videoIndex = int(i); break; }
    r->audioIndex = av_find_best_stream(r->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (r->videoIndex >= 0) {
        r->device = playbackDevice();
        const auto *codec = avcodec_find_decoder(r->format->streams[r->videoIndex]->codecpar->codec_id);
        if (!codec) throw std::runtime_error("FFmpeg缺少此媒体的视频解码器");
        r->prepareHardware(codec);
        r->video = openCodec(r->videoIndex); info.video = true; info.width = r->video->width; info.height = r->video->height;
        const auto rate = av_guess_frame_rate(r->format, r->format->streams[r->videoIndex], nullptr);
        if (rate.num > 0 && rate.den > 0) r->defaultFrameUs = std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), {1, 1000000}));
    }
    if (r->audioIndex >= 0) {
        r->audio = openCodec(r->audioIndex); info.audio = true; const auto *stream = r->format->streams[r->audioIndex];
        const auto start = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        info.audioStartSamples = std::max<int64_t>(0, av_rescale_q(start, stream->time_base, {1, 48000}) - av_rescale_q(r->originUs, {1, 1000000}, {1, 48000}));
        if (!live && stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            r->audioEnd = av_rescale_q(start + stream->duration, stream->time_base, {1, 48000}) - av_rescale_q(r->originUs, {1, 1000000}, {1, 48000});
            info.audioEndSamples = r->audioEnd;
        }
    }
    if (!info.video && !info.audio) throw std::runtime_error("媒体中没有可播放的音视频流");
    m_resources = std::move(r); return info;
}
bool MediaDecoder::readNext()
{
    // 6. 每次读一个AVPacket，按stream_index交给对应解码器；其他流忽略，正常EOF由调用方finish。
    if (!m_resources || m_resources->finished) return false;
    auto &r = *m_resources; r.deadlineUs = nowUs() + (r.live ? 4000000 : 10000000);
    std::unique_ptr<AVPacket, PacketDelete> packet(av_packet_alloc()); if (!packet) throw std::bad_alloc();
    const int result = av_read_frame(r.format, packet.get()); if (result == AVERROR_EOF) return false;
    check(result, "read media packet");
    if (packet->stream_index == r.videoIndex) r.send(r.video, packet.get(), true);
    else if (packet->stream_index == r.audioIndex) r.send(r.audio, packet.get(), false);
    return true;
}
void MediaDecoder::finish()
{
    // 7. 正常EOF送空包取完延迟帧，再排SWR；用户stop直接析构，避免重新交付迟到帧。
    if (!m_resources || m_resources->finished) return;
    auto &r = *m_resources;
    if (r.video) r.send(r.video, nullptr, true);
    if (r.audio) { r.send(r.audio, nullptr, false); r.drainAudio(); }
    r.finished = true;
}
}
