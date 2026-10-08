#pragma once
#include <QByteArray>
#include <QMetaType>

namespace csn {
// 采集与音频处理共用的包；拥有 PCM 数据，不引用系统端点缓冲。
struct AudioPacket {
    QByteArray pcm;                    // 交错 Float32、小端；每帧包含 channels 个采样值。
    int sampleRate = 0;                // 每秒采样帧数；采集保留设备值，混音输出固定 48000。
    int channels = 0;                  // 每帧声道数；混音输出固定 2。
    quint64 channelMask = 0;           // Windows/FFmpeg 扬声器位掩码，0 表示未知；多声道需明确布局。
    qint64 timestamp100ns = 0;          // 首个采样帧的 QPC 时间，单位 100ns，与视频同域。
    bool discontinuity = false;        // 设备/队列丢包、时间跳变或混音缺包，不能假定连续。
    bool timestampEstimated = false;   // 时间戳包含设备估计值，不能当作精准同步依据。
};
} // namespace csn
Q_DECLARE_METATYPE(csn::AudioPacket)
