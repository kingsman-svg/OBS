#include "AudioTranscoder.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace obs::media {
namespace {
constexpr AVRational AudioTime{1, 48000};
void check(int result) {
    if (result < 0)
        throw std::runtime_error("audio transcoding failed");
}
struct FrameDelete {
    void operator()(AVFrame *p) const {
        av_frame_free(&p);
    }
};
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
Frame frame() {
    auto *p = av_frame_alloc();
    if (!p)
        throw std::bad_alloc();
    return Frame(p);
}
} // namespace
struct AudioTranscoder::Resources {
    AVCodecContext *decoder = nullptr, *encoder = nullptr; // AAC 输入、libopus 输出，均不跨线程。
    SwrContext *swr = nullptr;   // 任意输入格式/采样率 → 48kHz stereo Float32。
    AVAudioFifo *fifo = nullptr; // 只存一个连续时间段，不跨缺口补大段静音。
    AVChannelLayout layout{};    // 当前 SWR 输入布局，不能只比较声道数。
    AVRational timeBase{};
    AVSampleFormat format = AV_SAMPLE_FMT_NONE;
    int rate = 0;
    std::int64_t inputOrigin = 0, inputSamples = 0; // 源 PTS 原点和本段实际解码样本数。
    std::int64_t fifoPts = 0; // FIFO 首个样本的绝对 48kHz 媒体时间。
    bool initialized = false, finished = false, changed = false;
    AudioTranscodeStats stats;
    ~Resources() {
        avcodec_free_context(&decoder);
        avcodec_free_context(&encoder);
        swr_free(&swr);
        av_audio_fifo_free(fifo);
        av_channel_layout_uninit(&layout);
    }
    void beginSegment(const AVFrame &input, std::int64_t pts) {
        // 2. 格式/真实时间跳变重建整个输出段，丢弃旧 FIFO/编码历史，不能接着编码错误时间。
        if (initialized) {
            ++stats.resets;
            changed = true;
        }
        swr_free(&swr);
        avcodec_free_context(&encoder);
        av_channel_layout_uninit(&layout);
        av_audio_fifo_reset(fifo);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &input.ch_layout,
                                  static_cast<AVSampleFormat>(input.format), input.sample_rate, 0,
                                  nullptr));
        check(swr_init(swr));
        check(av_channel_layout_copy(&layout, &input.ch_layout));
        format = static_cast<AVSampleFormat>(input.format);
        rate = input.sample_rate;
        const auto *codec = avcodec_find_encoder_by_name("libopus");
        if (!codec)
            throw std::runtime_error("FFmpeg libopus encoder unavailable");
        encoder = avcodec_alloc_context3(codec);
        if (!encoder)
            throw std::bad_alloc();
        encoder->sample_rate = 48000;
        encoder->sample_fmt = AV_SAMPLE_FMT_FLT;
        encoder->time_base = AudioTime;
        encoder->bit_rate = 96000;
        check(av_channel_layout_copy(&encoder->ch_layout, &stereo));
        check(av_opt_set(encoder->priv_data, "application", "lowdelay", 0));
        check(av_opt_set(encoder->priv_data, "frame_duration", "20", 0));
        check(avcodec_open2(encoder, codec, nullptr));
        if (encoder->frame_size != 960)
            throw std::runtime_error("20ms Opus frames required");
        inputOrigin = pts;
        inputSamples = 0;
        fifoPts = av_rescale_q(pts, timeBase, AudioTime);
        stats.encoderDelaySamples = encoder->initial_padding;
        stats.inputRate = rate;
        initialized = true;
    }
    void collect(std::vector<MediaUnit> &out, std::uint64_t epoch) {
        auto *packet = av_packet_alloc();
        if (!packet)
            throw std::bad_alloc();
        try {
            for (;;) {
                const int result = avcodec_receive_packet(encoder, packet);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                    break;
                check(result);
                if (packet->pts == AV_NOPTS_VALUE || packet->size <= 0 || packet->size > 1200)
                    throw std::runtime_error("invalid Opus packet");
                auto bytes = std::make_shared<std::vector<std::byte>>(
                    reinterpret_cast<std::byte *>(packet->data),
                    reinterpret_cast<std::byte *>(packet->data + packet->size));
                // 4. 使用编码器返回的 PTS（已经减去 lookahead），不再另加/另减 initial_padding。
                out.push_back({bytes, av_rescale_q(packet->pts, AudioTime, AVRational{1, 1000000}),
                               epoch, false, MediaKind::Audio, 20000, changed});
                changed = false;
                ++stats.packets;
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
        } catch (...) {
            av_packet_free(&packet);
            throw;
        }
    }
    void encode(std::vector<MediaUnit> &out, std::uint64_t epoch, bool tail = false) {
        while (av_audio_fifo_size(fifo) >= 960 || (tail && av_audio_fifo_size(fifo) > 0)) {
            const int samples = std::min(960, av_audio_fifo_size(fifo));
            auto input = frame();
            input->format = AV_SAMPLE_FMT_FLT;
            input->sample_rate = 48000;
            check(av_channel_layout_copy(&input->ch_layout, &encoder->ch_layout));
            input->nb_samples = samples;
            input->pts = fifoPts;
            check(av_frame_get_buffer(input.get(), 0));
            if (av_audio_fifo_read(fifo, reinterpret_cast<void **>(input->data), samples) !=
                samples)
                throw std::runtime_error("audio FIFO underflow");
            fifoPts += samples;
            if (samples < 960)
                stats.tailInputSamples = samples;
            check(avcodec_send_frame(encoder, input.get()));
            collect(out, epoch);
        }
    }
    void append(float *pcm, int samples, std::vector<MediaUnit> &out, std::uint64_t epoch) {
        if (samples <= 0)
            return;
        if (av_audio_fifo_size(fifo) + samples > 48000)
            throw std::runtime_error("audio FIFO limit");
        void *planes[]{pcm};
        if (av_audio_fifo_write(fifo, planes, samples) != samples)
            throw std::runtime_error("audio FIFO write");
        stats.resampledSamples += samples;
        encode(out, epoch);
    }
    void convert(const AVFrame &input, std::vector<MediaUnit> &out, std::uint64_t epoch) {
        const auto pts =
            input.best_effort_timestamp != AV_NOPTS_VALUE ? input.best_effort_timestamp : input.pts;
        if (pts == AV_NOPTS_VALUE || input.sample_rate <= 0 || input.nb_samples <= 0 ||
            input.nb_samples > 8192 || input.ch_layout.nb_channels < 1 ||
            input.ch_layout.nb_channels > 8)
            throw std::invalid_argument("AAC frame properties unsupported");
        const auto expectedUs =
            initialized
                ? av_rescale_q(inputOrigin, timeBase, AVRational{1, 1000000}) +
                      av_rescale_q(inputSamples, AVRational{1, rate}, AVRational{1, 1000000})
                : 0;
        const auto ptsUs = av_rescale_q(pts, timeBase, AVRational{1, 1000000});
        if (!initialized || rate != input.sample_rate || format != input.format ||
            av_channel_layout_compare(&layout, &input.ch_layout) ||
            std::llabs(ptsUs - expectedUs) > 5000)
            beginSegment(input, pts);
        // 3. PTS 由本段首个样本和实际输出数推进；滤波延迟只影响交付时机，不改变媒体时间。
        const auto capacity =
            av_rescale_rnd(swr_get_delay(swr, rate) + input.nb_samples, 48000, rate, AV_ROUND_UP);
        if (capacity < 1 || capacity > 48000)
            throw std::runtime_error("resampler output limit");
        std::vector<float> pcm(static_cast<std::size_t>(capacity) * 2);
        auto *data = reinterpret_cast<std::uint8_t *>(pcm.data());
        const int samples =
            swr_convert(swr, &data, static_cast<int>(capacity),
                        const_cast<const std::uint8_t **>(input.extended_data), input.nb_samples);
        check(samples);
        inputSamples += input.nb_samples;
        stats.decodedSamples += input.nb_samples;
        append(pcm.data(), samples, out, epoch);
    }
    void receive(std::vector<MediaUnit> &out, std::uint64_t epoch) {
        auto decoded = frame();
        for (;;) {
            const int result = avcodec_receive_frame(decoder, decoded.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                break;
            check(result);
            convert(*decoded, out, epoch);
            av_frame_unref(decoded.get());
        }
    }
};
AudioTranscoder::AudioTranscoder(const AVCodecParameters &parameters, AVRational timeBase)
    : m_resources(std::make_unique<Resources>()) {
    // 1. 读取线程建立 AAC 解码；输出等真实首帧确定格式后初始化，不凭输入声明猜样本布局。
    if (parameters.codec_id != AV_CODEC_ID_AAC || timeBase.num <= 0 || timeBase.den <= 0)
        throw std::invalid_argument("AAC input required");
    auto &r = *m_resources;
    r.timeBase = timeBase;
    const auto *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec)
        throw std::runtime_error("AAC decoder unavailable");
    r.decoder = avcodec_alloc_context3(codec);
    if (!r.decoder)
        throw std::bad_alloc();
    check(avcodec_parameters_to_context(r.decoder, &parameters));
    r.decoder->pkt_timebase = timeBase;
    r.decoder->thread_count = 1;
    check(avcodec_open2(r.decoder, codec, nullptr));
    r.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, 1920);
    if (!r.fifo)
        throw std::bad_alloc();
}
AudioTranscoder::~AudioTranscoder() = default;
std::vector<MediaUnit> AudioTranscoder::push(const AVPacket &packet, std::uint64_t epoch) {
    auto &r = *m_resources;
    if (r.finished || packet.pts == AV_NOPTS_VALUE)
        throw std::invalid_argument("audio input not running or missing PTS");
    std::vector<MediaUnit> out;
    const int result = avcodec_send_packet(r.decoder, &packet);
    if (result == AVERROR(EAGAIN)) {
        r.receive(out, epoch);
        check(avcodec_send_packet(r.decoder, &packet));
    } else
        check(result);
    r.receive(out, epoch);
    return out;
}
std::vector<MediaUnit> AudioTranscoder::finish(std::uint64_t epoch) {
    // 5. 仅正常 EOF 排 decoder→SWR→FIFO→encoder；最后的 Opus 填充不宣称是有效源样本。
    auto &r = *m_resources;
    std::vector<MediaUnit> out;
    if (r.finished)
        return out;
    check(avcodec_send_packet(r.decoder, nullptr));
    r.receive(out, epoch);
    if (r.initialized) {
        for (;;) {
            std::vector<float> pcm(4096 * 2);
            auto *data = reinterpret_cast<std::uint8_t *>(pcm.data());
            const int count = swr_convert(r.swr, &data, 4096, nullptr, 0);
            check(count);
            if (!count)
                break;
            r.append(pcm.data(), count, out, epoch);
        }
        r.encode(out, epoch, true);
        check(avcodec_send_frame(r.encoder, nullptr));
        r.collect(out, epoch);
    }
    r.finished = true;
    return out;
}
AudioTranscodeStats AudioTranscoder::stats() const {
    return m_resources->stats;
}
} // namespace obs::media
