#include "MediaPlayer.h"
#include <QMutexLocker>
#include <windows.h>
#include <winrt/base.h>
#include <algorithm>
#include <stdexcept>

namespace csn {
namespace {
qint64 nowUs()
{
    LARGE_INTEGER tick{}, rate{}; QueryPerformanceCounter(&tick); QueryPerformanceFrequency(&rate);
    return tick.QuadPart / rate.QuadPart * 1000000 + tick.QuadPart % rate.QuadPart * 1000000 / rate.QuadPart;
}
}
MediaPlayer::MediaPlayer(QObject *parent) : QThread(parent)
{
    qRegisterMetaType<MediaInfo>();
    // 1. 直连只更新受锁保护的错误状态，输出线程不访问View；stop唤醒阻塞中的PCM入口。
    connect(&m_audio, &WasapiPlayback::failed, this, [this](const QString &message) {
        { QMutexLocker lock(&m_mutex); m_audioError = message; m_changed.wakeAll(); }
        m_audio.stop();
    }, Qt::DirectConnection);
}
MediaPlayer::~MediaPlayer() { stop(); wait(); }
bool MediaPlayer::begin(const QString &source, bool audioOutput, bool live, VideoDecodeMode mode, bool lowLatency, bool adaptiveBuffer)
{
    // 2. GUI只保存输入、清状态并start；Controller等finished回到GUI后才允许重启。
    QMutexLocker lock(&m_mutex);
    if (isRunning() || m_audio.isRunning()) return false;
    m_source = source.trimmed(); m_audioRequested = audioOutput; m_audioActive = false;
    m_audioError.clear(); m_stats = {}; m_video.clear(); m_wallOriginUs = m_mediaEndUs = m_videoEndUs = 0;
    m_audioEndSamples = -1;
    m_live = live; m_liveClockBaseUs = -1; m_decodeMode = mode;
    m_lowLatency = live && lowLatency; m_startedUs = nowUs();
    m_adaptiveBuffer = m_lowLatency && adaptiveBuffer; m_videoBuffer.begin(m_startedUs);
    m_videoBuffering = m_adaptiveBuffer; m_videoStarted = false; m_videoHeldUs = 0; m_videoRebufferSinceUs = -1;
    m_accepting = true; m_decodeEof = false; start(); return true;
}
void MediaPlayer::stop()
{
    const qint64 position = clockUs();
    requestInterruption();
    { QMutexLocker lock(&m_mutex); m_stats.positionUs = position; m_accepting = false; m_video.clear(); m_changed.wakeAll(); }
    m_audio.stop();
}
void MediaPlayer::setVolume(float volume) { m_audio.setVolume(volume); }
void MediaPlayer::checkAudioError() const
{
    QMutexLocker lock(&m_mutex);
    if (!m_audioError.isEmpty()) throw std::runtime_error(m_audioError.toStdString());
}
qint64 MediaPlayer::clockUs() const
{
    qint64 origin, end, base; bool audio, eof;
    { QMutexLocker lock(&m_mutex); if (!m_accepting) return m_stats.positionUs;
      origin = m_wallOriginUs; audio = m_audioActive; end = m_mediaEndUs; eof = m_decodeEof;
      base = m_live ? std::max<qint64>(0, m_liveClockBaseUs) : 0;
      if (m_adaptiveBuffer && !audio && m_videoBuffering) return m_videoHeldUs; }
    if (!origin) return 0;
    qint64 result = base + std::max<qint64>(0, nowUs() - origin);
    if (audio) {
        const auto clock = m_audio.clock();
        if (!clock.started && !clock.ended) return 0; // 预缓冲期间不提前播放画面。
        if (clock.started) result = clock.timeUs + (clock.ended ? std::max<qint64>(0, nowUs() - clock.endedQpcUs) : 0);
    }
    return eof ? std::min(result, end) : result;
}
PlaybackStats MediaPlayer::stats() const
{
    const qint64 position = clockUs(); const auto audio = m_audio.clock();
    QMutexLocker lock(&m_mutex); auto result = m_stats;
    if (m_accepting) result.positionUs = position;
    result.audioUnderruns = m_audioActive ? audio.underruns : 0;
    result.adaptiveBuffer = m_adaptiveBuffer;
    result.queuedVideo = m_video.size();
    if (!m_video.empty()) result.videoQueueSpanUs = m_video.back().ptsUs + m_video.back().durationUs - m_video.front().ptsUs;
    if (m_audioActive) {
        result.audioQueuedUs = audio.queuedUs; result.audioPaddingUs = audio.devicePaddingUs; result.audioBufferUs = audio.deviceBufferUs;
        result.droppedAudioSamples = audio.droppedSamples; result.audioCatchups = audio.catchups;
        result.renderedAudioSamples = audio.renderedSamples; result.insertedSilenceSamples = audio.silenceSamples;
        result.buffering = audio.buffering; result.bufferTargetUs = audio.targetUs; result.deliveryJitterUs = audio.deliveryJitterUs;
        result.bufferIncreases = audio.bufferIncreases; result.bufferDecreases = audio.bufferDecreases;
        result.rebuffers = audio.rebuffers; result.rebufferUs = audio.rebufferUs;
    } else if (m_adaptiveBuffer) {
        result.buffering = m_videoBuffering; result.bufferTargetUs = m_videoBuffer.targetUs(); result.deliveryJitterUs = m_videoBuffer.jitterUs();
        result.bufferIncreases = m_videoBuffer.increases(); result.bufferDecreases = m_videoBuffer.decreases();
    }
    return result;
}
PlaybackVideo MediaPlayer::takeVideo()
{
    qint64 clock = clockUs(); PlaybackVideo result;
    QMutexLocker lock(&m_mutex);
    if (!m_accepting) return result;
    if (m_adaptiveBuffer && !m_audioActive && !scheduleVideo(nowUs(), clock)) return result;
    // 5. 未来帧留队列；明显迟到且有替代帧时丢旧帧，最后一张仍交付并保留。
    while (!m_video.empty() && m_video.front().ptsUs <= clock) {
        if (result.source.texture) ++m_stats.droppedVideo;
        result = std::move(m_video.front()); m_video.pop_front();
        if (clock - result.ptsUs <= 80000) break;
    }
    if (result.source.texture) {
        if (!m_stats.displayedVideo) m_stats.firstDeliveryUs = nowUs() - m_startedUs;
        ++m_stats.displayedVideo; m_changed.wakeAll();
    }
    return result;
}
bool MediaPlayer::scheduleVideo(qint64 now, qint64 &clock)
{
    // 5a. 纯视频/关闭音频输出也使用自适应策略；GUI取帧驱动状态，解码线程仅入队和记录交付。
    const auto ready = m_video.empty() ? 0 : std::max<qint64>(0, m_video.back().ptsUs + m_video.back().durationUs
        - (m_videoBuffering ? m_video.front().ptsUs : std::max(clock, m_video.front().ptsUs)));
    const auto previousTarget = m_videoBuffer.targetUs();
    m_videoBuffer.update(now, ready);
    if (!m_videoBuffering && m_videoBuffer.targetUs() > previousTarget && ready < m_videoBuffer.targetUs() && !m_decodeEof) {
        m_videoHeldUs = clock; m_videoBuffering = true; m_videoRebufferSinceUs = now; ++m_stats.rebuffers;
    }
    if (!m_videoBuffering && m_video.empty() && clock >= m_videoEndUs && !m_decodeEof) {
        m_videoHeldUs = clock; m_videoBuffering = true; m_videoRebufferSinceUs = now;
        ++m_stats.rebuffers; m_videoBuffer.underrun(now);
    }
    if (m_videoBuffering) {
        if (m_video.empty() || (ready < m_videoBuffer.targetUs() && !m_decodeEof)) return false;
        m_liveClockBaseUs = std::max(m_videoHeldUs, m_video.front().ptsUs); m_wallOriginUs = now;
        clock = m_liveClockBaseUs; m_videoBuffering = false; m_videoStarted = true;
        if (m_videoRebufferSinceUs >= 0) { m_stats.rebufferUs += now - m_videoRebufferSinceUs; m_videoRebufferSinceUs = -1; }
    } else if (m_videoStarted && m_videoBuffer.catchup(now, ready) && !m_video.empty()) {
        // 5b. 只裁解码纹理，不能任意丢参考压缩包。重新锚定主时钟，实现目标降低后的实际追赶。
        const auto target = m_video.back().ptsUs + m_video.back().durationUs - m_videoBuffer.targetUs();
        while (m_video.size() > 1 && m_video.front().ptsUs + m_video.front().durationUs <= target) { m_video.pop_front(); ++m_stats.droppedVideo; }
        m_liveClockBaseUs = std::max(clock, m_video.front().ptsUs); m_wallOriginUs = now; clock = m_liveClockBaseUs;
    }
    return true;
}
void MediaPlayer::pushVideo(const PlaybackVideo &frame)
{
    QMutexLocker lock(&m_mutex);
    // 4. 解码背压只挡工作线程；音轨位于后面时先建立静音时钟，GUI才能消费早期视频。
    if (m_live && !m_adaptiveBuffer && !m_audioActive && (m_liveClockBaseUs < 0 || frame.ptsUs > m_liveClockBaseUs + nowUs() - m_wallOriginUs + 300000)) {
        m_liveClockBaseUs = frame.ptsUs; m_wallOriginUs = nowUs(); // 首帧/较大跳变追赶直播位置。
    }
    // 直播也保留最多16张未来帧：硬解首屏比音频主时钟快时，不能在到期前全部挤掉。
    if (m_live && m_video.size() >= 16) { m_video.pop_front(); ++m_stats.droppedVideo; }
    // 4a. 自适应允许500ms/16帧未来画面余量，覆盖150ms恢复目标与解码突发；固定对照仍为300ms。
    while (m_lowLatency && !m_video.empty() && frame.ptsUs - m_video.front().ptsUs > (m_adaptiveBuffer ? 500000 : 300000)) {
        m_video.pop_front(); ++m_stats.droppedVideo;
    }
    while (!m_live && m_accepting && m_video.size() >= 16 && m_audioError.isEmpty()) {
        if (m_audioActive) m_audio.forceStart();
        m_changed.wait(&m_mutex, 10);
    }
    if (!m_audioError.isEmpty()) throw std::runtime_error(m_audioError.toStdString());
    if (!m_accepting || isInterruptionRequested()) throw std::runtime_error("播放已停止");
    if (m_stats.decodedVideo == 0 && m_audioActive) m_audio.videoReady(); // 首帧硬解/GPU转换已完成，声音才建立主时钟。
    if (!m_stats.decodedVideo) m_stats.firstDecodedUs = nowUs() - m_startedUs;
    m_video.push_back(frame); ++m_stats.decodedVideo;
    if (m_adaptiveBuffer && !m_audioActive) m_videoBuffer.observe(frame.ptsUs, frame.durationUs, nowUs());
    if (frame.decodePath == VideoDecodePath::D3D11Gpu) ++m_stats.gpuVideo;
    else if (frame.decodePath == VideoDecodePath::D3D11Readback) ++m_stats.readbackVideo;
    else ++m_stats.softwareVideo;
    m_stats.decoderNote = frame.decoderNote;
    m_mediaEndUs = std::max(m_mediaEndUs, frame.ptsUs + frame.durationUs);
    m_videoEndUs = std::max(m_videoEndUs, frame.ptsUs + frame.durationUs);
}
void MediaPlayer::pushAudio(const PlaybackAudio &packet)
{
    checkAudioError(); bool output;
    { QMutexLocker lock(&m_mutex);
      const auto count = packet.pcm.size() / 8; m_stats.audioSamples += quint64(count); output = m_audioActive;
      m_mediaEndUs = std::max(m_mediaEndUs, (packet.pts + count) * 1000000 / 48000); }
    if (output && !m_audio.push(packet)) { checkAudioError(); throw std::runtime_error("播放已停止"); }
    if (output && m_audioEndSamples >= 0 && packet.pts + packet.pcm.size() / 8 >= m_audioEndSamples) m_audio.finish();
}
void MediaPlayer::run()
{
    QString error; bool endOfFile = false;
    try {
        // 3. 普通解码对象只在本线程存在；读包/解码/像素转换/上传都不占GUI。
        MediaDecoder decoder;
        const auto info = decoder.open(m_source, [this] { return isInterruptionRequested(); },
            [this](const PlaybackVideo &frame) { pushVideo(frame); }, [this](const PlaybackAudio &packet) { pushAudio(packet); }, m_live, m_decodeMode, m_lowLatency);
        {
            QMutexLocker lock(&m_mutex); m_audioActive = info.audio && m_audioRequested;
            if (!info.video && !m_audioActive) { m_adaptiveBuffer = false; m_videoBuffering = false; } // 无实际输出时沿用QPC进度，不等待不存在的视频。
            m_audioEndSamples = info.audioEndSamples; m_wallOriginUs = nowUs();
            m_stats.openUs = nowUs() - m_startedUs;
        }
        if (!isInterruptionRequested()) {
            if (m_audioActive && !m_audio.begin(info.audioStartSamples, m_live, info.video, m_lowLatency, m_adaptiveBuffer)) throw std::runtime_error("音频输出线程尚未退出");
            emit opened(info);
            while (!isInterruptionRequested()) { checkAudioError(); if (!decoder.readNext()) { endOfFile = true; break; } }
            if (endOfFile && m_live && !isInterruptionRequested()) throw std::runtime_error("直播媒体已断开，请等待主播重开或重新播放");
            if (endOfFile && !isInterruptionRequested()) {
                // 6. EOF仍取完解码/SWR尾部，然后关闭音频入口；数据读完不代表已经播完。
                decoder.finish(); if (m_audioActive) m_audio.finish();
                { QMutexLocker lock(&m_mutex); m_decodeEof = true; }
                for (;;) {
                    if (isInterruptionRequested()) break;
                    checkAudioError(); const qint64 clock = clockUs();
                    bool videoDone;
                    { QMutexLocker lock(&m_mutex); videoDone = m_video.empty() && clock >= m_mediaEndUs; }
                    if (videoDone && (!m_audioActive || !m_audio.isRunning())) break;
                    msleep(5);
                }
            }
        }
    } catch (const winrt::hresult_error &failure) { error = QString::fromStdWString(failure.message().c_str()); }
      catch (const std::exception &failure) { error = QString::fromUtf8(failure.what()); }
    // 7. 解码对象先析构，再关闭/等待音频；正常GUI只等finished，不调用wait。
    const qint64 position = clockUs(); m_audio.stop(); m_audio.wait();
    { QMutexLocker lock(&m_mutex); m_stats.positionUs = endOfFile && !isInterruptionRequested() ? std::min(position, m_mediaEndUs) : position;
      m_accepting = false; m_video.clear(); }
    if (!error.isEmpty() && !isInterruptionRequested()) emit failed(tr("播放失败：%1").arg(error));
    else emit completed(endOfFile && !isInterruptionRequested());
    // 8. run返回后QThread发finished，Controller才解锁下一轮输入。
}
}
