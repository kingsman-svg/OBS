#include "EngineSession.h"
#include <cmath>
#include <stdexcept>

namespace {
void check(cudaError_t error) {
    if (error != cudaSuccess) throw std::runtime_error(cudaGetErrorString(error));
}
size_t elements(nvinfer1::Dims dims) {
    size_t count = 1;
    if (dims.nbDims < 0) throw std::runtime_error("Invalid tensor shape");
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] <= 0 || static_cast<uint64_t>(dims.d[i]) > 100000000ULL / count) throw std::runtime_error("Unsupported tensor shape");
        count *= static_cast<size_t>(dims.d[i]);
    }
    return count;
}
}

EngineSession::EngineSession(const std::vector<char>& plan, TrtLogger& logger) {
    try {
        // 1. 反序列化：模型必须来自本工具，版本与设备兼容性由 TensorRT 检查。
        m_runtime.reset(nvinfer1::createInferRuntime(logger));
        if (!m_runtime) throw std::runtime_error("Cannot create TensorRT runtime");
        m_engine.reset(m_runtime->deserializeCudaEngine(plan.data(), plan.size()));
        if (!m_engine) throw std::runtime_error("Cannot deserialize engine");
        m_context.reset(m_engine->createExecutionContext());
        if (!m_context) throw std::runtime_error("Cannot create execution context");
        check(cudaStreamCreate(&m_stream));
        // 2. 按名称分配所有 I/O；拒绝动态形状、非 Float32 和多输入，避免静默误读。
        for (int i = 0; i < m_engine->getNbIOTensors(); ++i) {
            const std::string name = m_engine->getIOTensorName(i);
            if (m_engine->getTensorDataType(name.c_str()) != nvinfer1::DataType::kFLOAT)
                throw std::runtime_error("Only Float32 I/O is supported");
            const size_t count = elements(m_engine->getTensorShape(name.c_str()));
            auto& address = m_device[name];
            check(cudaMalloc(&address, count * sizeof(float)));
            if (!m_context->setTensorAddress(name.c_str(), address)) throw std::runtime_error("Cannot bind tensor");
            if (m_engine->getTensorIOMode(name.c_str()) == nvinfer1::TensorIOMode::kINPUT) {
                if (!m_inputName.empty()) throw std::runtime_error("Only single input is supported");
                m_inputName = name;
                m_inputCount = count;
            } else m_outputs[name].resize(count);
        }
        if (m_inputName.empty() || m_outputs.empty()) throw std::runtime_error("Missing engine I/O");
    } catch (...) { release(); throw; }
}

EngineSession::~EngineSession() { release(); }

void EngineSession::release() noexcept {
    // 3. 先同步再释放；构造失败时 nullptr 和空容器也可以走同一条清理路径。
    if (m_stream) cudaStreamSynchronize(m_stream);
    m_context.reset();
    for (auto& entry : m_device) if (entry.second) cudaFree(entry.second);
    m_device.clear();
    if (m_stream) cudaStreamDestroy(m_stream);
    m_stream = nullptr;
    m_engine.reset();
    m_runtime.reset();
}

const std::map<std::string, std::vector<float>>& EngineSession::run(const std::vector<float>& input) {
    if (input.size() != m_inputCount) throw std::runtime_error("Wrong input element count");
    // 1. CPU 校准输入 → device 输入；一条 stream 保证拷贝、推理、读回的顺序。
    check(cudaMemcpyAsync(m_device.at(m_inputName), input.data(), input.size() * sizeof(float), cudaMemcpyHostToDevice, m_stream));
    // 2. 执行内部已融合的计算图；enqueue 成功只表示提交成功。
    if (!m_context->enqueueV3(m_stream)) throw std::runtime_error("TensorRT enqueue failed");
    // 3. 输出读回并同步，随后才允许校准统计或 CPU 后处理访问。
    for (auto& entry : m_outputs)
        check(cudaMemcpyAsync(entry.second.data(), m_device.at(entry.first), entry.second.size() * sizeof(float), cudaMemcpyDeviceToHost, m_stream));
    check(cudaStreamSynchronize(m_stream));
    for (const auto& entry : m_outputs)
        for (float value : entry.second) if (!std::isfinite(value)) throw std::runtime_error("Non-finite output: " + entry.first);
    return m_outputs;
}

float EngineSession::benchmark(const std::vector<float>& input, int iterations) {
    run(input);
    if (iterations <= 0) throw std::runtime_error("Invalid benchmark count");
    // 1. 预热；不把 CPU 图片预处理和 H2D/D2H 计入 GPU 推理时间。
    for (int i = 0; i < 5; ++i) if (!m_context->enqueueV3(m_stream)) throw std::runtime_error("Warmup failed");
    cudaEvent_t begin = nullptr, end = nullptr;
    try {
        check(cudaEventCreate(&begin));
        check(cudaEventCreate(&end));
        // 2. 使用同一 stream 上的 event 包围多次执行，等待 end 完成。
        check(cudaEventRecord(begin, m_stream));
        for (int i = 0; i < iterations; ++i)
            if (!m_context->enqueueV3(m_stream)) throw std::runtime_error("Benchmark enqueue failed");
        check(cudaEventRecord(end, m_stream));
        check(cudaEventSynchronize(end));
        float elapsed = 0;
        check(cudaEventElapsedTime(&elapsed, begin, end));
        cudaEventDestroy(begin); cudaEventDestroy(end);
        return elapsed / iterations;
    } catch (...) {
        if (begin) cudaEventDestroy(begin);
        if (end) cudaEventDestroy(end);
        throw;
    }
}

std::string EngineSession::inspect() const {
    std::unique_ptr<nvinfer1::IEngineInspector> inspector(m_engine->createEngineInspector());
    if (!inspector) throw std::runtime_error("Cannot create engine inspector");
    const auto* information = inspector->getEngineInformation(nvinfer1::LayerInformationFormat::kJSON);
    if (!information) throw std::runtime_error("Cannot inspect engine");
    return information;
}
