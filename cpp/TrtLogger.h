#pragma once
#include <NvInfer.h>
#include <cstdio>

// TensorRT 日志接收器；寿命必须覆盖 Builder、Runtime 和 ExecutionContext。
class TrtLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override { // 只输出警告/错误；回调绝不抛异常。
        if (severity <= Severity::kWARNING) std::fprintf(stderr, "[TensorRT] %s\n", message);
    }
};
