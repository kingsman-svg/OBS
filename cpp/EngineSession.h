#pragma once
#include "TrtLogger.h"
#include <cuda_runtime_api.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

// 离线验证使用的最小 TensorRT 会话：只接受固定尺寸、单输入、Float32 I/O。
// OBS 的实时会话将使用 GPU 输入；这里的 CPU 拷贝仅用于校准与对照测试。
class EngineSession final {
public:
    EngineSession(const std::vector<char>& plan, TrtLogger& logger); // 反序列化并分配命名张量缓冲。
    ~EngineSession();                                             // 等待本会话任务后释放 CUDA 资源。
    EngineSession(const EngineSession&) = delete;                  // GPU 缓冲不共享所有权。
    EngineSession& operator=(const EngineSession&) = delete;       // 禁止隐式复制。
    const std::map<std::string, std::vector<float>>& run(const std::vector<float>& input); // H2D → 推理 → D2H。
    float benchmark(const std::vector<float>& input, int iterations = 30); // CUDA event 测 enqueue，毫秒/次。
    std::string inspect() const;                                  // 返回实际引擎层信息，核查 INT8/稀疏 tactic。
private:
    void release() noexcept;                                      // 支持构造中途失败后的幂等清理。
    std::unique_ptr<nvinfer1::IRuntime> m_runtime;                  // Runtime 必须晚于引擎销毁。
    std::unique_ptr<nvinfer1::ICudaEngine> m_engine;                // 反序列化后的不可变引擎。
    std::unique_ptr<nvinfer1::IExecutionContext> m_context;         // 本线程串行使用的执行上下文。
    cudaStream_t m_stream = nullptr;                               // 本会话独占 CUDA stream。
    std::string m_inputName;                                       // 唯一输入张量的名称。
    size_t m_inputCount = 0;                                       // 输入 Float32 元素数。
    std::map<std::string, void*> m_device;                          // 各命名张量的独占 device 内存。
    std::map<std::string, std::vector<float>> m_outputs;            // 本次推理输出；下次 run 会覆盖。
};
