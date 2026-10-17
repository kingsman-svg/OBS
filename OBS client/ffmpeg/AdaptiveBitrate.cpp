#include "AdaptiveBitrate.h"
#include <algorithm>
#include <stdexcept>

namespace csn {
bool AdaptiveBitrate::valid(const BitrateControl &control)
{
    return control.minimum >= MinimumVideoBitrate && control.maximum <= MaximumVideoBitrate
        && control.minimum <= control.maximum;
}
void AdaptiveBitrate::begin(const BitrateControl &control, int current, qint64 now, const BitrateFeedback &feedback)
{
    // 1. 新会话独立计数；configure只重置反馈基线，不清本轮累计调整次数。
    m_reductions = m_increases = 0;
    configure(control, current, now, feedback);
}
void AdaptiveBitrate::configure(const BitrateControl &control, int current, qint64 now, const BitrateFeedback &feedback)
{
    if (!valid(control)) throw std::invalid_argument("视频码率范围无效");
    m_control = control;
    m_target = control.automatic ? std::clamp(current, control.minimum, control.maximum) : control.maximum;
    m_previous = feedback; m_lastSampleUs = m_lastChangeUs = now; m_lastDecreaseUs = m_stableSinceUs = -1;
    m_sampled = false; m_writeLoad = 0; m_pressureWindows = 0;
    m_state = control.automatic ? BitrateState::Observing : BitrateState::Manual;
}
bool AdaptiveBitrate::update(qint64 now, const BitrateFeedback &value)
{
    // 2. 取墙钟窗口的累计差值；同一历史长写出不能在后续窗口重复触发下降。
    const auto elapsed = now - m_lastSampleUs;
    if (elapsed < 500000) return false;
    const auto frames = value.videoFrames - m_previous.videoFrames;
    const auto bytes = value.outputBytes - m_previous.outputBytes;
    const auto expired = value.expired - m_previous.expired;
    const auto write = std::max<qint64>(0, value.writeUs - m_previous.writeUs);
    const auto process = std::max<qint64>(0, value.videoProcessUs - m_previous.videoProcessUs);
    m_previous = value; m_lastSampleUs = now;
    if (!m_control.automatic) { m_state = BitrateState::Manual; return true; }
    const auto load = std::clamp(double(write) / elapsed, 0.0, 1.0);
    m_writeLoad = m_sampled ? .7 * m_writeLoad + .3 * load : load; m_sampled = true;
    const bool hardBlock = value.peakWriteUs >= 250000;
    // 3. 无视频/无输出不做上探；长观察空档也不能算作“稳定了几秒”，真实长写阻塞仍保留下降证据。
    if ((!frames || !bytes || elapsed > 2000000) && !hardBlock) {
        m_stableSinceUs = -1; m_pressureWindows = 0; m_state = BitrateState::Idle; return true;
    }
    const bool processing = frames && double(process) / frames >= 28000 && load < .15 && value.peakWriteUs < 100000;
    if (processing) {
        m_stableSinceUs = -1; m_pressureWindows = 0; m_state = BitrateState::ProcessingLimited; return true;
    }
    const bool pressure = m_writeLoad >= .65 || (m_writeLoad >= .30 && (expired || value.queueWaitMs >= 150));
    m_pressureWindows = pressure ? m_pressureWindows + 1 : 0;
    if (hardBlock || m_pressureWindows >= 2) {
        // 4. 严重写阻塞直接下降25%，持续压力下降20%；冷却与下限防止逐帧重配置。
        m_stableSinceUs = -1;
        if (m_target == m_control.minimum) { m_state = BitrateState::AtMinimum; return true; }
        if (m_lastDecreaseUs < 0 || now - m_lastDecreaseUs >= 1500000) {
            m_target = int(std::max<qint64>(m_control.minimum, qint64(m_target) * (hardBlock ? 75 : 80) / 100));
            m_lastDecreaseUs = m_lastChangeUs = now; ++m_reductions; m_pressureWindows = 0;
        }
        m_state = BitrateState::Decreasing; return true;
    }
    // 5. 串行RTMP写出本身有占时，不能要求近乎空闲才恢复；与65%压力阈值及150ms队列阈值留迟滞。
    const bool healthy = m_writeLoad < .55 && value.peakWriteUs < 100000 && value.queueWaitMs < 80 && !expired
        && frames >= 4 && value.videoBitrate >= quint64(m_target) / 2;
    if (!healthy) { m_stableSinceUs = -1; m_state = BitrateState::Observing; return true; }
    if (m_stableSinceUs < 0) m_stableSinceUs = now;
    if (now - m_stableSinceUs >= 5000000 && now - m_lastChangeUs >= 3000000 && m_target < m_control.maximum) {
        const int step = std::clamp(m_target / 10, 50000, 500000);
        m_target = std::min(m_control.maximum, m_target + step); ++m_increases; m_lastChangeUs = now;
        m_state = BitrateState::Recovering;
    } else m_state = BitrateState::Observing;
    return true;
}
}
