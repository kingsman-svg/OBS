#include "AudioResampler.h"
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
// 将 FFmpeg 负错误码转成可读异常；业务层统一转为界面提示。
void checkAudio(int result)
{
    if (result >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    throw std::runtime_error(message);
}
}
namespace csn {
AudioResampler::~AudioResampler() { reset(); }

void AudioResampler::reset()
{
    // 1. 先释放滤波器；2. 清除时间/格式，保证下一段不使用旧延迟。
    swr_free(&m_context);
    m_inputRate = m_inputChannels = 0; m_inputMask = 0;
    m_origin = m_inputFrames = m_outputFrames = m_lastTimestamp = 0;
    m_discontinuity = m_estimated = false;
}

void AudioResampler::configure(const AudioPacket &input)
{
    // 1. 只在布局明确时混合多声道；不能按声道数猜测 5.1/7.1 的扬声器顺序。
    AVChannelLayout source{}, target = AV_CHANNEL_LAYOUT_STEREO;
    if (input.channelMask) checkAudio(av_channel_layout_from_mask(&source, input.channelMask));
    else if (input.channels <= 2) av_channel_layout_default(&source, input.channels);
    else throw std::runtime_error("多声道音频缺少扬声器布局，无法安全下混");
    if (source.nb_channels != input.channels) {
        av_channel_layout_uninit(&source);
        throw std::runtime_error("音频声道数与扬声器布局不一致");
    }
    // 2. 输入/输出均为交错 Float32，swr 负责滤波重采样和声道变换。
    const int result = swr_alloc_set_opts2(&m_context, &target, AV_SAMPLE_FMT_FLT, OutputRate,
        &source, AV_SAMPLE_FMT_FLT, input.sampleRate, 0, nullptr);
    av_channel_layout_uninit(&source);
    checkAudio(result);
    // 3. 麦克风单声道复制到左右声道，保留原电平；多声道沿用 FFmpeg 下混矩阵。
    if (input.channels == 1) {
        const double duplicate[]{1, 1};
        checkAudio(swr_set_matrix(m_context, duplicate, 1));
    }
    checkAudio(swr_init(m_context));
    m_inputRate = input.sampleRate; m_inputChannels = input.channels; m_inputMask = input.channelMask;
    m_origin = input.timestamp100ns;
}

AudioPacket AudioResampler::convert(const AudioPacket &input)
{
    // 1. 校验 Float32 包布局及边界；空数据不启动时钟，坏输入不能进入 swr。
    if (input.sampleRate < 8000 || input.sampleRate > 192000 || input.channels < 1 || input.channels > 32
        || input.timestamp100ns <= 0 || input.timestamp100ns > std::numeric_limits<qint64>::max() - 10000000
        || input.pcm.size() % (input.channels * qsizetype(sizeof(float))))
        throw std::runtime_error("音频包采样率、声道、时间戳或 Float32 布局无效");
    const auto frames = input.pcm.size() / (input.channels * qsizetype(sizeof(float)));
    if (!frames) return {};
    if (frames > input.sampleRate / 2) throw std::runtime_error("单包音频超过 500ms 上限");
    const bool changed = m_inputRate != input.sampleRate || m_inputChannels != input.channels || m_inputMask != input.channelMask;
    const qint64 expected = m_context ? m_origin + av_rescale(m_inputFrames, 10000000, m_inputRate) : input.timestamp100ns;
    const qint64 difference = input.timestamp100ns - expected;
    // 2. 重复/倒序包不推进滤波器；5ms 内的包时间抖动吸收到连续样本时钟。
    if (m_context && !changed && !input.discontinuity
        && (difference < -50000 || input.timestamp100ns <= m_lastTimestamp)) return {};
    const bool broken = input.discontinuity || (m_context && (changed || std::abs(difference) > 50000));
    if (!m_context || changed || broken) {
        reset();
        try { configure(input); }
        catch (...) { reset(); throw; }
        m_discontinuity = broken;
    }
    // 3. 累计输入帧数，转换后按实际输出样本数标记时间，绝不按 Qt 定时器计时。
    m_estimated = m_estimated || input.timestampEstimated;
    auto result = resample(reinterpret_cast<const uchar *>(input.pcm.constData()), int(frames));
    m_inputFrames += frames;
    m_lastTimestamp = input.timestamp100ns;
    return result;
}

AudioPacket AudioResampler::resample(const uchar *data, int frames)
{
    // 1. 上界包含滤波延迟；有足够输出容量，避免把正常音频滞留在 swr 中。
    const int capacity = swr_get_out_samples(m_context, frames);
    checkAudio(capacity);
    if (capacity > OutputRate) throw std::runtime_error("重采样缓冲超过一秒上限");
    AudioPacket output;
    output.pcm.resize(qsizetype(capacity) * 2 * sizeof(float));
    auto *destination = reinterpret_cast<uint8_t *>(output.pcm.data());
    const uint8_t *source = data;
    // 2. data=nullptr 是排空：让 swr 输出仍需未来样本的滤波尾部。
    const int produced = swr_convert(m_context, &destination, capacity, data ? &source : nullptr, frames);
    checkAudio(produced);
    output.pcm.resize(qsizetype(produced) * 2 * sizeof(float));
    if (!produced) return {};
    // 3. 时间戳指向首个输出样本，下一包沿同一连续时间轴前进。
    output.sampleRate = OutputRate; output.channels = 2; output.channelMask = AV_CH_LAYOUT_STEREO;
    output.timestamp100ns = m_origin + av_rescale(m_outputFrames, 10000000, OutputRate);
    output.discontinuity = m_discontinuity; output.timestampEstimated = m_estimated;
    m_outputFrames += produced; m_discontinuity = false;
    return output;
}

AudioPacket AudioResampler::drain()
{
    if (!m_context) return {};
    // 1. 输出尾部；2. 释放上下文，重复 drain 不会重复输出同一段。
    try { auto output = resample(nullptr, 0); reset(); return output; }
    catch (...) { reset(); throw; }
}
} // namespace csn
