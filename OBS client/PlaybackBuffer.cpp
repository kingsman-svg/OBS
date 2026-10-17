#include "PlaybackBuffer.h"
#include <algorithm>
#include <cmath>

namespace csn {
void PlaybackBuffer::begin(qint64 now)
{
    // 1. Controller经Player/音频begin启动一轮；普通对象只保存策略，不创建线程。
    *this = PlaybackBuffer{}; m_lastSampleUs = m_lastDecreaseUs = now;
}
void PlaybackBuffer::observe(qint64 pts, qint64 duration, qint64 now)
{
    // 2. 用相邻PTS间隔对照单调交付间隔；跳变/倒序重新建基线，不能直接相减不同设备墙钟。
    if (m_lastArrivalUs >= 0 && now >= m_lastArrivalUs && pts > m_lastPtsUs
        && std::abs(pts - m_lastEndUs) <= 30000 && now - m_lastArrivalUs <= 2000000) {
        const auto late = std::max<qint64>(0, (now - m_lastArrivalUs) - (pts - m_lastPtsUs));
        m_latePeakUs = std::max(m_latePeakUs, late);
        m_jitterUs = .9 * m_jitterUs + .1 * late;
    }
    m_lastArrivalUs = now; m_lastPtsUs = pts; m_lastEndUs = pts + duration; ++m_packets;
}
void PlaybackBuffer::increase(qint64 now, qint64 desired)
{
    m_stableSinceUs = m_overSinceUs = -1; m_reduced = false;
    if (m_lastIncreaseUs >= 0 && now - m_lastIncreaseUs < 1000000) return;
    desired = std::clamp(desired, MinimumUs, MaximumUs);
    if (desired > m_targetUs) { m_targetUs = desired; m_lastIncreaseUs = now; ++m_increases; }
}
void PlaybackBuffer::underrun(qint64 now)
{
    // 3. 每次实际进入重新缓冲才调用，断流期间的每次唤醒不算新的欠载。
    increase(now, m_targetUs + 20000);
}
void PlaybackBuffer::update(qint64 now, qint64 ready)
{
    const auto elapsed = now - m_lastSampleUs;
    if (elapsed < 500000) return;
    const auto packets = m_packets;
    const auto peak = m_latePeakUs;
    m_packets = 0; m_latePeakUs = 0; m_lastSampleUs = now;
    if (packets < 2 || elapsed > 2000000) { m_stableSinceUs = -1; return; }
    // 4. 迟交付同时接近低水位才提高目标；解码正常批量吐帧不直接当作网络拥塞。
    if (peak >= 40000 && ready < m_targetUs) { increase(now, std::max(m_targetUs + 20000, MinimumUs + peak)); return; }
    if (peak >= 20000 || ready < 15000) { m_stableSinceUs = -1; return; }
    if (m_stableSinceUs < 0) m_stableSinceUs = now;
    // 5. 健康5秒后，每2秒最多降10ms；降低目标由消费侧真正执行，不能只改一个数字。
    if (now - m_stableSinceUs >= 5000000 && now - m_lastDecreaseUs >= 2000000 && m_targetUs > MinimumUs) {
        m_targetUs = std::max(MinimumUs, m_targetUs - 10000); m_lastDecreaseUs = now; ++m_decreases; m_reduced = true;
    }
}
bool PlaybackBuffer::catchup(qint64 now, qint64 ready)
{
    // 6. 普通突发留60ms余量；降目标后留20ms。持续积压才剪，避免每个AAC批次都被裁掉。
    if (ready <= m_targetUs + (m_reduced ? 20000 : 60000)) { m_overSinceUs = -1; return false; }
    if (m_overSinceUs < 0) m_overSinceUs = now;
    if (now - m_overSinceUs < 500000 || (m_lastCatchupUs >= 0 && now - m_lastCatchupUs < 1000000)) return false;
    m_lastCatchupUs = now; m_overSinceUs = -1; m_reduced = false; return true;
}
}
