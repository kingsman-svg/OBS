#include "WasapiPlayback.h"
#include <QMutexLocker>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace csn {
namespace {
using Microsoft::WRL::ComPtr;
qint64 nowUs()
{
    LARGE_INTEGER tick{}, frequency{}; QueryPerformanceCounter(&tick); QueryPerformanceFrequency(&frequency);
    return tick.QuadPart / frequency.QuadPart * 1000000 + tick.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart;
}
struct EventDelete { void operator()(void *handle) const { if (handle) CloseHandle(handle); } };
}
WasapiPlayback::WasapiPlayback() = default;
WasapiPlayback::~WasapiPlayback() { stop(); wait(); }
bool WasapiPlayback::begin(qint64 startSample, bool live, bool waitForVideo, bool lowLatency)
{
    // 1. MediaPlayer::run调用；上一轮必须已wait结束，工作线程不会看到旧PCM/时钟。
    QMutexLocker lock(&m_mutex);
    if (isRunning()) return false;
    m_packets.clear(); m_queuedFrames = m_offset = 0; m_clock = {};
    m_startSample = std::max<qint64>(0, startSample);
    m_accepting = true; m_finishing = false; m_force = false; m_live = live; m_videoReady = !waitForVideo;
    m_lowLatency = live && lowLatency; m_audioGap = false;
    m_prebufferFrames = m_lowLatency ? 2400 : waitForVideo && !live ? 9600 : 4800;
    m_clock.prebufferUs = m_prebufferFrames * 1000000 / 48000; start(); return true;
}
bool WasapiPlayback::push(const PlaybackAudio &packet)
{
    qint64 frames = packet.pcm.size() / 8;
    if (packet.pts < 0 || packet.pcm.size() % 8 || frames <= 0 || frames > 48000)
        throw std::invalid_argument("播放PCM必须是有PTS的48kHz立体声Float32，单包不超过1秒");
    QMutexLocker lock(&m_mutex);
    // 2. 点播保留未到期数据，用背压限制内存；用户停止会关闭入口并唤醒等待者。
    while (!m_live && m_accepting && m_queuedFrames + frames > 48000) {
        // 音轨领先视频超过1秒时，先消费已到PCM；否则读包线程无法读到首张视频。
        if (!m_videoReady) { m_force = true; m_changed.wakeAll(); }
        m_changed.wait(&m_mutex);
    }
    if (!m_accepting) return false;
    PlaybackAudio accepted = packet;
    if (m_live) {
        const qint64 limit = m_lowLatency ? 5760 : 9600;
        if (frames > limit) {
            const qint64 skip = frames - limit;
            accepted.pcm.remove(0, skip * 8); accepted.pts += skip; frames = limit; m_clock.droppedSamples += quint64(skip);
            m_audioGap = true;
        }
        // 2a. 低延迟直播最多120ms，旧预算200ms；只裁未播放PCM，端点按PTS追赶。
        while (!m_packets.empty() && m_queuedFrames + frames > limit) {
            const auto skipped = m_packets.front().pcm.size() / 8 - m_offset;
            m_queuedFrames -= skipped; m_clock.droppedSamples += quint64(skipped); m_packets.pop_front(); m_offset = 0;
            m_audioGap = true;
        }
    }
    m_packets.push_back(std::move(accepted)); m_queuedFrames += frames; m_changed.wakeAll(); return true;
}
void WasapiPlayback::forceStart() { QMutexLocker lock(&m_mutex); m_force = true; m_changed.wakeAll(); }
void WasapiPlayback::videoReady() { QMutexLocker lock(&m_mutex); m_videoReady = true; m_changed.wakeAll(); }
void WasapiPlayback::finish() { QMutexLocker lock(&m_mutex); m_accepting = false; m_finishing = true; m_changed.wakeAll(); }
void WasapiPlayback::stop()
{
    requestInterruption(); QMutexLocker lock(&m_mutex);
    m_accepting = false; m_packets.clear(); m_queuedFrames = m_offset = 0; m_changed.wakeAll();
}
void WasapiPlayback::setVolume(float value) { m_volume.store(std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f); }
PlaybackClock WasapiPlayback::clock() const
{
    QMutexLocker lock(&m_mutex); auto result = m_clock; result.queuedUs = m_queuedFrames * 1000000 / 48000; return result;
}

