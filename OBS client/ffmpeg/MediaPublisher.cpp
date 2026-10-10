#include "MediaPublisher.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTemporaryFile>
#include <winrt/base.h>
#include <optional>
#include <stdexcept>

namespace csn {
namespace {
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
        avformat_free_context(format);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
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
    m_settings = settings; m_video.clear(); m_audio.clear(); m_stats = {};
    m_accepting = true; m_finishing = false; m_aborted = false;
    start(); return true; // 2. QThread建立工作线程，随后在该线程调用run。
}
void MediaPublisher::pushVideo(const TimedVideoFrame &frame)
{
    QMutexLocker lock(&m_mutex);
    if (!m_accepting || !m_settings.video) return;
    if (m_video.size() == 16) { m_video.pop_front(); ++m_stats.droppedVideo; }
    m_video.push_back(frame); m_ready.wakeOne();
}
void MediaPublisher::pushAudio(const TimedAudioPacket &packet)
{
    QMutexLocker lock(&m_mutex);
    if (!m_accepting || !m_settings.audio) return;
    if (m_audio.size() == 50) { m_audio.pop_front(); ++m_stats.droppedAudio; }
    m_audio.push_back(packet); m_ready.wakeOne();
}
void MediaPublisher::stop()
{
    QMutexLocker lock(&m_mutex);
    m_accepting = false; m_finishing = true; m_ready.wakeOne();
}
void MediaPublisher::abort()
{
    QMutexLocker lock(&m_mutex);
    m_accepting = false; m_aborted = true; m_video.clear(); m_audio.clear(); m_ready.wakeOne();
}
EncodingStats MediaPublisher::stats() const { QMutexLocker lock(&m_mutex); return m_stats; }
void MediaPublisher::publishStats(const EncodingStats &value)
{
    QMutexLocker lock(&m_mutex);
    const auto videoDrops = m_stats.droppedVideo, audioDrops = m_stats.droppedAudio;
    m_stats = value; m_stats.droppedVideo = videoDrops; m_stats.droppedAudio = audioDrops;
}

void MediaPublisher::run()
{
    QString savedPath, error;
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
            if (!m_video.empty() && m_video.front().source.video.texture)
                m_video.front().source.video.texture->GetDevice(&device);
        }
        bool proceed;
        { QMutexLocker lock(&m_mutex); proceed = !m_aborted && (!settings.video || device); }
        if (proceed) {
            // 4. 同目录临时文件先完成trailer再改名；已有目标绝不覆盖，失败不留下半成品MP4。
            const QFileInfo destination(settings.path);
            if (settings.path.isEmpty() || destination.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) != 0)
                throw std::invalid_argument("本步录制输出必须是本地.mp4路径");
            if (destination.exists()) throw std::runtime_error("录制目标已存在，请使用新的文件名");
            if (!QDir().mkpath(destination.absolutePath())) throw std::runtime_error("无法创建录制目录");
            QTemporaryFile file(QDir(destination.absolutePath()).filePath(QStringLiteral(".obs-record-XXXXXX.part")));
            if (!file.open()) throw std::runtime_error(file.errorString().toStdString());
            Muxer mux;
            check(avformat_alloc_output_context2(&mux.format, nullptr, "mp4", nullptr), "MP4 context");
            auto *buffer = static_cast<uint8_t *>(av_malloc(65536));
            if (!buffer) throw std::bad_alloc();
            mux.io = avio_alloc_context(buffer, 65536, 1, &file, nullptr, writeFile, seekFile);
            if (!mux.io) { av_free(buffer); throw std::bad_alloc(); }
            mux.format->pb = mux.io; mux.format->flags |= AVFMT_FLAG_CUSTOM_IO;
            // 限制封装交织缓存；保留各路原始PTS和AAC编码延迟，由MP4编辑列表描述。
            mux.format->max_interleave_delta = 100000;
            AVStream *video = nullptr, *audio = nullptr;
            MediaEncoder encoder;
            encoder.open(settings, device.Get(), (mux.format->oformat->flags & AVFMT_GLOBALHEADER) != 0,
                [&](AVPacket *packet, const AVCodecContext *codec) {
                    auto *stream = codec->codec_type == AVMEDIA_TYPE_VIDEO ? video : audio;
                    if (!stream) throw std::logic_error("编码包缺少对应MP4流");
                    av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
                    packet->stream_index = stream->index;
                    check(av_interleaved_write_frame(mux.format, packet), "write MP4 packet");
                });
            auto addStream = [&](const AVCodecContext *codec) -> AVStream * {
                if (!codec) return nullptr;
                auto *stream = avformat_new_stream(mux.format, nullptr); if (!stream) throw std::bad_alloc();
                check(avcodec_parameters_from_context(stream->codecpar, codec), "MP4 stream parameters");
                stream->time_base = codec->time_base; return stream;
            };
            video = addStream(encoder.videoContext()); audio = addStream(encoder.audioContext());
            check(avformat_write_header(mux.format, nullptr), "write MP4 header");
            emit opened();
            // 5. 只取队列头的引用后解锁；编码/GPU等待/文件IO都不占用GUI入口的互斥锁。
            for (;;) {
                std::optional<TimedVideoFrame> frame;
                std::optional<TimedAudioPacket> packet;
                {
                    QMutexLocker lock(&m_mutex);
                    while (m_video.empty() && m_audio.empty() && !m_finishing && !m_aborted) m_ready.wait(&m_mutex);
                    if (m_aborted || (m_finishing && m_video.empty() && m_audio.empty())) break;
                    const bool takeVideo = !m_video.empty() && (m_audio.empty()
                        || av_compare_ts(m_video.front().pts, {1, 30}, m_audio.front().pts, {1, 48000}) <= 0);
                    if (takeVideo) { frame = std::move(m_video.front()); m_video.pop_front(); }
                    else { packet = std::move(m_audio.front()); m_audio.pop_front(); }
                }
                if (frame) encoder.pushVideo(*frame); else encoder.pushAudio(*packet);
                publishStats(encoder.stats());
            }
            bool abandoned;
            { QMutexLocker lock(&m_mutex); abandoned = m_aborted; }
            if (!abandoned) {
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
    if (!error.isEmpty()) emit failed(tr("录制失败：%1").arg(error));
    else emit completed(savedPath, stats());
}
}
