#include "MediaEncoder.h"
#include "GpuVideoOutput.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace csn {
namespace {
void check(int result, const char *operation)
{
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
struct FrameDelete { void operator()(AVFrame *frame) const { av_frame_free(&frame); } };
struct PacketDelete { void operator()(AVPacket *packet) const { av_packet_free(&packet); } };
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
Frame frame()
{
    Frame result(av_frame_alloc());
    if (!result) throw std::bad_alloc(); return result;
}
}

struct MediaEncoder::Resources {
    RecordingSettings settings;              // 本轮固定编码参数，运行中不修改。
    AVCodecContext *video = nullptr;          // NVENC H.264，硬件D3D11帧输入。
    AVCodecContext *audio = nullptr;          // AAC，48kHz平面Float32输入。
    AVBufferRef *device = nullptr;            // FFmpeg设备引用，持有采集D3D11设备的AddRef。
    AVBufferRef *frames = nullptr;            // BGRA硬件帧池，AVFrame引用控制纹理复用。
    AVAudioFifo *fifo = nullptr;              // 将480样本包组合成AAC要求的帧长。
    std::unique_ptr<GpuVideoOutput> gpu;       // 编码输出Shader，无SwapChain、不访问GUI。
    PacketSink sink;                          // 同一工作线程同步交给封装层，禁止保留裸包指针。
    EncodingStats stats;                      // 只在编码工作线程写入。
    qint64 lastVideoPts = -1;                 // 拒绝重复/倒序视频PTS，跳变保持原值。
    qint64 audioInputEnd = -1;                // 下一个输入样本位置，用于缺口补零和重叠校验。
    qint64 audioCursor = 0;                   // FIFO头部对应的样本PTS。
    bool finished = false;                   // finish之后拒绝新输入，重复finish不输出。

