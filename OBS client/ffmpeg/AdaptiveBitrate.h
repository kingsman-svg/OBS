#pragma once
#include <QtGlobal>

namespace csn {
inline constexpr int MinimumVideoBitrate = 100000; // 码率单位统一为bit/s，界面显示时才转kbps。
inline constexpr int MaximumVideoBitrate = 50000000;
struct BitrateControl {
    bool automatic = false;      // false固定maximum，true在minimum～maximum之间调整。
    int minimum = 400000;        // 自动下限，不代表该码率足够维持当前尺寸的画质。
    int maximum = 4000000;       // 手动目标或自动上限；进入自动模式时不主动跳升到上限。
};
struct BitrateFeedback {
    quint64 videoFrames = 0;     // 以下为本轮累计值，策略按单调时间窗口取差值。
    quint64 outputBytes = 0;     // 本机媒体IO字节，仅作为有发送活动的证据，不估算可用带宽。
    quint64 expired = 0;         // 视频/音频过期丢弃总数，提供队列压力旁证。
    qint64 writeUs = 0;          // 累计媒体写出时间，含交织与flush，不是RTT。
    qint64 videoProcessUs = 0;   // 累计GPU/编码时间，已扣除同步写出。
    qint64 peakWriteUs = 0;      // 自上次被采样以来的最长媒体写出；取样后调用方清零。
    qint64 queueWaitMs = 0;      // 当前两路队头等待的最大值，裁剪证据由expired保留。
    quint64 videoBitrate = 0;    // 实际压缩码率；低负载时不把“写得快”当成恢复带宽的证明。
};
enum class BitrateState { Manual, Observing, Decreasing, AtMinimum, Recovering, ProcessingLimited, Idle };

// 普通策略对象，无线程/锁/FFmpeg资源；由Publisher::run独占，人工时钟可直接验证决策。
class AdaptiveBitrate final {
public:
    static bool valid(const BitrateControl &control); // 校验完整配置，错误请求不改变旧状态。
    void begin(const BitrateControl &control, int current, qint64 nowUs, const BitrateFeedback &feedback); // 每轮清计数。
    void configure(const BitrateControl &control, int current, qint64 nowUs, const BitrateFeedback &feedback); // 切换/改范围清旧采样。
    bool update(qint64 nowUs, const BitrateFeedback &feedback); // 满500ms才采样；返回true时调用方清峰值。
    int target() const { return m_target; }                      // 最新计划目标，下一张视频才提交编码器。
    bool automatic() const { return m_control.automatic; }       // 工作线程已经接收的模式。
    BitrateState state() const { return m_state; }               // 用于界面解释本次保持/调整原因。
    quint64 reductions() const { return m_reductions; }          // 本轮自动下降次数，不含用户主动改范围。
    quint64 increases() const { return m_increases; }            // 本轮自动回升次数。
    double writeLoad() const { return m_writeLoad; }             // EWMA写出占时比例，不称为带宽利用率。
private:
    BitrateControl m_control;                // 当前已接收的模式和上下限。
    BitrateFeedback m_previous;              // 上次采样累计值，配置切换时重新建立基线。
    int m_target = 0;                        // 当前计划码率，始终位于配置范围内。
    qint64 m_lastSampleUs = 0;               // 上次取样的单调时间。
    qint64 m_lastChangeUs = 0;               // 限制恢复探测的频率。
    qint64 m_lastDecreaseUs = -1;            // 相邻下降至少1.5秒，首个严重写阻塞可立即响应。
    qint64 m_stableSinceUs = -1;             // 连续健康、有媒体负载的开始时间。
    int m_pressureWindows = 0;              // 中等压力需连续两个采样窗口，过滤短突发。
    bool m_sampled = false;                 // 第一份有效观测直接初始化EWMA。
    double m_writeLoad = 0;                 // 当前30%＋历史70%的写出占时平滑值。
    quint64 m_reductions = 0, m_increases = 0; // 仅begin清零，模式切换保留本轮诊断。
    BitrateState m_state = BitrateState::Manual; // 当前策略状态，不宣称网络精确分类。
};
}
