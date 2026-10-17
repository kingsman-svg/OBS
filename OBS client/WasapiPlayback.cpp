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
bool WasapiPlayback::begin(qint64 startSample, bool live, bool waitForVideo, bool lowLatency, bool adaptiveBuffer)
{
    // 1. MediaPlayer::run调用；上一轮必须已wait结束，工作线程不会看到旧PCM/时钟。
    QMutexLocker lock(&m_mutex);
    if (isRunning()) return false;
    m_packets.clear(); m_queuedFrames = m_offset = 0; m_clock = {};
    m_startSample = std::max<qint64>(0, startSample);
    m_accepting = true; m_finishing = false; m_force = false; m_live = live; m_videoReady = !waitForVideo;
    m_lowLatency = live && lowLatency; m_audioGap = false;
    m_adaptive = m_lowLatency && adaptiveBuffer; m_buffer.begin(nowUs());
    m_clock.adaptive = m_adaptive; m_clock.buffering = m_adaptive; m_clock.targetUs = m_adaptive ? m_buffer.targetUs() : 0;
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
        const qint64 limit = m_adaptive ? PlaybackBuffer::CapacityUs * 48000 / 1000000 : m_lowLatency ? 5760 : 9600;
        if (frames > limit) {
            const qint64 skip = frames - limit;
            accepted.pcm.remove(0, skip * 8); accepted.pts += skip; frames = limit; m_clock.droppedSamples += quint64(skip);
            m_audioGap = true;
        }
        // 2a. 自适应固定300ms安全上限，目标50～150ms；固定低延迟120ms，只裁未提交PCM。
        while (!m_packets.empty() && m_queuedFrames + frames > limit) {
            const auto skipped = m_packets.front().pcm.size() / 8 - m_offset;
            m_queuedFrames -= skipped; m_clock.droppedSamples += quint64(skipped); m_packets.pop_front(); m_offset = 0;
            m_audioGap = true;
        }
    }
    if (m_adaptive) m_buffer.observe(accepted.pts * 1000000 / 48000, frames * 1000000 / 48000, nowUs());
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
    QMutexLocker lock(&m_mutex); auto result = m_clock; result.queuedUs = m_queuedFrames * 1000000 / 48000;
    if (m_adaptive) {
        result.targetUs = m_buffer.targetUs(); result.deliveryJitterUs = m_buffer.jitterUs();
        result.bufferIncreases = m_buffer.increases(); result.bufferDecreases = m_buffer.decreases();
    }
    return result;
}

void WasapiPlayback::dropFrames(qint64 count)
{
    while (count > 0 && !m_packets.empty()) {
        const auto skip = std::min(count, m_packets.front().pcm.size() / 8 - m_offset);
        m_offset += skip; m_queuedFrames -= skip; count -= skip; m_clock.droppedSamples += quint64(skip);
        if (m_offset == m_packets.front().pcm.size() / 8) { m_packets.pop_front(); m_offset = 0; }
    }
}

