#pragma once
#include "AudioPacket.h"
struct SwrContext;

namespace csn {
struct AudioDriftStats {
    double clockPpm = 0;             // QPC时长相对标称样本时长的偏差；正值表示设备偏慢。
    double correctionPpm = 0;        // 已量化并提交swr的采样补偿；正值生成更多输出样本。
    double phaseErrorMs = 0;         // 可信QPC与已输出+滤波库存对应的相位误差，非网络延迟。
    quint64 adjustments = 0;         // 一秒估计窗口实际提交次数，reset清零。
    quint64 restarts = 0, rejected = 0, outliers = 0; // 格式/真跳变重建、重复倒序丢弃、异常估计次数。
    bool reliable = false;           // 最近用于估计的时间戳可信；估计时间戳不驱动校正。
};
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
    AudioDriftStats drift() const { return m_drift; } // Mixer/Controller串行读取，不访问采集线程。
private:
    void configure(const AudioPacket &input);    // 按采样率、声道及掩码初始化 swr。
    AudioPacket resample(const uchar *data, int frames); // 执行转换并用输出样本数生成连续时间戳。
    void compensate(const AudioPacket &input);  // 一秒QPC/样本窗口估计频差，附加慢相位回正，限幅/限速后软补偿。
    SwrContext *m_context = nullptr;             // 独占的 libswresample 状态；非线程安全。
    int m_inputRate = 0;                         // 当前输入采样率，格式变化时重建。
    int m_inputChannels = 0;                     // 当前输入声道数。
    quint64 m_inputMask = 0;                     // 当前输入布局，防止同声道数不同布局被混用。
    qint64 m_origin = 0;                         // 当前连续输入段的首帧 QPC 时间。
    qint64 m_inputFrames = 0;                    // 当前段已接收帧数，避免逐包时间舍入累积。
    qint64 m_outputFrames = 0;                   // 当前段实际输出帧数，包含 drain 的尾部。
    qint64 m_lastTimestamp = 0;                  // 上个输入包QPC，重复/倒序包不能再次推进滤波状态。
    int m_lastFrames = 0;                       // 上一包实际输入帧数，识别单次跳变而非累计漂移。
    bool m_lastEstimated = false;               // 前包估计时间不参与单包时钟跳变判定。
    bool m_discontinuity = false;               // 下一非空输出要带不连续标志。
    bool m_estimated = false;                   // 当前连续段是否包含估计时间戳。
    AudioDriftStats m_drift;                    // 当前会话诊断，内部重建与drain保留，显式reset清零。
    qint64 m_rateTime = 0, m_rateFrames = 0;     // 最近一秒频差估计窗口的QPC和累计输入帧起点。
    qint64 m_phaseTime = 0, m_phaseOutput = 0;   // 可信段QPC与输出库存原点，恢复可信时间后重新建立。
    bool m_clockValid = false, m_rateObserved = false; // 是否已有可信基线和有效频差，避免初始/估计时间误校正。
    double m_clockPpm = 0, m_commandPpm = 0;     // 平滑频差、限速后的命令；每秒最多改变100ppm，绝对不超1000ppm。
};
} // namespace csn