    ~Resources()
    {
        // 8. 编码上下文先释放其硬件帧引用，再释放池与设备；失败初始化同样走此路径。
        avcodec_free_context(&video); avcodec_free_context(&audio);
        if (fifo) av_audio_fifo_free(fifo);
        gpu.reset(); av_buffer_unref(&frames); av_buffer_unref(&device);
    }
    void receive(AVCodecContext *codec)
    {
        Packet packet(av_packet_alloc()); if (!packet) throw std::bad_alloc();
        for (;;) {
            const int result = avcodec_receive_packet(codec, packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            check(result, "receive_packet");
            // FFmpeg 7.1 NVENC可能不回传AVFrame.duration；固定30fps必须补上最后一帧的时长。
            if (codec->codec_type == AVMEDIA_TYPE_VIDEO && packet->duration <= 0) packet->duration = 1;
            const auto size = quint64(packet->size);
            sink(packet.get(), codec); // 封装层可能清空packet，所以先保存字节数。
            ++stats.packets; stats.bytes += size; av_packet_unref(packet.get());
        }
    }
    void send(AVCodecContext *codec, AVFrame *input)
    {
        // 5. send/receive并非一进一出；先释放积压输出，再继续提交，停止使用空帧排空。
        int result = avcodec_send_frame(codec, input);
        if (result == AVERROR(EAGAIN)) { receive(codec); result = avcodec_send_frame(codec, input); }
        if (result != AVERROR_EOF) check(result, "send_frame");
        receive(codec);
    }
    void encodeAudio(bool tail)
    {
        while (av_audio_fifo_size(fifo) >= audio->frame_size || (tail && av_audio_fifo_size(fifo) > 0)) {
            const int count = std::min(av_audio_fifo_size(fifo), audio->frame_size);
            auto input = frame(); input->format = audio->sample_fmt; input->sample_rate = audio->sample_rate;
            check(av_channel_layout_copy(&input->ch_layout, &audio->ch_layout), "audio layout");
            // AAC支持SMALL_LAST_FRAME，最后不足1024只提交实际样本，避免把停止时间人为延长。
            input->nb_samples = count; input->pts = audioCursor;
            check(av_frame_get_buffer(input.get(), 0), "audio frame buffer");
            if (av_audio_fifo_read(fifo, reinterpret_cast<void **>(input->extended_data), count) != count)
                throw std::runtime_error("AAC音频FIFO读取不足");
            send(audio, input.get()); audioCursor += count;
        }
    }
    void append(const float *left, const float *right, int count)
    {
        check(av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + count), "audio fifo realloc");
        void *planes[]{const_cast<float *>(left), const_cast<float *>(right)};
        if (av_audio_fifo_write(fifo, planes, count) != count) throw std::runtime_error("AAC音频FIFO写入不足");
        encodeAudio(false);
    }
};

MediaEncoder::MediaEncoder() = default;
MediaEncoder::~MediaEncoder() = default;

void MediaEncoder::open(const RecordingSettings &settings, ID3D11Device *sourceDevice, bool globalHeader, PacketSink sink)
{
    // 1. 普通类不创建线程；Publisher::run调用open，所有FFmpeg上下文都只在该线程使用。
    if (m_resources) throw std::logic_error("编码器已初始化");
    if ((!settings.video && !settings.audio) || !sink || settings.width < 64 || settings.height < 64
        || settings.width > 3840 || settings.height > 2160 || settings.width % 2 || settings.height % 2
        || settings.videoBitrate < 100000 || settings.audioBitrate < 32000)
        throw std::invalid_argument("编码参数无效");
    auto resources = std::make_unique<Resources>(); resources->settings = settings; resources->sink = std::move(sink);
    if (settings.video) {
        // 2. 在同一D3D11设备上建立BGRA硬件帧池；NVENC内部将RGB转换为YUV420。
        if (!sourceDevice) throw std::invalid_argument("视频编码缺少首帧设备");
        resources->device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!resources->device) throw std::bad_alloc();
        auto *device = reinterpret_cast<AVHWDeviceContext *>(resources->device->data);
        auto *d3d = static_cast<AVD3D11VADeviceContext *>(device->hwctx);
        sourceDevice->AddRef(); d3d->device = sourceDevice;
        check(av_hwdevice_ctx_init(resources->device), "D3D11 device context");
        resources->frames = av_hwframe_ctx_alloc(resources->device);
        if (!resources->frames) throw std::bad_alloc();
        auto *pool = reinterpret_cast<AVHWFramesContext *>(resources->frames->data);
        pool->format = AV_PIX_FMT_D3D11; pool->sw_format = AV_PIX_FMT_BGRA;
        pool->width = settings.width; pool->height = settings.height; pool->initial_pool_size = 0;
        auto *d3dFrames = static_cast<AVD3D11VAFramesContext *>(pool->hwctx);
        d3dFrames->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(av_hwframe_ctx_init(resources->frames), "D3D11 frame pool");
        resources->gpu = std::make_unique<GpuVideoOutput>(sourceDevice);
        // 3. 首期固定30fps、无B帧、关闭lookahead；真实低延迟还需后续传输/播放测量。
        const auto *codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) throw std::runtime_error("FFmpeg缺少h264_nvenc编码器");
        auto *video = resources->video = avcodec_alloc_context3(codec);
        if (!video) throw std::bad_alloc();
        video->width = settings.width; video->height = settings.height;
        video->pix_fmt = AV_PIX_FMT_D3D11; video->time_base = {1, MediaTimeline::VideoRate};
        video->framerate = {MediaTimeline::VideoRate, 1}; video->bit_rate = settings.videoBitrate;
        video->gop_size = MediaTimeline::VideoRate * 2; video->max_b_frames = 0;
        video->sample_aspect_ratio = {1, 1}; video->color_range = AVCOL_RANGE_MPEG;
        video->colorspace = AVCOL_SPC_BT470BG; video->color_primaries = AVCOL_PRI_BT709; video->color_trc = AVCOL_TRC_BT709;
        video->hw_frames_ctx = av_buffer_ref(resources->frames);
        if (!video->hw_frames_ctx) throw std::bad_alloc();
        if (globalHeader) video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set(video->priv_data, "preset", "p4", 0), "NVENC preset");
        check(av_opt_set(video->priv_data, "tune", "ll", 0), "NVENC tune");
        check(av_opt_set(video->priv_data, "rc", "cbr", 0), "NVENC bitrate mode");
        check(av_opt_set(video->priv_data, "rgb_mode", "yuv420", 0), "NVENC RGB mode");
        check(av_opt_set_int(video->priv_data, "rc-lookahead", 0, 0), "NVENC lookahead");
        check(av_opt_set_int(video->priv_data, "zerolatency", 1, 0), "NVENC reorder");
        check(av_opt_set_int(video->priv_data, "delay", 0, 0), "NVENC output delay");
        check(avcodec_open2(video, codec, nullptr), "open NVENC");
    }
    if (settings.audio) {
        // 4. 混音已经48kHz，只将交错Float32变成AAC的平面Float32，不再次做采样率转换。
        const auto *codec = avcodec_find_encoder_by_name("aac");
        if (!codec || !(codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME))
            throw std::runtime_error("FFmpeg缺少支持音频尾部的AAC编码器");
        auto *audio = resources->audio = avcodec_alloc_context3(codec);
        if (!audio) throw std::bad_alloc();
        audio->sample_rate = MediaTimeline::AudioRate; audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio->time_base = {1, MediaTimeline::AudioRate}; audio->bit_rate = settings.audioBitrate;
        av_channel_layout_default(&audio->ch_layout, 2);
        if (globalHeader) audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(audio, codec, nullptr), "open AAC");
        if (audio->frame_size <= 0 || audio->frame_size > 4096) throw std::runtime_error("AAC帧长无效");
        resources->fifo = av_audio_fifo_alloc(audio->sample_fmt, 2, audio->frame_size);
        if (!resources->fifo) throw std::bad_alloc();
    }
    m_resources = std::move(resources);
}

