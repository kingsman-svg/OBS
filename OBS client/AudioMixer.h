#pragma once
#include "AudioResampler.h"
#include "CaptureSource.h"
#include "StereoLimiter.h"
#include <array>
#include <deque>

namespace csn {
// 一路混音输入的状态，直接放在同一头文件，不再建立设备/处理继承树。
struct AudioMixTrack {
    AudioResampler resampler;            // 此路独立滤波状态，不能与另一设备共用。
    std::deque<AudioPacket> packets;      // 已转为 48kHz 立体声的有界时间戳队列。
    int queuedFrames = 0;                // 缓存帧数，最多保留 500ms。
    bool enabled = false;                // 本轮是否选择这路；未启用不算缺包。
    float gain = 1;                      // 线性增益 0～2，默认原音量。
    bool muted = false;                  // 静音仍推进消费时间，恢复不会重放旧声音。
    float currentGain = 1, targetGain = 1, gainStep = 0; // 当前/有效目标/逐采样步长，左右共用。
    int rampFrames = 0;                  // 剩余过渡帧数，运行时改变音量或静音用480帧即10ms。
};

// 两路实时混音；调用方串行使用，时间由 QPC 驱动，不依赖定时器精度。
class AudioMixer final {
public:
    void begin(bool microphone, bool system); // 新一轮采集清缓存，保留用户音量/静音选择。
    void clear();                            // 退出/异常丢弃数据，不交付尾部。
    void setControl(CaptureSource::Kind kind, float gain, bool muted); // 更新音量/静音。
    void disable(CaptureSource::Kind kind);   // 设备失败撤掉此路，其他成功路继续工作。
    void push(CaptureSource::Kind kind, const AudioPacket &packet); // 转换并按时间戳入队。
    QList<AudioPacket> takeFrames(qint64 now100ns); // 80ms对齐和一包限幅预读，单次最多20个10ms包。
    QList<AudioPacket> finish();              // 先排空重采样再排空限幅，末尾不足10ms补零。
    bool active() const;                      // 至少一路启用；未收包时不产生无意义输出。
    float level() const { return m_level; }   // 最近一包混音后的 RMS 电平。
    quint64 clippedSamples() const { return m_clippedSamples; } // 兼容旧接口：限幅前超出±1的采样值数，非输出硬削波数。
    quint64 limitedFrames() const { return m_limiter.limitedFrames(); } // 左右联动衰减帧数。
    quint64 safetyClips() const { return m_limiter.safetyClips(); } // 最终异常越界保护计数。
    float limiterGain() const { return m_limiter.gain(); } // 供现有状态文字显示增益衰减。
    AudioDriftStats drift(CaptureSource::Kind kind) const { return m_tracks[index(kind)].resampler.drift(); } // 串行取每路漂移诊断。
    static constexpr int FrameSamples = 480; // 48kHz 下每包 10ms，每帧两个 Float32。
private:
    static int index(CaptureSource::Kind kind); // 麦克风0/系统回环1，其他类型拒绝。
    qint64 frameAt(qint64 timestamp) const;    // QPC 转成本轮相对输出帧位置。
    void enqueue(AudioMixTrack &track, AudioPacket packet); // 丢弃过时数据并限制缓存。
    AudioPacket mixFrame();                   // 对齐、补零、增益平滑、浮点相加，再交预读限幅；首包可能为空。
    std::array<AudioMixTrack, 2> m_tracks;     // 两路独立 resampler 和队列，GUI 串行访问。
    StereoLimiter m_limiter;                 // 总线联动限幅，额外等待一包，在现有120ms同步总预算内。
    qint64 m_origin = 0;                      // 首个有效输入包时间；开始前为0。
    bool m_originEstimated = false;           // 原点来自估计时间时，所有输出都保留该不确定性。
    qint64 m_cursor = 0;                      // 下一输出包的相对帧位置，只向前移动。
    bool m_discontinuity = false;             // 缓存溢出/跳过积压后标记下一包。
    float m_level = 0;                        // 最近输出 RMS，清理时归零。
    quint64 m_clippedSamples = 0;             // 限幅前±1超幅计数，左右采样值分别统计。
};
} // namespace csn
