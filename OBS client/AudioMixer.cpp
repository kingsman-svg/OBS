#include "AudioMixer.h"
extern "C" {
#include <libavutil/mathematics.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace csn {
namespace {
constexpr int Rate = AudioResampler::OutputRate;
constexpr int MaxBuffered = Rate / 2; // 每路 500ms；GUI 停顿不能无限堆积。
constexpr qint64 Wait100ns = 800000; // 为另一端点及 swr 滤波留 80ms 对齐窗口。
int frames(const AudioPacket &packet) { return int(packet.pcm.size() / (2 * sizeof(float))); }
}

int AudioMixer::index(CaptureSource::Kind kind)
{
    if (kind == CaptureSource::Kind::Microphone) return 0;
    if (kind == CaptureSource::Kind::Loopback) return 1;
    throw std::runtime_error("混音仅支持麦克风和系统回环");
}
bool AudioMixer::active() const { return m_tracks[0].enabled || m_tracks[1].enabled; }
qint64 AudioMixer::frameAt(qint64 timestamp) const { return av_rescale(timestamp - m_origin, Rate, 10000000); }

void AudioMixer::clear()
{
    // 1. 丢弃各路滤波及缓存；2. 重置时间/统计，音量和静音仍保留。
    for (auto &track : m_tracks) {
        track.resampler.reset(); track.packets.clear(); track.queuedFrames = 0; track.enabled = false;
    }
    m_origin = m_cursor = 0; m_discontinuity = false; m_level = 0; m_clippedSamples = 0;
    m_originEstimated = false;
}
void AudioMixer::begin(bool microphone, bool system)
{
    clear();
    m_tracks[0].enabled = microphone; m_tracks[1].enabled = system;
}
void AudioMixer::setControl(CaptureSource::Kind kind, float gain, bool muted)
{
    if (!std::isfinite(gain) || gain < 0 || gain > 2) throw std::runtime_error("音频增益需要在0～2之间");
    auto &track = m_tracks[index(kind)]; track.gain = gain; track.muted = muted;
}
void AudioMixer::disable(CaptureSource::Kind kind)
{
    auto &track = m_tracks[index(kind)];
    track.enabled = false; track.resampler.reset(); track.packets.clear(); track.queuedFrames = 0;
    m_discontinuity = true;
    if (!active()) m_level = 0;
}

void AudioMixer::push(CaptureSource::Kind kind, const AudioPacket &packet)
{
    // 1. 未选择的路不参与；2. 串行转换；3. 将变长结果加入该路时间队列。
    auto &track = m_tracks[index(kind)];
    if (!track.enabled) return;
    auto converted = track.resampler.convert(packet);
    if (converted.pcm.isEmpty()) return;
    enqueue(track, std::move(converted));
}

void AudioMixer::enqueue(AudioMixTrack &track, AudioPacket packet)
{
    if (packet.pcm.isEmpty()) return;
    // 1. 首包建立公共 QPC 原点；已经输出过的迟到整包不能重放。
    if (!m_origin) { m_origin = packet.timestamp100ns; m_originEstimated = packet.timestampEstimated; }
    if (frameAt(packet.timestamp100ns) + frames(packet) <= m_cursor) return;
    // 2. 向前跳变保留跳变前有效数据；时钟倒退造成重叠时才撤掉旧队列。
    if (packet.discontinuity && !track.packets.empty()
        && frameAt(packet.timestamp100ns) < frameAt(track.packets.back().timestamp100ns) + frames(track.packets.back())) {
        track.packets.clear(); track.queuedFrames = 0; m_discontinuity = true;
    }
    track.queuedFrames += frames(packet);
    track.packets.push_back(std::move(packet));
    // 3. 缓存溢出丢旧包，下次输出标明时间不连续，内存始终有界。
    while (track.queuedFrames > MaxBuffered && track.packets.size() > 1) {
        track.queuedFrames -= frames(track.packets.front()); track.packets.pop_front(); m_discontinuity = true;
    }
}

AudioPacket AudioMixer::mixFrame()
{
    // 1. 固定 480 帧，先补零；每路按绝对样本位置求本包交集。
    std::array<float, FrameSamples * 2> mixed{};
    bool discontinuity = m_discontinuity, estimated = m_originEstimated;
    for (auto &track : m_tracks) {
        if (!track.enabled) continue;
        while (!track.packets.empty() && frameAt(track.packets.front().timestamp100ns) + frames(track.packets.front()) <= m_cursor) {
            track.queuedFrames -= frames(track.packets.front()); track.packets.pop_front();
        }
        std::array<bool, FrameSamples> covered{};
        for (const auto &packet : track.packets) {
            const qint64 first = frameAt(packet.timestamp100ns);
            if (first >= m_cursor + FrameSamples) break;
            const qint64 from = std::max(first, m_cursor), to = std::min(first + frames(packet), m_cursor + FrameSamples);
            if (to <= from) continue;
            if (packet.discontinuity && from == first) discontinuity = true;
            estimated = estimated || packet.timestampEstimated;
            for (qint64 position = from; position < to; ++position) {
                const int output = int(position - m_cursor);
                if (covered[output]) continue; // 不把重复/重叠包叠加成额外音量。
                covered[output] = true;
                if (track.muted) continue;
                for (int channel = 0; channel < 2; ++channel) {
                    float value;
                    const auto offset = ((position - first) * 2 + channel) * qsizetype(sizeof(float));
                    std::memcpy(&value, packet.pcm.constData() + offset, sizeof(value));
                    if (std::isfinite(value)) mixed[output * 2 + channel] += std::clamp(value, -1.0f, 1.0f) * track.gain;
                }
            }
        }
        // 2. 活跃且未静音的路缺包才标记；静音也按时消费，不攒旧声音。
        if (!track.muted && track.gain > 0 && std::find(covered.begin(), covered.end(), false) != covered.end())
            discontinuity = true;
    }
    // 3. 相加后统一削波到 [-1,1] 并统计，再计算实际输出 RMS。
    double energy = 0;
    for (auto &value : mixed) {
        if (value < -1 || value > 1) ++m_clippedSamples;
        value = std::clamp(value, -1.0f, 1.0f); energy += double(value) * value;
    }
    m_level = float(std::sqrt(energy / mixed.size()));
    AudioPacket output;
    output.pcm = QByteArray(reinterpret_cast<const char *>(mixed.data()), qsizetype(sizeof(mixed)));
    output.sampleRate = Rate; output.channels = 2; output.channelMask = 3;
    output.timestamp100ns = m_origin + av_rescale(m_cursor, 10000000, Rate);
    output.discontinuity = discontinuity; output.timestampEstimated = estimated;
    m_cursor += FrameSamples; m_discontinuity = false;
    return output;
}

QList<AudioPacket> AudioMixer::takeFrames(qint64 now100ns)
{
    QList<AudioPacket> output;
    if (!active() || !m_origin || now100ns <= m_origin + Wait100ns) return output;
    // 1. 只输出已过去 80ms 的数据；GUI 定时器抖动不会改变包长或采样时钟。
    const qint64 ready = av_rescale_rnd(now100ns - m_origin - Wait100ns, Rate, 10000000, AV_ROUND_DOWN);
    // 2. 一次最多追赶 200ms，过大的积压跳过，防止恢复 GUI 时处理历史长队列。
    if (ready - m_cursor > FrameSamples * 20) {
        m_cursor = (ready / FrameSamples - 20) * FrameSamples; m_discontinuity = true;
    }
    while (m_cursor + FrameSamples <= ready) output.append(mixFrame());
    return output;
}

QList<AudioPacket> AudioMixer::finish()
{
    QList<AudioPacket> output;
    // 1. 两路都停止后排空 swr 尾部，随后确定最后一份有效数据的位置。
    for (auto &track : m_tracks) if (track.enabled) enqueue(track, track.resampler.drain());
    qint64 end = m_cursor;
    for (const auto &track : m_tracks) if (!track.packets.empty())
        end = std::max(end, frameAt(track.packets.back().timestamp100ns) + frames(track.packets.back()));
    // 2. 停止排空也限制 500ms；最后不足 480 帧由 mixFrame 自然补零并标记。
    if (end - m_cursor > MaxBuffered) {
        m_cursor = ((end - MaxBuffered + FrameSamples - 1) / FrameSamples) * FrameSamples;
        m_discontinuity = true;
    }
    while (active() && m_origin && m_cursor < end) output.append(mixFrame());
    // 3. 输出已拥有字节数据，清缓存和启用状态；保留本轮统计供界面查看。
    for (auto &track : m_tracks) { track.enabled = false; track.packets.clear(); track.queuedFrames = 0; }
    m_level = 0;
    return output;
}
} // namespace csn
