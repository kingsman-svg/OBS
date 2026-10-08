#pragma once
#include "AudioPacket.h"
struct SwrContext;

namespace csn {
// 一路连续音频对应一个实例；非 QObject，由调用方串行使用，默认输出 48kHz 立体声。
class AudioResampler final {
public:
    AudioResampler() = default;                 // 延迟到首个输入包才创建 FFmpeg 上下文。
    ~AudioResampler();                          // swr_free 归还上下文及滤波器缓冲。
    AudioResampler(const AudioResampler &) = delete; // 独占 SwrContext，不允许浅复制。
    AudioResampler &operator=(const AudioResampler &) = delete;
    AudioPacket convert(const AudioPacket &input); // 输入任意合法包长；返回的包长由滤波延迟决定。
    AudioPacket drain();                        // 正常结束取出滤波尾部并重置；可重复调用。
    void reset();                               // 丢弃旧滤波数据，换源/异常/退出时使用。
    static constexpr int OutputRate = 48000;     // 处理层统一采样率，采集层不变。
private:
    void configure(const AudioPacket &input);    // 按采样率、声道及掩码初始化 swr。
    AudioPacket resample(const uchar *data, int frames); // 执行转换并用输出样本数生成连续时间戳。
    SwrContext *m_context = nullptr;             // 独占的 libswresample 状态；非线程安全。
    int m_inputRate = 0;                         // 当前输入采样率，格式变化时重建。
    int m_inputChannels = 0;                     // 当前输入声道数。
    quint64 m_inputMask = 0;                     // 当前输入布局，防止同声道数不同布局被混用。
    qint64 m_origin = 0;                         // 当前连续输入段的首帧 QPC 时间。
    qint64 m_inputFrames = 0;                    // 当前段已接收帧数，避免逐包时间舍入累积。
    qint64 m_outputFrames = 0;                   // 当前段实际输出帧数，包含 drain 的尾部。
    qint64 m_lastTimestamp = 0;                  // 上个输入包时间，过滤小于5ms包长的重复/倒序包。
    bool m_discontinuity = false;               // 下一非空输出要带不连续标志。
    bool m_estimated = false;                   // 当前连续段是否包含估计时间戳。
};
} // namespace csn