void WasapiPlayback::renderAdaptive(IAudioClient *client, IAudioRenderClient *render, IAudioClock *clock,
                                   void *event, quint32 capacity, quint64 frequency)
{
    // 1. run已创建COM/端点；解码线程只push，设备调用、硬件时钟和状态转换均由本输出线程负责。
    qint64 submitted = 0, mediaBase = 0, rebufferSince = -1;
    bool running = false, everStarted = false, draining = false, trim = false, refill = false;
    qint64 fade = 0;
    const auto deviceTarget = std::min<quint32>(capacity, 1440); // 设备仍最多30ms，目标在应用层调整。
    while (!isInterruptionRequested()) {
        UINT32 padding = 0; winrt::check_hresult(client->GetCurrentPadding(&padding));
        // 2. 每次先读取实际设备进度。重新缓冲Stop/Reset后用mediaBase映射到原媒体PTS，保持单调。
        if (running) {
            UINT64 position = 0; winrt::check_hresult(clock->GetPosition(&position, nullptr));
            const auto time = mediaBase * 1000000 / 48000
                + qint64(position / frequency * 1000000 + position % frequency * 1000000 / frequency);
            QMutexLocker lock(&m_mutex);
            m_clock.timeUs = std::max(m_clock.timeUs, std::min(time, submitted * 1000000 / 48000));
        }
        bool reset = false, wait = false, finished = false;
        {
            QMutexLocker lock(&m_mutex);
            m_clock.devicePaddingUs = qint64(padding) * 1000000 / 48000;
            const auto now = nowUs(), ready = (m_queuedFrames + padding) * 1000000 / 48000;
            const auto previousTarget = m_buffer.targetUs();
            m_buffer.update(now, ready);
            finished = m_finishing && m_packets.empty();
            if (finished && !padding) break;
            // 2a. 提高目标必须真正补水位：受控排空后重新预缓冲，不只修改诊断数字。
            if (running && !draining && !m_finishing && m_buffer.targetUs() > previousTarget && ready < m_buffer.targetUs()) {
                draining = refill = true;
            }
            // 3. 超目标持续积压/队列硬裁剪/PTS跳变：先停止填设备，待旧padding排空再改媒体映射。
            if (running && !draining && !m_finishing && m_buffer.catchup(now, ready)) { draining = true; trim = true; }
            if (running && !m_packets.empty() && (m_audioGap || m_packets.front().pts + m_offset > submitted)) draining = true;
            if (running && (draining || m_packets.empty())) {
                if (padding) wait = true;
                else {
                    reset = true;
                    if ((!draining || refill) && !m_finishing) {
                        if (!draining) { ++m_clock.underruns; m_buffer.underrun(now); }
                        ++m_clock.rebuffers;
                        m_clock.buffering = true; rebufferSince = now;
                    }
                }
            }
            if (!running && !reset) {
                // 4. 首播/恢复都要真正攒够当前目标；保持设备和视频主时钟冻结，不靠补零假装在播放。
                const auto required = m_buffer.targetUs() * 48000 / 1000000;
                wait = m_packets.empty() || ((!m_videoReady || m_queuedFrames < required) && !m_finishing && !m_force);
                if (!wait) {
                    submitted = mediaBase = m_packets.front().pts + m_offset;
                    m_audioGap = false; // 首播/恢复直接锚定保留PCM，不能把启动前裁剪再当一次运行中追赶。
                    fade = 144; // 恢复时3ms淡入，减轻跳转边沿；不是变速/时间拉伸。
                }
            }
        }
        if (reset) {
            // 5. Stop冻结设备时钟，Reset只在padding排空后执行；不能一边播放旧声音一边跳视频时钟。
            winrt::check_hresult(client->Stop()); winrt::check_hresult(client->Reset()); running = false;
            QMutexLocker lock(&m_mutex);
            if (trim) dropFrames(std::max<qint64>(0, m_queuedFrames - m_buffer.targetUs() * 48000 / 1000000));
            if (draining && !refill) ++m_clock.catchups; // 只为补足更高目标而暂停，不计成媒体跳转。
            m_audioGap = false; draining = trim = refill = false; m_clock.devicePaddingUs = 0; m_clock.buffering = true;
            continue;
        }
        if (wait || finished) { WaitForSingleObject(event, 10); continue; }
        UINT32 available = padding >= deviceTarget ? 0 : deviceTarget - padding;
        {
            QMutexLocker lock(&m_mutex);
            // 6. 不向端点补断粮静音。一次只借当前确实有的PCM，未来PTS缺口在下轮排空/重映射。
            available = UINT32(std::min<qint64>(available, m_queuedFrames));
        }
        if (available) {
            BYTE *bytes = nullptr; winrt::check_hresult(render->GetBuffer(available, &bytes));
            UINT32 written = 0;
            try {
                QMutexLocker lock(&m_mutex);
                while (written < available && !m_packets.empty() && !isInterruptionRequested()) {
                    auto &packet = m_packets.front();
                    if (packet.pts + m_offset < submitted) { dropFrames(std::min(packet.pcm.size() / 8 - m_offset, submitted - packet.pts - m_offset)); continue; }
                    if (packet.pts + m_offset > submitted) { draining = true; break; }
                    const auto count = UINT32(std::min<qint64>(available - written, packet.pcm.size() / 8 - m_offset));
                    const auto volume = m_volume.load();
                    for (UINT32 i = 0; i < count; ++i) {
                        const float gain = volume * (fade ? float(145 - fade) / 144 : 1.0f);
                        if (fade) --fade;
                        for (int channel = 0; channel < 2; ++channel) {
                            float value = 0; std::memcpy(&value, packet.pcm.constData() + (m_offset + i) * 8 + channel * 4, 4);
                            value = std::isfinite(value) ? std::clamp(value * gain, -1.0f, 1.0f) : 0;
                            std::memcpy(bytes + (written + i) * 8 + channel * 4, &value, 4);
                        }
                    }
                    written += count; submitted += count; m_offset += count; m_queuedFrames -= count; m_clock.renderedSamples += count;
                    if (m_offset == packet.pcm.size() / 8) { m_packets.pop_front(); m_offset = 0; }
                }
                m_changed.wakeAll();
            } catch (...) { render->ReleaseBuffer(0, 0); throw; }
            winrt::check_hresult(render->ReleaseBuffer(written, 0));
            if (written && !running) {
                winrt::check_hresult(client->Start()); running = everStarted = true;
                QMutexLocker lock(&m_mutex); m_clock.started = true; m_clock.buffering = false;
                if (rebufferSince >= 0) { m_clock.rebufferUs += nowUs() - rebufferSince; rebufferSince = -1; }
            }
        }
        WaitForSingleObject(event, 10);
    }
    // 7. stop随时打断重新缓冲；自然EOF排空已有媒体，不等不可能再补齐的目标。资源仍由run归还。
    winrt::check_hresult(client->Stop());
    QMutexLocker lock(&m_mutex);
    if (rebufferSince >= 0) m_clock.rebufferUs += nowUs() - rebufferSince;
    if (!isInterruptionRequested() && everStarted) m_clock.timeUs = std::max(m_clock.timeUs, submitted * 1000000 / 48000);
    m_clock.buffering = false; m_clock.ended = true; m_clock.endedQpcUs = nowUs();
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
            if (m_adaptive) {
                renderAdaptive(client.Get(), render.Get(), clock.Get(), event.get(), capacity, frequency);
            } else {
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
            }
        } catch (const winrt::hresult_error &failure) { error = QString::fromStdWString(failure.message().c_str()); }
          catch (const std::exception &failure) { error = QString::fromUtf8(failure.what()); }
        if (client) client->Stop();
    }
    CoUninitialize();
    { QMutexLocker lock(&m_mutex); m_accepting = false; m_packets.clear(); m_queuedFrames = m_offset = 0; m_changed.wakeAll(); }
    if (!error.isEmpty() && !isInterruptionRequested()) emit failed(tr("音频播放失败：%1").arg(error));
}
}
