#include "StereoLimiter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace csn {
void StereoLimiter::reset(bool clearStatistics)
{
    // 1. begin/clear清状态；没有线程或设备，丢弃待处理包即释放其PCM。
    m_pending = {}; m_gain = 1; m_level = 0;
    if (clearStatistics) m_limitedFrames = m_safetyClips = 0;
}
AudioPacket StereoLimiter::push(AudioPacket packet)
{
    // 2. 输入是已经对齐/增益相加的浮点值，可以超过1；格式与包长必须保持不变。
    if (packet.sampleRate != 48000 || packet.channels != 2 || packet.channelMask != 3 || packet.pcm.size() != Frames * 8)
        throw std::invalid_argument("限幅只接受48kHz立体声10ms浮点混音");
    const bool continuous = !m_pending.pcm.isEmpty() && packet.timestamp100ns == m_pending.timestamp100ns + 100000;
    auto output = process(continuous ? packet : AudioPacket{}); m_pending = std::move(packet); return output;
}
AudioPacket StereoLimiter::process(const AudioPacket &future)
{
    if (m_pending.pcm.isEmpty()) return {};
    std::array<float, Frames * 2> samples{};
    std::memcpy(samples.data(), m_pending.pcm.constData(), sizeof(samples));
    std::array<float, Frames + Lookahead> envelope{};
    // 3. 左右取共同峰值。反向约束让增益最多以1/240每采样的斜率下降，在峰值到来前完成攻击。
    for (int i = 0; i < int(envelope.size()); ++i) {
        float left = 0, right = 0;
        if (i < Frames) { left = samples[i * 2]; right = samples[i * 2 + 1]; }
        else if (!future.pcm.isEmpty()) {
            std::memcpy(&left, future.pcm.constData() + (i - Frames) * 8, 4);
            std::memcpy(&right, future.pcm.constData() + (i - Frames) * 8 + 4, 4);
        }
        const auto peak = std::max(std::abs(left), std::abs(right));
        envelope[i] = std::isfinite(peak) && peak > Ceiling ? Ceiling / peak : 1.0f;
    }
    for (int i = int(envelope.size()) - 2; i >= 0; --i)
        envelope[i] = std::min(envelope[i], envelope[i + 1] + 1.0f / Lookahead);
    constexpr float release = .9995834201f; // exp(-1/(48000*0.05))，50ms释放时间常数。
    double energy = 0;
    // 4. 正向释放慢慢回到1，同一增益同时作用左右；仅把最终保护clamp留作异常/舍入兜底。
    for (int i = 0; i < Frames; ++i) {
        m_gain = std::min(envelope[i], 1 - (1 - m_gain) * release);
        if (m_gain < .999999f) ++m_limitedFrames;
        for (int channel = 0; channel < 2; ++channel) {
            auto &value = samples[i * 2 + channel]; value *= m_gain;
            if (!std::isfinite(value) || std::abs(value) > 1) { ++m_safetyClips; value = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0; }
            energy += double(value) * value;
        }
    }
    m_level = float(std::sqrt(energy / samples.size()));
    AudioPacket output = m_pending;
    output.pcm = QByteArray(reinterpret_cast<const char *>(samples.data()), qsizetype(sizeof(samples)));
    return output; // PTS/不连续/估计时间标记对应原待处理包，而不是未来参考包。
}
AudioPacket StereoLimiter::drain()
{
    // 5. 未来补零只用于计算包络，不当成有效时长交付；最后一包输出一次，不漏掉尾部。
    auto output = process({}); m_pending = {}; return output;
}
}
