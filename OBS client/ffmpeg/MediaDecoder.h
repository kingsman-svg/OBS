#pragma once
#include "VideoCapture.h"
#include "WasapiPlayback.h"
#include <QString>
#include <functional>
#include <memory>

namespace csn {
enum class VideoDecodeMode { Automatic, Software, RequireHardware }; // 默认优先硬件；强制模式用于真实GPU验收。
enum class VideoDecodePath { SoftwareUpload, D3D11Gpu, D3D11Readback }; // 按实际输出帧记录，回读路径不能冒充全GPU。
struct PlaybackVideo {
    VideoFrame source;             // BGRA GPU纹理及序号；文件PTS单独保存，不冒充采集QPC。
    qint64 ptsUs = 0;              // 相对于媒体共同原点的微秒PTS。
    qint64 durationUs = 0;         // 实际帧时长/帧率推算值，EOF保留最后一帧的持续时间。
    VideoDecodePath decodePath = VideoDecodePath::SoftwareUpload; // 当前帧实际解码/转换路径。
    QString decoderNote;          // 软件回退或硬件回读原因；不包含媒体地址/凭据。
};
struct MediaInfo {
    qint64 durationUs = 0;         // 容器/流给出的时长；未知时为0。
    qint64 audioStartSamples = 0;  // 音轨相对共同原点的起点，1/48000秒，前面保留静音。
    qint64 audioEndSamples = -1;   // 音轨实际结束位置，未知时-1；视频更长时可提前结束声音。
    int width = 0, height = 0;     // 首期视频解码尺寸，运行中按帧检查变化。
    bool video = false;           // 找到非封面视频流。
    bool audio = false;           // 找到音频流，可独立于视频播放。
    bool live = false;            // 直播无固定尾部；短探测/读取超时，不使用文件音轨结束裁剪。
};
// 普通类，无线程；MediaPlayer::run独占解封装、硬件/软件解码、GPU转换及重采样资源。
class MediaDecoder final {
public:
    using VideoSink = std::function<void(const PlaybackVideo &)>; // 在播放线程调用，只向有界队列交付。
    using AudioSink = std::function<void(const PlaybackAudio &)>; // 同线程交付48kHz立体声PCM。
    MediaDecoder();                              // 构造不打开文件或设备。
    ~MediaDecoder();                             // RAII释放格式/解码/SWR/SWS/GPU资源。
    MediaInfo open(const QString &source, std::function<bool()> cancelled, VideoSink video, AudioSink audio,
                   bool live = false, VideoDecodeMode mode = VideoDecodeMode::Automatic,
                   bool lowLatency = true); // 直播短探测，false保留旧预算用于对照；点播不使用。
    bool readNext();                             // 读取一个包并send/receive；false表示正常EOF。
    void finish();                               // EOF时排空两个解码器，再排空SWR尾部，幂等。
private:
    struct Resources;                            // FFmpeg状态及转换方法隐藏在cpp。
    std::unique_ptr<Resources> m_resources;       // 一轮播放独占，重启重新创建实例。
};
}
Q_DECLARE_METATYPE(csn::MediaInfo)
