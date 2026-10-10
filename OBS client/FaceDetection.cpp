#include "FaceDetection.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace csn {
QSize scrfdResize(const QSize &source)
{
    if (source.isEmpty() || source.width() > 16384 || source.height() > 16384)
        throw std::runtime_error("Invalid video dimensions");
    const double scale = std::min(640.0 / source.width(), 640.0 / source.height());
    return {std::max(1, int(source.width() * scale)), std::max(1, int(source.height() * scale))};
}

QVector<FaceDetection> decodeScrfd(const FaceTensors &tensors, const QSize &source)
{
    // 1. 先检查九个输出的长度与有限值，拒绝其它网络或错误的 I/O 契约。
    if (tensors.size() != 9) throw std::runtime_error("SCRFD requires nine output tensors");
    const auto resized = scrfdResize(source);
    QVector<FaceDetection> candidates;
    for (int stride : {8, 16, 32}) {
        const auto suffix = "_" + std::to_string(stride);
        const auto &scores = tensors.at("score" + suffix);
        const auto &boxes = tensors.at("bbox" + suffix);
        const auto &points = tensors.at("kps" + suffix);
        const int width = 640 / stride;
        const size_t count = size_t(width) * width * 2;
        if (scores.size() != count || boxes.size() != count * 4 || points.size() != count * 10)
            throw std::runtime_error("Unexpected SCRFD output shape");
        for (const auto *tensor : {&scores, &boxes, &points})
            for (float value : *tensor) if (!std::isfinite(value)) throw std::runtime_error("Non-finite SCRFD output");
        // 2. 每格两个 anchor；距离和五点偏移都乘以当前 stride。
        for (size_t i = 0; i < count; ++i) {
            if (scores[i] < 0.5f) continue;
            const double x = double((i / 2) % width) * stride, y = double((i / 2) / width) * stride;
            const double left = x - double(boxes[4 * i]) * stride, top = y - double(boxes[4 * i + 1]) * stride;
            const double right = x + double(boxes[4 * i + 2]) * stride, bottom = y + double(boxes[4 * i + 3]) * stride;
            if (right <= left || bottom <= top) continue;
            FaceDetection face;
            face.box = QRectF(QPointF(left, top), QPointF(right, bottom));
            face.confidence = scores[i];
            for (int j = 0; j < 5; ++j)
                face.landmarks[j] = {x + double(points[10 * i + 2 * j]) * stride, y + double(points[10 * i + 2 * j + 1]) * stride};
            candidates.append(face);
        }
    }
    // 3. 按置信度降序做 IoU NMS，限制候选数量，避免噪声输入拖慢 GUI 交付。
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.confidence > b.confidence; });
    if (candidates.size() > 5000) candidates.resize(5000);
    QVector<FaceDetection> kept, result;
    const QRectF bounds(0, 0, source.width(), source.height());
    const double scaleX = double(resized.width()) / source.width(), scaleY = double(resized.height()) / source.height();
    for (const auto &candidate : candidates) {
        bool suppressed = false;
        for (const auto &previous : kept) {
            const auto intersection = candidate.box.intersected(previous.box);
            const double overlap = intersection.width() * intersection.height();
            const double total = candidate.box.width() * candidate.box.height() + previous.box.width() * previous.box.height() - overlap;
            if (total > 0 && overlap / total > 0.45) { suppressed = true; break; }
        }
        if (suppressed) continue;
        kept.append(candidate);
        // 4. 按实际取整后的两轴尺寸逆缩放；补边检测和越界框不进入叠加结果。
        auto face = candidate;
        face.box = QRectF(face.box.x() / scaleX, face.box.y() / scaleY, face.box.width() / scaleX, face.box.height() / scaleY).intersected(bounds);
        if (face.box.isEmpty()) continue;
        for (auto &point : face.landmarks) point = {std::clamp(point.x() / scaleX, 0.0, double(source.width())), std::clamp(point.y() / scaleY, 0.0, double(source.height()))};
        result.append(face);
        if (result.size() == 16) break;
    }
    return result;
}
}