void WasapiPlayback::run()
{
    // 3. COM及端点接口仅在音频工作线程创建/释放；不跨线程调用IAudioClient。
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) { emit failed(tr("音频输出COM初始化失败")); return; }
    QString error;
    {
        ComPtr<IAudioClient> client;
        try {
            ComPtr<IMMDeviceEnumerator> enumerator;
            winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
            ComPtr<IMMDevice> device;
            winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
            winrt::check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
            // 4. 固定48kHz双声道Float32；共享模式允许Windows转换到端点实际格式。
            WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            format.nChannels = 2; format.nSamplesPerSec = 48000; format.wBitsPerSample = 32;
            format.nBlockAlign = 8; format.nAvgBytesPerSec = 48000 * 8;
            winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                m_lowLatency ? 300000 : 1000000, 0, &format, nullptr)); // 低延迟请求30ms，实际容量随后读取。
            std::unique_ptr<void, EventDelete> event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (!event) throw std::runtime_error("创建音频输出事件失败");
            winrt::check_hresult(client->SetEventHandle(event.get()));
            UINT32 capacity = 0; winrt::check_hresult(client->GetBufferSize(&capacity));
            { QMutexLocker lock(&m_mutex); m_clock.deviceBufferUs = qint64(capacity) * 1000000 / 48000; }
            ComPtr<IAudioRenderClient> render; winrt::check_hresult(client->GetService(IID_PPV_ARGS(&render)));
            ComPtr<IAudioClock> clock; winrt::check_hresult(client->GetService(IID_PPV_ARGS(&clock)));
            UINT64 frequency = 0; winrt::check_hresult(clock->GetFrequency(&frequency));
            if (!frequency || !capacity) throw std::runtime_error("音频输出时钟/缓冲无效");
            // 5. 等首张视频转换完成再建立声音时钟；低延迟直播预缓冲50ms，点播预算保持原值。
            {
                QMutexLocker lock(&m_mutex);
                while ((m_queuedFrames < m_prebufferFrames || !m_videoReady) && !m_finishing && !m_force && !isInterruptionRequested())
                    m_changed.wait(&m_mutex, 10);
            }
            qint64 submitted = 0, mediaOffset = 0; // 直播追赶时媒体位置跳转，设备时钟仍连续。
            bool started = false;
            while (!isInterruptionRequested()) {
                UINT32 padding = 0; winrt::check_hresult(client->GetCurrentPadding(&padding));
                { QMutexLocker lock(&m_mutex); m_clock.devicePaddingUs = qint64(padding) * 1000000 / 48000; }
                bool finished;
                { QMutexLocker lock(&m_mutex); finished = m_finishing && m_packets.empty(); }
                if (finished && padding == 0) break;
                UINT32 available = capacity - padding;
                if (m_lowLatency) {
                    // 5a. 驱动容量可能大于请求；只维持最多30ms已提交音频，避免填满整个大端点缓冲。
                    const auto target = std::min<UINT32>(capacity, 1440);
                    available = padding >= target ? 0 : std::min(available, target - padding);
                }
                if (available && !finished) {
                    BYTE *bytes = nullptr; winrt::check_hresult(render->GetBuffer(available, &bytes));
                    UINT32 written = 0;
                    try {
                        QMutexLocker lock(&m_mutex);
                        // 6. PTS缺口逐段填零，重叠/迟到包裁掉已播放区间；不分配大段静音内存。
                        while (written < available) {
                            if (m_packets.empty()) {
                                if (m_finishing || isInterruptionRequested()) break;
                                std::memset(bytes + written * 8, 0, size_t(available - written) * 8);
                                m_clock.silenceSamples += available - written;
                                submitted += available - written; written = available;
                                if (started && submitted > m_startSample) {
                                    ++m_clock.underruns;
                                    if (qEnvironmentVariableIsSet("OBS_DECODE_TRACE")) qInfo("audio underrun: submitted=%lld force=%d", submitted, m_force);
                                }
                                break;
                            }
                            auto &packet = m_packets.front();
                            const qint64 total = packet.pcm.size() / 8;
                            if (packet.pts + m_offset < submitted) {
                                const qint64 skip = std::min(total - m_offset, submitted - packet.pts - m_offset);
                                m_offset += skip; m_queuedFrames -= skip;
                            }
                            if (m_offset == total) { m_packets.pop_front(); m_offset = 0; continue; }
                            if (packet.pts + m_offset > submitted) {
                                if (m_live && (m_audioGap || !started || packet.pts + m_offset - submitted > (m_lowLatency ? 5760 : 9600))) {
                                    // 6a. 主动裁剪后立即跟随首个保留PTS；启动也无需补播加入直播之前的历史静音。
                                    const qint64 skip = packet.pts + m_offset - submitted;
                                    submitted += skip; mediaOffset += skip; m_audioGap = false; ++m_clock.catchups; continue;
                                }
                                const auto zeros = UINT32(std::min<qint64>(available - written, packet.pts + m_offset - submitted));
                                m_clock.silenceSamples += zeros;
                                std::memset(bytes + written * 8, 0, size_t(zeros) * 8); written += zeros; submitted += zeros; continue;
                            }
                            const auto count = UINT32(std::min<qint64>(available - written, total - m_offset));
                            m_audioGap = false; m_clock.renderedSamples += count;
                            const float volume = m_volume.load();
                            for (UINT32 i = 0; i < count * 2; ++i) {
                                float value = 0; std::memcpy(&value, packet.pcm.constData() + m_offset * 8 + i * 4, 4);
                                value = std::isfinite(value) ? std::clamp(value * volume, -1.0f, 1.0f) : 0;
                                std::memcpy(bytes + written * 8 + i * 4, &value, 4);
                            }
                            written += count; submitted += count; m_offset += count; m_queuedFrames -= count;
                            if (m_offset == total) { m_packets.pop_front(); m_offset = 0; }
                        }
                        m_changed.wakeAll();
                    } catch (...) { render->ReleaseBuffer(0, 0); throw; }
                    winrt::check_hresult(render->ReleaseBuffer(written, 0));
                    if (!started && written) {
                        winrt::check_hresult(client->Start()); started = true;
                        QMutexLocker lock(&m_mutex); m_clock.started = true;
                    }
                }
                // 7. GetPosition/frequency是实际设备进度，不能用写入帧数冒充已播放帧数。
                if (started) {
                    UINT64 position = 0; winrt::check_hresult(clock->GetPosition(&position, nullptr));
                    const qint64 time = qint64(position / frequency * 1000000 + position % frequency * 1000000 / frequency)
                        + mediaOffset * 1000000 / 48000;
                    QMutexLocker lock(&m_mutex);
                    m_clock.timeUs = std::max(m_clock.timeUs, std::min(time, submitted * 1000000 / 48000));
                }
                WaitForSingleObject(event.get(), 10); // 事件驱动，超时也检查stop与尾部padding。
            }
            // 8. 自然EOF排空后停止；用户stop不等剩余声音，随后释放全部接口及COM。
            if (client) client->Stop();
            QMutexLocker lock(&m_mutex);
            if (!isInterruptionRequested()) m_clock.timeUs = submitted * 1000000 / 48000;
            m_clock.ended = true; m_clock.endedQpcUs = nowUs();
        } catch (const winrt::hresult_error &failure) { error = QString::fromStdWString(failure.message().c_str()); }
          catch (const std::exception &failure) { error = QString::fromUtf8(failure.what()); }
        if (client) client->Stop();
    }
    CoUninitialize();
    { QMutexLocker lock(&m_mutex); m_accepting = false; m_packets.clear(); m_queuedFrames = m_offset = 0; m_changed.wakeAll(); }
    if (!error.isEmpty() && !isInterruptionRequested()) emit failed(tr("音频播放失败：%1").arg(error));
}
}
