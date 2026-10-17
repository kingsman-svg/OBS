#include "AudioResampler.h"
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}
#include <algorithm>
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
    m_lastFrames = 0; m_lastEstimated = m_clockValid = m_rateObserved = false;
    m_rateTime = m_rateFrames = m_phaseTime = m_phaseOutput = 0;
    m_clockPpm = m_commandPpm = 0; m_drift = {};
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
    // 4. 同为48kHz也先建立滤波器；后续set_compensation不必重建/丢失已有滤波库存。
    checkAudio(av_opt_set_int(m_context, "flags", SWR_FLAG_RESAMPLE, 0));
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
    const qint64 expected = m_context ? m_lastTimestamp + av_rescale(m_lastFrames, 10000000, m_inputRate) : input.timestamp100ns;
    // 2. 仅比较相邻包的间隔。缓慢累计超过5ms仍交给软补偿，不能再按会话累计偏差丢包/重建。
    if (m_context && !changed && !input.discontinuity && input.timestamp100ns <= m_lastTimestamp) { ++m_drift.rejected; return {}; }
    const bool jump = m_context && !input.timestampEstimated && !m_lastEstimated && std::abs(input.timestamp100ns - expected) > 50000;
    const bool broken = input.discontinuity || (m_context && (changed || jump));
    if (!m_context || changed || broken) {
        auto statistics = m_drift;
        if (m_context) ++statistics.restarts;
        reset();
        m_drift = statistics;
        try { configure(input); }
        catch (...) { reset(); throw; }
        m_discontinuity = broken;
    }
    // 3. 累计输入帧数，转换后按实际输出样本数标记时间，绝不按 Qt 定时器计时。
    compensate(input);
    m_estimated = m_estimated || input.timestampEstimated;
    auto result = resample(reinterpret_cast<const uchar *>(input.pcm.constData()), int(frames));
    m_inputFrames += frames;
    m_lastTimestamp = input.timestamp100ns;
    m_lastFrames = int(frames); m_lastEstimated = input.timestampEstimated;
    return result;
}

void AudioResampler::compensate(const AudioPacket &input)
{
    // 1. 只采用设备给出的QPC首帧时间。估计时间暂停软补偿，下一可信段重新建基线而不跳输出PTS。
    if (input.timestampEstimated) {
        if (m_commandPpm != 0) checkAudio(swr_set_compensation(m_context, 0, 0));
        m_clockValid = m_rateObserved = false; m_clockPpm = m_commandPpm = 0;
        m_drift.reliable = false; m_drift.clockPpm = m_drift.correctionPpm = m_drift.phaseErrorMs = 0; return;
    }
    const auto inventory = m_outputFrames + swr_get_delay(m_context, OutputRate);
    if (!m_clockValid) {
        m_rateTime = m_phaseTime = input.timestamp100ns; m_rateFrames = m_inputFrames; m_phaseOutput = inventory;
        m_clockValid = true; m_drift.reliable = true; return;
    }
    const auto nominal = av_rescale(m_inputFrames - m_rateFrames, 10000000, m_inputRate);
    if (nominal < 10000000) return; // 一秒窗口过滤包级调度/时间戳噪声。
    const double measured = double(input.timestamp100ns - m_rateTime - nominal) * 1000000 / nominal;
    m_rateTime = input.timestamp100ns; m_rateFrames = m_inputFrames;
    // 2. 超过2000ppm不作为正常晶振漂移学习；暂停校正/重建可信基线，不用极端重采样追赶坏时间。
    if (std::abs(measured) > 2000) {
        ++m_drift.outliers; m_clockValid = m_rateObserved = false; m_commandPpm = m_clockPpm = 0;
        checkAudio(swr_set_compensation(m_context, 0, 0)); m_drift.reliable = false;
        m_drift.clockPpm = m_drift.correctionPpm = m_drift.phaseErrorMs = 0; return;
    }
    m_clockPpm = m_rateObserved ? .8 * m_clockPpm + .2 * measured : measured; m_rateObserved = true;
    // 3. 输出库存包含swr滤波延迟，不能把那十几帧误当成采样时钟漂移。四秒慢慢收敛相位。
    const double wanted = double(input.timestamp100ns - m_phaseTime) * OutputRate / 10000000 + m_phaseOutput;
    const double error = wanted - inventory;
    const double desired = std::clamp(m_clockPpm + error * 1000000 / (OutputRate * 4.0), -1000.0, 1000.0);
    m_commandPpm += std::clamp(desired - m_commandPpm, -100.0, 100.0);
    constexpr int distance = OutputRate * 2; // 两秒补偿距离，每秒刷新；一个样本约10.4ppm。
    const int delta = int(std::llround(m_commandPpm * distance / 1000000));
    // 4. 正delta生成更多样本，适配设备偏慢；负delta相反。不丢整包、不插整段静音、不修改采集QPC。
    checkAudio(swr_set_compensation(m_context, delta, distance));
    m_drift.clockPpm = m_clockPpm; m_drift.correctionPpm = double(delta) * 1000000 / distance;
    m_drift.phaseErrorMs = error * 1000 / OutputRate; m_drift.reliable = true; ++m_drift.adjustments;
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
    try { auto output = resample(nullptr, 0); const auto statistics = m_drift; reset(); m_drift = statistics; return output; }
    catch (...) { reset(); throw; }
}
} // namespace csn
