#pragma once
#include "AudioPacket.h"

namespace csn {
// 普通DSP对象；AudioMixer串行持有，无线程。只接受48kHz立体声Float32的10ms包。
class StereoLimiter final {
public:
    static constexpr int Frames = 480;      // 每次交付480个采样帧，保持现有混音契约。
    static constexpr int Lookahead = 240;   // 5ms预读；按10ms包交付时保留一包等待下一包。
    static constexpr float Ceiling = .98f;  // 样本峰值上限，保留少量余量；不是过采样真峰值限制。
    void reset(bool clearStatistics = true); // 清预读和包络；会话清统计，跳过积压时可保留统计。
    AudioPacket push(AudioPacket packet);  // 第一包仅缓存；下一包到达后输出前一包，保留原时间戳。
    AudioPacket drain();                   // EOF仅用零作未来参考，输出最后有效包；重复排空为空。
    float gain() const { return m_gain; }   // 最后一个采样帧的左右联动增益。
    float level() const { return m_level; } // 最近输出包RMS，含真实限幅后的左右声道。
    quint64 limitedFrames() const { return m_limitedFrames; } // 实际应用衰减的采样帧数，左右算一帧。
    quint64 safetyClips() const { return m_safetyClips; } // 最终有限性/越界保护计数，正常算法应为0。
private:
    AudioPacket process(const AudioPacket &future); // 用未来前240帧求预读包络，再输出当前待处理包。
    AudioPacket m_pending;                 // 最多持有一包有效原始浮点混音，不加入无效起始静音。
    float m_gain = 1, m_level = 0;          // 跨包连续的限制增益和最近RMS。
    quint64 m_limitedFrames = 0, m_safetyClips = 0; // 每轮计数，由reset清零。
};
}
