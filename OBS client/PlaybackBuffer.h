#pragma once
#include <QtGlobal>

namespace csn {
// 播放端普通策略对象，无线程/设备。音频输出线程或纯视频路径在自己的队列锁内使用。
class PlaybackBuffer final {
public:
    static constexpr qint64 MinimumUs = 50000;  // 初始/最低目标50ms，留出基本交付余量。
    static constexpr qint64 MaximumUs = 150000; // 目标最多150ms，不用无限等待掩盖持续断流。
    static constexpr qint64 CapacityUs = 300000; // 固定安全上限；目标变化不扩张内存边界。
    void begin(qint64 nowUs);                 // 每次begin重置目标、交付基线、冷却和计数。
    void observe(qint64 ptsUs, qint64 durationUs, qint64 nowUs); // 记录解码交付间隙，包含网络/解码/调度影响。
    void update(qint64 nowUs, qint64 readyUs); // 500ms窗口；低水位伴随迟交付时快增，稳定5秒后慢减。
    void underrun(qint64 nowUs);             // 实际断粮：提高恢复目标，清稳定状态；连续断流不重复增长。
    bool catchup(qint64 nowUs, qint64 readyUs); // 超目标持续500ms才追赶，间隔至少1秒。
    qint64 targetUs() const { return m_targetUs; } // 启动/恢复所需量；运行时也是追赶的保留目标。
    qint64 jitterUs() const { return qint64(m_jitterUs); } // 平滑交付迟到量，不称作网络RTT/纯网络抖动。
    quint64 increases() const { return m_increases; } // 本轮目标增加次数，到达上限后不虚增。
    quint64 decreases() const { return m_decreases; } // 本轮目标减少次数。
private:
    void increase(qint64 nowUs, qint64 desiredUs); // 限定50～150ms，增长冷却1秒。
    qint64 m_targetUs = MinimumUs;            // 当前目标，不是每包额外等待时间。
    qint64 m_lastArrivalUs = -1, m_lastPtsUs = -1, m_lastEndUs = -1; // 相邻有效媒体交付基线。
    qint64 m_lastSampleUs = 0, m_lastIncreaseUs = -1, m_lastDecreaseUs = 0; // 采样与升降冷却。
    qint64 m_stableSinceUs = -1, m_overSinceUs = -1, m_lastCatchupUs = -1; // 健康观察及持续积压。
    qint64 m_latePeakUs = 0;                  // 当前窗口最长正向交付偏差，取样后清零。
    int m_packets = 0;                       // 窗口有实际媒体才允许判断稳定，空闲不上下调。
    double m_jitterUs = 0;                   // 当前10%＋历史90%，用于诊断交付变化。
    bool m_reduced = false;                  // 目标刚降低时允许小幅、有冷却的实际追赶。
    quint64 m_increases = 0, m_decreases = 0; // begin清零，不跨会话继承。
};
}
