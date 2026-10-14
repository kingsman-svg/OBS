#pragma once
#include "VideoCapture.h"
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVector>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace csn {
// 检测坐标已经还原到原视频像素；五点顺序由 SCRFD 模型定义。
struct FaceDetection {
    QRectF box;                          // 原图人脸框，裁剪到视频边界。
    std::array<QPointF, 5> landmarks{};   // 双眼、鼻尖和两个嘴角。
    float confidence = 0;                // 模型置信度，不作为身份识别结果。
};
struct BeautySettings {
    bool enabled = false;                // 本轮启用本地美颜，默认保留原画面。
    float smoothing = 0.5f;              // 磨皮强度0～1，运行中可调整。
    float brightness = 0.15f;            // 提亮强度0～1，局部柔和提升而非全图曝光。
};
struct FaceFrame {
    VideoFrame video;                    // 同帧原图或独立美颜纹理，保留原采集序号/QPC。
    QVector<FaceDetection> faces;         // NMS 后最多 16 张人脸，供 Shader 叠加。
    float gpuMs = 0;                     // CUDA 预处理 + enqueue，排除纹理复制和输出读回。
    qint64 processingMs = 0;             // 工作线程处理耗时，含互操作、读回和 CPU 后处理。
    quint64 replacedFrames = 0;          // 单帧输入邮箱被较新帧替换的次数。
    quint64 registrations = 0;           // 本轮 CUDA 纹理注册次数，尺寸/设备变化才增加。
    bool drawFaces = true;               // 是否绘制诊断框点，独立于检测和美颜开关。
    bool beautified = false;             // 本帧确实应用效果；无脸/零强度直接交付原纹理。
    qint64 beautyMs = 0;                 // D3D11美颜提交及完成等待耗时，不冒充纯GPU时间。
};
using FaceTensors = std::map<std::string, std::vector<float>>; // 按固定引擎输出名保存原始张量。
QSize scrfdResize(const QSize &source); // 640×640 等比缩放，右/下补边；至少保留一个像素。
QVector<FaceDetection> decodeScrfd(const FaceTensors &tensors, const QSize &source); // 检查形状、解码、NMS、逆缩放。
}
