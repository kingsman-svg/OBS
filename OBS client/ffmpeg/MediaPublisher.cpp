#include "MediaPublisher.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <QUrl>
#include <winrt/base.h>
#include <optional>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace csn {
namespace {
qint64 nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
qint64 mediaUs(qint64 pts, int rate) { return av_rescale_q(pts, {1, rate}, {1, 1000000}); }
template<class Queue>
qint64 catchupCutoff(const Queue &queue, int rate, int duration, qint64 now, qint64 limit, qint64 target)
{
    if (queue.empty()) return -1;
    const auto end = [&](const auto &entry) { return mediaUs(entry.value.pts + duration, rate); };
    const auto span = end(queue.back()) - mediaUs(queue.front().value.pts, rate);
    if (span <= limit && now - queue.front().enteredUs <= limit) return -1;
    // 1a. 时间跨度处理突发交付，单调时钟处理网络等待；整批都老时允许清空，绝不发送最后一张过期帧。
    qint64 cutoff = std::max<qint64>(0, end(queue.back()) - target);
    for (const auto &entry : queue) {
        if (now - entry.enteredUs <= target) break;
        cutoff = std::max(cutoff, end(entry));
    }
    return cutoff;
}
void check(int result, const char *operation)
{
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
// 本地MP4用Qt文件IO支持中文路径；AVIO回调不会跨出编码工作线程，也不能抛异常。
int writeFile(void *opaque, const uint8_t *bytes, int count)
{
    auto *file = static_cast<QTemporaryFile *>(opaque);
    return file->write(reinterpret_cast<const char *>(bytes), count) == count ? count : AVERROR(EIO);
}
int64_t seekFile(void *opaque, int64_t offset, int whence)
{
    auto *file = static_cast<QTemporaryFile *>(opaque);
    if (whence & AVSEEK_SIZE) return file->size();
    whence &= ~AVSEEK_FORCE;
    const qint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? file->pos() : whence == SEEK_END ? file->size() : -1;
    if (base < 0 || offset < -base || (offset > 0 && base > INT64_MAX - offset)) return AVERROR(EINVAL);
    return file->seek(base + offset) ? base + offset : AVERROR(EIO);
}
struct Muxer {
    AVFormatContext *format = nullptr; // 独占输出上下文，streams由FFmpeg释放。
    AVIOContext *io = nullptr;         // 独占自定义IO，format只借用，不能avio_closep。
    ~Muxer() {
        if (format && !io && format->pb) avio_closep(&format->pb); // 网络IO由FFmpeg拥有。
        avformat_free_context(format);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    }
};
struct NetworkIo {
    std::atomic<bool> *cancel = nullptr; // 借用Publisher的取消标记，只在run期间存在。
    qint64 deadlineMs = 0;              // 每次连接/写出最多5秒，用单调时钟。
    QElapsedTimer timer;
    void renew() { deadlineMs = timer.elapsed() + 5000; }
    static int interrupt(void *opaque) {
        const auto &io = *static_cast<NetworkIo *>(opaque);
        return io.cancel->load() || io.timer.elapsed() >= io.deadlineMs;
    }
};
}

MediaPublisher::MediaPublisher(QObject *parent) : QThread(parent)
{
    qRegisterMetaType<EncodingStats>();
}
MediaPublisher::~MediaPublisher() { stop(); wait(); }

bool MediaPublisher::begin(const RecordingSettings &settings)
{
    // 1. begin在GUI调用；Controller必须等上轮finished回到GUI后才能重新调用。
    QMutexLocker lock(&m_mutex);
    if (isRunning()) return false;
    if (settings.live && (settings.liveQueueTargetMs < 10 || settings.liveQueueLimitMs > 5000
        || settings.liveQueueTargetMs >= settings.liveQueueLimitMs)) return false;
    const BitrateControl control{settings.adaptiveBitrate, settings.adaptiveBitrate ? settings.minimumVideoBitrate : MinimumVideoBitrate, settings.videoBitrate};
    if ((settings.adaptiveBitrate && (!settings.live || !settings.video)) || !AdaptiveBitrate::valid(control)) return false;
    m_settings = settings; m_video.clear(); m_audio.clear(); m_stats = {};
    m_outputSamples.clear(); m_pendingBitrateControl.reset(); m_writeStartedUs = m_bitratePeakWriteUs = 0;
    m_videoCutoffUs = m_audioCutoffUs = 0; m_videoGap = m_audioGap = false;
    m_stats.requestedVideoBitrate = settings.video ? settings.videoBitrate : 0;
    m_stats.targetVideoBitrate = m_stats.requestedVideoBitrate;
    m_stats.automaticBitrate = settings.adaptiveBitrate;
    m_stats.bitrateState = settings.adaptiveBitrate ? BitrateState::Observing : BitrateState::Manual;
    m_stats.muxInterleaveMs = settings.live && settings.lowLatency ? 40 : 100;
    m_accepting = true; m_finishing = false; m_aborted = false; m_cancelIo.store(false);
    start(); return true; // 2. QThread建立工作线程，随后在该线程调用run。
}
void MediaPublisher::pushVideo(const TimedVideoFrame &frame)
{
    QMutexLocker lock(&m_mutex);
    if (!m_accepting || !m_settings.video) return;
    if (m_settings.live && frame.pts >= 0 && mediaUs(frame.pts, 30) < m_videoCutoffUs) {
        ++m_stats.droppedVideo; ++m_stats.expiredVideo; return; // 已越过的时间不接受迟到数据。
    }
    const auto now = nowUs(); m_video.push_back({frame, now}); trimLiveQueues(now);
    if (m_video.size() > 16) { m_video.pop_front(); ++m_stats.droppedVideo; m_videoGap = m_settings.live; }
    m_stats.peakQueuedVideo = std::max(m_stats.peakQueuedVideo, quint64(m_video.size()));
    m_ready.wakeOne();
}
void MediaPublisher::pushAudio(const TimedAudioPacket &packet)
{
    QMutexLocker lock(&m_mutex);
    if (!m_accepting || !m_settings.audio) return;
    if (m_settings.live && packet.pts >= 0 && mediaUs(packet.pts, 48000) < m_audioCutoffUs) {
        ++m_stats.droppedAudio; ++m_stats.expiredAudio; return;
    }
    const auto now = nowUs(); m_audio.push_back({packet, now}); trimLiveQueues(now);
    if (m_audio.size() > 50) { m_audio.pop_front(); ++m_stats.droppedAudio; m_audioGap = m_settings.live; }
    m_stats.peakQueuedAudio = std::max(m_stats.peakQueuedAudio, quint64(m_audio.size()));
    m_ready.wakeOne();
}
void MediaPublisher::trimLiveQueues(qint64 now)
{
    // 2a. 本地MP4保持原策略；仅在锁内裁剪有界原始媒体，不触碰编码器/AVIO/TCP中的数据。
    if (!m_settings.live) return;
    const qint64 limit = qint64(m_settings.liveQueueLimitMs) * 1000, target = qint64(m_settings.liveQueueTargetMs) * 1000;
    const auto audio = catchupCutoff(m_audio, 48000, 480, now, limit, target);
    const auto video = catchupCutoff(m_video, 30, 1, now, limit, target);
    if (audio >= 0) {
        m_audioCutoffUs = std::max(m_audioCutoffUs, audio);
        m_videoCutoffUs = std::max(m_videoCutoffUs, m_audioCutoffUs); // 声音也过期时，画面不能留在旧区间。
    }
    if (video >= 0) m_videoCutoffUs = std::max(m_videoCutoffUs, video); // 仅视频积压不主动丢连续音频。
    const auto before = m_stats.expiredVideo + m_stats.expiredAudio;
    while (!m_video.empty() && mediaUs(m_video.front().value.pts, 30) < m_videoCutoffUs) {
        m_video.pop_front(); ++m_stats.droppedVideo; ++m_stats.expiredVideo; m_videoGap = true;
    }
    while (!m_audio.empty() && mediaUs(m_audio.front().value.pts, 48000) < m_audioCutoffUs) {
        m_audio.pop_front(); ++m_stats.droppedAudio; ++m_stats.expiredAudio; m_audioGap = true;
    }
    if (m_stats.expiredVideo + m_stats.expiredAudio != before) ++m_stats.liveCatchups;
}
bool MediaPublisher::requestVideoBitrate(int bitsPerSecond)
{
    // 2b. 兼容原手动入口，明确切回固定目标；GUI与策略共享同一条工作线程提交路径。
    return requestBitrateControl({false, MinimumVideoBitrate, bitsPerSecond});
}
bool MediaPublisher::requestBitrateControl(const BitrateControl &control)
{
    if (!AdaptiveBitrate::valid(control)) return false;
    QMutexLocker lock(&m_mutex);
    if (!m_accepting || !m_settings.video || (control.automatic && !m_settings.live)) return false;
    m_pendingBitrateControl = control; m_stats.requestedVideoBitrate = control.maximum;
    m_ready.wakeOne(); return true;
}
BitrateFeedback MediaPublisher::bitrateFeedback(qint64 now) const
{
    BitrateFeedback result;
    result.videoFrames = m_stats.videoFrames; result.outputBytes = m_stats.outputBytes;
    result.expired = m_stats.expiredVideo + m_stats.expiredAudio;
    result.writeUs = m_stats.totalWriteUs; result.videoProcessUs = m_stats.totalVideoEncodeUs;
    result.peakWriteUs = m_bitratePeakWriteUs; result.videoBitrate = m_stats.videoPayloadBitrate;
    if (!m_video.empty()) result.queueWaitMs = std::max<qint64>(0, now - m_video.front().enteredUs) / 1000;
    if (!m_audio.empty()) result.queueWaitMs = std::max(result.queueWaitMs, std::max<qint64>(0, now - m_audio.front().enteredUs) / 1000);
    return result;
}
void MediaPublisher::publishBitrateState(const AdaptiveBitrate &policy)
{
    // 不覆盖GUI并发提交的用户上限；计划目标/模式与编码器实际提交目标分别展示。
    m_stats.targetVideoBitrate = m_settings.video ? policy.target() : 0;
    m_stats.automaticBitrate = policy.automatic(); m_stats.bitrateState = policy.state();
    m_stats.bitrateReductions = policy.reductions(); m_stats.bitrateIncreases = policy.increases();
    m_stats.bitrateWriteLoad = policy.writeLoad();
}
void MediaPublisher::stop()
{
    QMutexLocker lock(&m_mutex);
    m_accepting = false; m_finishing = true;
    m_pendingBitrateControl.reset(); // 停止后不再将未消费的界面请求应用到排空尾部。
    if (m_settings.live) m_cancelIo.store(true); // 网络停止优先退出，MP4仍正常排空。
    m_ready.wakeOne();
}
void MediaPublisher::abort()
{
    QMutexLocker lock(&m_mutex);
    m_accepting = false; m_aborted = true; m_cancelIo.store(true); m_video.clear(); m_audio.clear(); m_pendingBitrateControl.reset(); m_ready.wakeOne();
}
EncodingStats MediaPublisher::stats() const
{
    // 2c. 队列等待/正在写出均用当前单调时间计算；网络阻塞也不会卡住GUI取统计。
    QMutexLocker lock(&m_mutex); auto result = m_stats; const auto now = nowUs();
    result.bitrateControlPending = m_pendingBitrateControl.has_value();
    result.queuedVideo = m_video.size(); result.queuedAudio = m_audio.size();
    if (!m_video.empty()) {
        result.oldestVideoWaitMs = std::max<qint64>(0, now - m_video.front().enteredUs) / 1000;
        result.videoQueueSpanMs = std::max<qint64>(0, m_video.back().value.pts - m_video.front().value.pts + 1) * 1000 / MediaTimeline::VideoRate;
    }
    if (!m_audio.empty()) {
        result.oldestAudioWaitMs = std::max<qint64>(0, now - m_audio.front().enteredUs) / 1000;
        result.audioQueueSpanMs = std::max<qint64>(0, m_audio.back().value.pts + m_audio.back().value.source.pcm.size() / 8 - m_audio.front().value.pts) * 1000 / MediaTimeline::AudioRate;
    }
    result.writing = m_writeStartedUs != 0; result.currentWriteUs = result.writing ? now - m_writeStartedUs : 0;
    quint64 bytes = 0; for (const auto &sample : m_outputSamples) if (sample.endedUs > now - 1000000) bytes += sample.bytes;
    result.outputBitrate = bytes * 8; return result; // 固定1秒墙钟窗口，空闲后自然降到0。
}
void MediaPublisher::publishStats(const EncodingStats &value)
{
    QMutexLocker lock(&m_mutex);
    // 编码器只拥有这些字段；逐项合并，不能覆盖GUI同时提交的目标、队列/IO统计。
    m_stats.videoFrames = value.videoFrames; m_stats.audioSamples = value.audioSamples; m_stats.silentSamples = value.silentSamples;
    m_stats.packets = value.packets; m_stats.bytes = value.bytes; m_stats.encoderVideoBitrate = value.encoderVideoBitrate;
    m_stats.audioResyncs = value.audioResyncs; m_stats.skippedAudioSamples = value.skippedAudioSamples;
    m_stats.videoPayloadBitrate = value.videoPayloadBitrate; m_stats.videoRateWindowMs = value.videoRateWindowMs;
    m_stats.encoderVbvMs = value.encoderVbvMs; m_stats.encoderGopFrames = value.encoderGopFrames;
}
void MediaPublisher::finishWrite(qint64 startedUs, quint64 bytes, bool failed)
{
    const auto ended = nowUs(), duration = ended - startedUs;
    QMutexLocker lock(&m_mutex); m_writeStartedUs = 0; ++m_stats.writeCalls;
    m_stats.lastWriteUs = duration; m_stats.maxWriteUs = std::max(m_stats.maxWriteUs, duration); m_stats.totalWriteUs += duration;
    m_bitratePeakWriteUs = std::max(m_bitratePeakWriteUs, duration);
    if (failed) ++m_stats.writeFailures;
    if (m_settings.live) {
        m_stats.outputBytes += bytes; m_outputSamples.push_back({ended, bytes});
        while (!m_outputSamples.empty() && m_outputSamples.front().endedUs <= ended - 1000000) m_outputSamples.pop_front();
    }
}

void MediaPublisher::run()
{
    QString savedPath, error;
    const auto runStarted = nowUs(); // 只测本进程初始化阶段，不与媒体PTS相减。
    try {
        const auto settings = m_settings;
        // 3. 视频编码必须与首帧共用设备；不在构造/GUI线程提前创建另一个D3D11设备。
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (settings.video) {
            QElapsedTimer elapsed; elapsed.start();
            QMutexLocker lock(&m_mutex);
            while (m_video.empty() && !m_finishing && !m_aborted) {
                if (elapsed.elapsed() >= 5000) throw std::runtime_error("录制等待首个视频帧超时，采集预览仍可继续");
                m_ready.wait(&m_mutex, 50);
            }
            if (!m_video.empty() && m_video.front().value.source.video.texture)
                m_video.front().value.source.video.texture->GetDevice(&device);
        }
        bool proceed;
        { QMutexLocker lock(&m_mutex); proceed = !m_aborted && (!settings.video || device); }
        if (proceed) {
            // 4. 同目录临时文件先完成trailer再改名；已有目标绝不覆盖，失败不留下半成品MP4。
            const QFileInfo destination(settings.path);
            if (!settings.live && (settings.path.isEmpty() || destination.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) != 0))
                throw std::invalid_argument("本步录制输出必须是本地.mp4路径");
            if (!settings.live && destination.exists()) throw std::runtime_error("录制目标已存在，请使用新的文件名");
            if (!settings.live && !QDir().mkpath(destination.absolutePath())) throw std::runtime_error("无法创建录制目录");
            QTemporaryFile file(QDir(destination.absolutePath()).filePath(QStringLiteral(".obs-record-XXXXXX.part")));
            NetworkIo network; network.cancel = &m_cancelIo; network.timer.start(); network.renew();
            Muxer mux;
            check(avformat_alloc_output_context2(&mux.format, nullptr, settings.live ? "flv" : "mp4", nullptr), "output context");
            if (settings.live) {
                // 4a. 协议模块只认RTMP，不引用SRS类型；IO归本线程，GUI用interrupt取消。
                const QUrl url(settings.path);
                if (!url.isValid() || url.scheme() != QStringLiteral("rtmp") || url.host().isEmpty()
                    || !url.userInfo().isEmpty() || url.path().count('/') < 2)
                    throw std::invalid_argument("直播需要有效的rtmp://主机/应用/流地址");
                static const int initialized = avformat_network_init(); check(initialized, "network initialize");
                mux.format->interrupt_callback = {NetworkIo::interrupt, &network};
                mux.format->flags |= AVFMT_FLAG_FLUSH_PACKETS;
                AVDictionary *options = nullptr;
                av_dict_set(&options, "rw_timeout", "5000000", 0);
                av_dict_set(&options, "tcp_nodelay", "1", 0); // 直播小包及时交付，减少Nagle等待对发送反馈的干扰。
                av_dict_set(&options, "protocol_whitelist", "rtmp,tcp", 0);
                const auto address = url.toEncoded();
                const auto connecting = nowUs();
                const int result = avio_open2(&mux.format->pb, address.constData(), AVIO_FLAG_WRITE,
                    &mux.format->interrupt_callback, &options);
                { QMutexLocker lock(&m_mutex); m_stats.connectUs = nowUs() - connecting; }
                av_dict_free(&options); check(result, "connect RTMP");
            } else {
                if (!file.open()) throw std::runtime_error(file.errorString().toStdString());
                auto *buffer = static_cast<uint8_t *>(av_malloc(65536));
                if (!buffer) throw std::bad_alloc();
                mux.io = avio_alloc_context(buffer, 65536, 1, &file, nullptr, writeFile, seekFile);
                if (!mux.io) { av_free(buffer); throw std::bad_alloc(); }
                mux.format->pb = mux.io; mux.format->flags |= AVFMT_FLAG_CUSTOM_IO;
            }
            // 限制封装交织缓存；保留各路原始PTS和AAC编码延迟，由MP4编辑列表描述。
            mux.format->max_interleave_delta = settings.live && settings.lowLatency ? 40000 : 100000;
            AVStream *video = nullptr, *audio = nullptr;
            int64_t headerBytes = 0; bool transmitted = false;
            MediaEncoder encoder;
            encoder.open(settings, device.Get(), (mux.format->oformat->flags & AVFMT_GLOBALHEADER) != 0,
                [&](AVPacket *packet, const AVCodecContext *codec) {
                    auto *stream = codec->codec_type == AVMEDIA_TYPE_VIDEO ? video : audio;
                    if (!stream) throw std::logic_error("编码包缺少对应MP4流");
                    av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
                    packet->stream_index = stream->index;
                    network.renew();
                    const auto started = nowUs(), before = mux.format->pb->bytes_written;
                    { QMutexLocker lock(&m_mutex); m_writeStartedUs = started; }
                    try {
                        check(av_interleaved_write_frame(mux.format, packet), "write media packet");
                        if (settings.live) {
                            avio_flush(mux.format->pb); check(mux.format->pb->error, "flush RTMP");
                            if (!transmitted && mux.format->pb->bytes_written > headerBytes) {
                                { QMutexLocker lock(&m_mutex); m_stats.firstMediaUs = nowUs() - runStarted; }
                                transmitted = true; emit transmitting();
                            }
                        }
                    } catch (...) { finishWrite(started, quint64(std::max<int64_t>(0, mux.format->pb->bytes_written - before)), true); throw; }
                    finishWrite(started, quint64(std::max<int64_t>(0, mux.format->pb->bytes_written - before)), false);
                });
            auto addStream = [&](const AVCodecContext *codec) -> AVStream * {
                if (!codec) return nullptr;
                auto *stream = avformat_new_stream(mux.format, nullptr); if (!stream) throw std::bad_alloc();
                check(avcodec_parameters_from_context(stream->codecpar, codec), "MP4 stream parameters");
                stream->time_base = codec->time_base; return stream;
            };
            video = addStream(encoder.videoContext()); audio = addStream(encoder.audioContext());
            network.renew();
            AVDictionary *muxOptions = nullptr;
            if (settings.live) av_dict_set(&muxOptions, "flvflags", "no_duration_filesize", 0);
            const int headerResult = avformat_write_header(mux.format, &muxOptions);
            av_dict_free(&muxOptions); check(headerResult, "write media header");
            if (settings.live) { avio_flush(mux.format->pb); check(mux.format->pb->error, "flush RTMP header"); }
            headerBytes = mux.format->pb->bytes_written;
            AdaptiveBitrate policy; int submittedBitrate = settings.videoBitrate;
            {
                QMutexLocker lock(&m_mutex); const auto now = nowUs();
                policy.begin({settings.adaptiveBitrate, settings.adaptiveBitrate ? settings.minimumVideoBitrate : MinimumVideoBitrate, settings.videoBitrate},
                    submittedBitrate, now, bitrateFeedback(now));
                m_bitratePeakWriteUs = 0; publishBitrateState(policy);
                m_stats.readyUs = nowUs() - runStarted;
            }
            emit opened();
            // 5. 只取队列头的引用后解锁；编码/GPU等待/文件IO都不占用GUI入口的互斥锁。
            for (;;) {
                std::optional<TimedVideoFrame> frame;
                std::optional<TimedAudioPacket> packet;
                bool audioCatchup = false;
                int bitrate = 0;
                {
                    QMutexLocker lock(&m_mutex);
                    // 5a. IO可能刚阻塞过；必须先剔除过期数据，清空后等待新输入，不能空转或编码旧队头。
                    for (;;) {
                        const auto now = nowUs(); trimLiveQueues(now);
                        if (m_pendingBitrateControl) {
                            policy.configure(*m_pendingBitrateControl, policy.target(), now, bitrateFeedback(now));
                            m_pendingBitrateControl.reset(); m_bitratePeakWriteUs = 0;
                        }
                        if (policy.update(now, bitrateFeedback(now))) m_bitratePeakWriteUs = 0;
                        publishBitrateState(policy);
                        if (!m_video.empty() || !m_audio.empty() || m_finishing || m_aborted) break;
                        if (policy.automatic()) m_ready.wait(&m_mutex, 100); // 空闲也形成观察窗口，不把停帧当稳定恢复。
                        else m_ready.wait(&m_mutex); // 手动/录制沿用无数据休眠，不额外轮询。
                    }
                    if (m_aborted || (settings.live && m_cancelIo.load()) || (m_finishing && m_video.empty() && m_audio.empty())) break;
                    const bool takeVideo = !m_video.empty() && (m_audio.empty()
                        || av_compare_ts(m_video.front().value.pts, {1, 30}, m_audio.front().value.pts, {1, 48000}) <= 0);
                    if (takeVideo) {
                        frame = std::move(m_video.front().value); m_video.pop_front();
                        frame->discontinuity = frame->discontinuity || m_videoGap; m_videoGap = false;
                        if (policy.target() != submittedBitrate) bitrate = policy.target();
                    } else {
                        packet = std::move(m_audio.front().value); m_audio.pop_front();
                        audioCatchup = m_audioGap; m_audioGap = false;
                    }
                }
                // 5b. 最新参数只在下一张视频前应用；网络写出返回后仍由此线程处理，GUI不会等待。
                if (bitrate) encoder.setVideoBitrate(bitrate);
                const auto started = nowUs(); qint64 writeBefore;
                { QMutexLocker lock(&m_mutex); writeBefore = m_stats.totalWriteUs; }
                if (frame) encoder.pushVideo(*frame); else encoder.pushAudio(*packet, audioCatchup);
                if (frame) submittedBitrate = policy.target();
                if (frame) {
                    QMutexLocker lock(&m_mutex);
                    m_stats.lastVideoEncodeUs = std::max<qint64>(0, nowUs() - started - (m_stats.totalWriteUs - writeBefore));
                    m_stats.totalVideoEncodeUs += m_stats.lastVideoEncodeUs;
                }
                publishStats(encoder.stats());
            }
            bool abandoned;
            { QMutexLocker lock(&m_mutex); abandoned = m_aborted; }
            if (!abandoned && !settings.live) {
                // 6. stop关闭入口之后排空AAC尾部/NVENC包，再写索引和容器尾；保留实际时间长度。
                encoder.finish(); publishStats(encoder.stats());
                check(av_write_trailer(mux.format), "write MP4 trailer");
                avio_flush(mux.io); check(mux.io->error, "flush MP4");
                if (!file.flush()) throw std::runtime_error(file.errorString().toStdString());
                file.close();
                // 7. 单次本地改名拒绝覆盖；包括初始化后别人创建了同名文件的情况。
                QMutexLocker lock(&m_mutex);
                if (!m_aborted && (m_stats.videoFrames || m_stats.audioSamples)) {
                    // QTemporaryFile的close保留内部句柄；用它自己的rename原子改名并拒绝覆盖。
                    if (!file.rename(destination.absoluteFilePath()))
                        throw std::runtime_error((QStringLiteral("录制文件改名失败：") + file.errorString()).toStdString());
                    file.setAutoRemove(false); savedPath = destination.absoluteFilePath();
                }
            }
        }
    } catch (const winrt::hresult_error &failure) { error = QString::fromStdWString(failure.message().c_str()); }
      catch (const std::exception &failure) { error = QString::fromUtf8(failure.what()); }
    // 8. 局部RAII先释放编码/显存/封装/临时文件，再发结果与QThread::finished；此后才能begin。
    { QMutexLocker lock(&m_mutex); m_accepting = false; m_video.clear(); m_audio.clear(); }
    if (!error.isEmpty() && !(m_settings.live && m_cancelIo.load())) emit failed(tr("媒体输出失败：%1").arg(error));
    else emit completed(savedPath, stats());
}
}