void MediaEncoder::pushVideo(const TimedVideoFrame &timed)
{
    if (!m_resources || m_resources->finished || !m_resources->video) throw std::logic_error("视频编码器未运行");
    auto &r = *m_resources;
    if (!timed.source.video.texture || timed.pts < 0 || timed.pts <= r.lastVideoPts)
        throw std::invalid_argument("视频纹理或PTS无效/倒序");
    auto input = frame(); check(av_hwframe_get_buffer(r.frames, input.get(), 0), "hardware video frame");
    r.gpu->render(timed.source, reinterpret_cast<ID3D11Texture2D *>(input->data[0]));
    input->pts = timed.pts; input->duration = 1;
    input->color_range = r.video->color_range; input->colorspace = r.video->colorspace;
    input->color_primaries = r.video->color_primaries; input->color_trc = r.video->color_trc;
    // 跳过帧后请求关键帧；PTS仍保留原间隔，不把视频时间压短。
    if (timed.discontinuity) input->pict_type = AV_PICTURE_TYPE_I;
    r.send(r.video, input.get()); r.lastVideoPts = timed.pts; ++r.stats.videoFrames;
}

void MediaEncoder::pushAudio(const TimedAudioPacket &timed)
{
    // 6. 音频按PTS连续写FIFO，时间缺口补静音；过大的缺口明确失败，避免无界追赶。
    if (!m_resources || m_resources->finished || !m_resources->audio) throw std::logic_error("音频编码器未运行");
    auto &r = *m_resources; const auto &packet = timed.source;
    if (packet.sampleRate != 48000 || packet.channels != 2 || packet.channelMask != 3 || packet.pcm.size() != 480 * 8
        || timed.pts < 0 || timed.pts > std::numeric_limits<qint64>::max() - 480)
        throw std::invalid_argument("AAC输入必须是同步的48kHz立体声10ms包");
    if (r.audioInputEnd < 0) r.audioCursor = r.audioInputEnd = timed.pts;
    if (timed.pts < r.audioInputEnd) throw std::invalid_argument("AAC输入PTS重叠/倒序");
    qint64 gap = timed.pts - r.audioInputEnd;
    if (gap > 48000 * 2) throw std::runtime_error("音频时间缺口超过2秒，请停止后重新开始录制");
    std::array<float, 1024> zero{};
    while (gap > 0) {
        const int count = int(std::min<qint64>(gap, zero.size())); r.append(zero.data(), zero.data(), count);
        r.stats.silentSamples += quint64(count); gap -= count;
    }
    std::array<float, 480> left{}, right{};
    for (int i = 0; i < 480; ++i) {
        std::memcpy(&left[i], packet.pcm.constData() + i * 8, 4);
        std::memcpy(&right[i], packet.pcm.constData() + i * 8 + 4, 4);
        if (!std::isfinite(left[i]) || !std::isfinite(right[i])) throw std::invalid_argument("AAC输入含非有限样本");
    }
    r.append(left.data(), right.data(), 480); r.audioInputEnd = timed.pts + 480; r.stats.audioSamples += 480;
}

void MediaEncoder::finish()
{
    // 7. 先送出FIFO实际尾部，再分别发送空帧；封装层之后才能写trailer。
    if (!m_resources || m_resources->finished) return;
    auto &r = *m_resources;
    if (r.audio) { r.encodeAudio(true); r.send(r.audio, nullptr); }
    if (r.video) r.send(r.video, nullptr);
    r.finished = true;
}
const AVCodecContext *MediaEncoder::videoContext() const { return m_resources ? m_resources->video : nullptr; }
const AVCodecContext *MediaEncoder::audioContext() const { return m_resources ? m_resources->audio : nullptr; }
const EncodingStats &MediaEncoder::stats() const
{
    if (!m_resources) throw std::logic_error("编码器未初始化"); return m_resources->stats;
}
}
