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
bool MediaPlayer::begin(const QString &source, bool audioOutput)
{
    // 2. GUI只保存输入、清状态并start；Controller等finished回到GUI后才允许重启。
    QMutexLocker lock(&m_mutex);
    if (isRunning() || m_audio.isRunning()) return false;
    m_source = source.trimmed(); m_audioRequested = audioOutput; m_audioActive = false;
    m_audioError.clear(); m_stats = {}; m_video.clear(); m_wallOriginUs = m_mediaEndUs = 0;
    m_audioEndSamples = -1;
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
    qint64 origin, end; bool audio, eof;
    { QMutexLocker lock(&m_mutex); if (!m_accepting) return m_stats.positionUs;
      origin = m_wallOriginUs; audio = m_audioActive; end = m_mediaEndUs; eof = m_decodeEof; }
    if (!origin) return 0;
    qint64 result = std::max<qint64>(0, nowUs() - origin);
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
    result.audioUnderruns = m_audioActive ? audio.underruns : 0; return result;
}
PlaybackVideo MediaPlayer::takeVideo()
{
    const qint64 clock = clockUs(); PlaybackVideo result;
    QMutexLocker lock(&m_mutex);
    if (!m_accepting) return result;
    // 5. 未来帧留队列；明显迟到且有替代帧时丢旧帧，最后一张仍交付并保留。
    while (!m_video.empty() && m_video.front().ptsUs <= clock) {
        if (result.source.texture) ++m_stats.droppedVideo;
        result = std::move(m_video.front()); m_video.pop_front();
        if (clock - result.ptsUs <= 80000) break;
    }
    if (result.source.texture) { ++m_stats.displayedVideo; m_changed.wakeAll(); }
    return result;
}
void MediaPlayer::pushVideo(const PlaybackVideo &frame)
{
    QMutexLocker lock(&m_mutex);
    // 4. 解码背压只挡工作线程；音轨位于后面时先建立静音时钟，GUI才能消费早期视频。
    while (m_accepting && m_video.size() >= 16 && m_audioError.isEmpty()) {
        if (m_audioActive) m_audio.forceStart();
        m_changed.wait(&m_mutex, 10);
    }
    if (!m_audioError.isEmpty()) throw std::runtime_error(m_audioError.toStdString());
    if (!m_accepting || isInterruptionRequested()) throw std::runtime_error("播放已停止");
    m_video.push_back(frame); ++m_stats.decodedVideo;
    m_mediaEndUs = std::max(m_mediaEndUs, frame.ptsUs + frame.durationUs);
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
            [this](const PlaybackVideo &frame) { pushVideo(frame); }, [this](const PlaybackAudio &packet) { pushAudio(packet); });
        {
            QMutexLocker lock(&m_mutex); m_audioActive = info.audio && m_audioRequested;
            m_audioEndSamples = info.audioEndSamples; m_wallOriginUs = nowUs();
        }
        if (!isInterruptionRequested()) {
            if (m_audioActive && !m_audio.begin(info.audioStartSamples)) throw std::runtime_error("音频输出线程尚未退出");
            emit opened(info);
            while (!isInterruptionRequested()) { checkAudioError(); if (!decoder.readNext()) { endOfFile = true; break; } }
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
    if (!error.isEmpty() && !isInterruptionRequested()) emit failed(tr("点播失败：%1").arg(error));
    else emit completed(endOfFile && !isInterruptionRequested());
    // 8. run返回后QThread发finished，Controller才解锁下一轮输入。
}
}
