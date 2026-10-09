#pragma once
#include "TrtLogger.h"
#include <filesystem>

// 离线工具入口：ONNX → 基线 → 校准 → 可选 2:4 剪枝 → INT8 Q/DQ → 对照 → engine。
// 不依赖 Qt，不在 OBS 中构建模型；剪枝候选验证失败时不发布输出引擎。
class ModelOptimizer final {
public:
    void run(const std::filesystem::path& onnx, const std::filesystem::path& calibration,
             const std::filesystem::path& validation, const std::filesystem::path& output,
             bool sparse, bool smoke); // 正式模式要求独立图片集；smoke 仅验证技术流程。
    static void selfTest();           // 检查 2:4 布局、按通道量化尺度及检测结果验收逻辑。
private:
    TrtLogger m_logger;               // 日志接收器的寿命覆盖整个优化和验证流程。
};
