#include "MediaTimeline.h"
extern "C" {
#include <libavutil/mathematics.h>
}
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace csn {
namespace {
constexpr qint64 Second = 10000000;
constexpr int AudioFrames = 480;
constexpr int MaxVideo = 16;
constexpr int MaxAudio = 50;
}

void MediaTimeline::clear()
{
    m_video.clear(); m_audio.clear(); m_current = {};
    m_origin = m_lastNow = m_nextVideo = 0;
    m_lastVideoTime = m_lastVideoInput = m_audioEnd = -1;
    m_lastSequence = 0; m_active = m_videoEnabled = m_audioEnabled = false;
    m_videoGap = m_audioGap = false; m_stats = {};
}

void MediaTimeline::begin(qint64 origin, bool video, bool audio)
{
    // 1. 在启动设备前记录共同原点；音频/视频先到哪一路都不会改变它。
    if (origin <= 0 || origin > std::numeric_limits<qint64>::max() - Second)
        throw std::invalid_argument("媒体时间原点无效");
    clear(); m_origin = m_lastNow = origin;
    m_videoEnabled = video; m_audioEnabled = audio; m_active = video || audio;
}

void MediaTimeline::disableVideo()
{
    m_videoEnabled = false; m_video.clear(); m_current = {};
}
void MediaTimeline::disableAudio()
{
    m_audioEnabled = false; m_audio.clear();
}

bool MediaTimeline::pushVideo(const FaceFrame &frame)
{
    // 2. 纹理与框点作为一个整体缓存；GPU 处理耗时不写入来源时间戳。
    if (!m_active || !m_videoEnabled || !frame.video.texture) return false;
    const qint64 time = frame.video.timestamp100ns;
    if (time < m_origin || time <= m_lastVideoInput || time < m_lastVideoTime) {
        ++m_stats.droppedVideo; m_videoGap = true; return false;
    }
    m_lastVideoInput = time;
    m_video.push_back(frame);
    if (m_video.size() > MaxVideo) {
        m_video.pop_front(); ++m_stats.droppedVideo; m_videoGap = true;
    }
    return true;
}

bool MediaTimeline::pushAudio(const AudioPacket &packet)
{
    // 3. 这里位于混音之后，格式固定；编码器需要的样本格式转换放在下一层。
    if (!m_active || !m_audioEnabled) return false;
    if (packet.sampleRate != AudioRate || packet.channels != 2 || packet.channelMask != 3
        || packet.pcm.size() != AudioFrames * 2 * qsizetype(sizeof(float)))
        throw std::invalid_argument("同步层只接受 48kHz 立体声 Float32 的 10ms 混音包");
    if (packet.timestamp100ns < m_origin) { ++m_stats.droppedAudio; m_audioGap = true; return false; }
    const qint64 pts = av_rescale(packet.timestamp100ns - m_origin, AudioRate, Second);
    if (pts > std::numeric_limits<qint64>::max() - AudioFrames || (m_audioEnd >= 0 && pts < m_audioEnd)) {
        ++m_stats.droppedAudio; m_audioGap = true; return false;
    }
    TimedAudioPacket frame{packet, pts};
    if (m_audioEnd >= 0 && pts != m_audioEnd) frame.source.discontinuity = true;
    m_audioEnd = pts + AudioFrames; m_audio.push_back(std::move(frame));
    if (m_audio.size() > MaxAudio) {
        m_audio.pop_front(); ++m_stats.droppedAudio; m_audioGap = true;
    }
    return true;
}

MediaBatch MediaTimeline::takeFrames(qint64 now)
{
    // 4. 总缓冲只计算一次：音频已在混音层等 80ms，视频也等到同一 120ms 截止线。
    if (!m_active || now < m_lastNow) return {};
    m_lastNow = now;
    if (now < m_origin || now - m_origin < Delay100ns) return {};
    return takeUntil(now - m_origin - Delay100ns, false);
}

MediaBatch MediaTimeline::takeUntil(qint64 relative, bool draining)
{
    MediaBatch output;
    const qint64 dueVideo = av_rescale_rnd(relative, VideoRate, Second, AV_ROUND_DOWN);
    const int videoLimit = draining ? MaxVideo : 6;
    if (m_videoEnabled && dueVideo - m_nextVideo >= videoLimit) {
        const qint64 skipped = dueVideo - videoLimit + 1 - m_nextVideo;
        m_nextVideo += skipped; m_stats.droppedVideo += quint64(skipped); m_videoGap = true;
    }
    // 5. 按 30fps 时间格选当时最新已完成纹理；静止 WGC 的画面重复，PTS 不重复。
    while (m_videoEnabled && m_nextVideo <= dueVideo) {
        const qint64 time = m_origin + av_rescale(m_nextVideo, Second, VideoRate);
        while (!m_video.empty() && m_video.front().video.timestamp100ns <= time) {
            m_current = std::move(m_video.front()); m_video.pop_front();
        }
        if (m_current.video.texture) {
            const bool repeated = m_stats.videoFrames && m_current.video.sequence == m_lastSequence;
            output.video.append({m_current, m_nextVideo, repeated, m_videoGap});
            m_lastSequence = m_current.video.sequence; m_lastVideoTime = time;
            ++m_stats.videoFrames; if (repeated) ++m_stats.repeatedVideo;
            m_videoGap = false;
        }
        ++m_nextVideo;
    }
    // 6. 声音按样本位置交付，必须整个 10ms 包都已到期；两路无须一包对一帧。
    const qint64 dueAudio = av_rescale_rnd(relative, AudioRate, Second, AV_ROUND_DOWN);
    // 排空时队列本身已限制为500ms，保留全部尾部，不用视频末帧的取整时间再裁音频。
    while (!draining && !m_audio.empty() && dueAudio - m_audio.front().pts > AudioRate / 5) {
        m_audio.pop_front(); ++m_stats.droppedAudio; m_audioGap = true;
    }
    const int audioLimit = draining ? MaxAudio : 20;
    while (m_audioEnabled && !m_audio.empty() && m_audio.front().pts + AudioFrames <= dueAudio
        && output.audio.size() < audioLimit) {
        auto frame = std::move(m_audio.front()); m_audio.pop_front();
        frame.source.discontinuity = frame.source.discontinuity || m_audioGap;
        m_audioGap = false; output.audio.append(std::move(frame)); ++m_stats.audioPackets;
    }
    return output;
}

MediaBatch MediaTimeline::finish()
{
    // 7. 停止时只覆盖已经接受的媒体尾部；不按停止等待时长无限生成静止画面。
    if (!m_active) return {};
    qint64 end = 0;
    if (!m_video.empty()) {
        const qint64 slot = av_rescale_rnd(m_video.back().video.timestamp100ns - m_origin, VideoRate, Second, AV_ROUND_UP);
        end = av_rescale(slot, Second, VideoRate) + 1; // 最后一帧落到不早于采集时刻的格子，避免舍入丢尾帧。
    }
    if (!m_audio.empty()) end = std::max(end, av_rescale(m_audio.back().pts + AudioFrames, Second, AudioRate));
    auto output = takeUntil(end, true);
    m_video.clear(); m_audio.clear(); m_current = {}; m_active = false;
    m_videoEnabled = m_audioEnabled = false;
    return output;
}
} // namespace csn
