#include "GpuFaceDetector.h"
#include "FacePreprocess.h"
#include <NvInfer.h>
#include <cuda_d3d11_interop.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <winrt/base.h>
#include <QElapsedTimer>
#include <QFile>
#include <cstdio>
#include <stdexcept>

namespace csn {
namespace {
void checkCuda(cudaError_t code)
{
    if (code != cudaSuccess) throw std::runtime_error(cudaGetErrorString(code));
}
size_t tensorCount(const nvinfer1::Dims &shape)
{
    size_t count = 1;
    if (shape.nbDims <= 0) throw std::runtime_error("Invalid engine shape");
    for (int i = 0; i < shape.nbDims; ++i) {
        if (shape.d[i] <= 0 || quint64(shape.d[i]) > 4000000ULL / count)
            throw std::runtime_error("Dynamic or oversized engine tensor");
        count *= size_t(shape.d[i]);
    }
    return count;
}
// TensorRT 会登记进程内 logger；让它跨越设备切换和每次 worker 重启的资源生命周期。
class RuntimeLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char *message) noexcept override // 不访问 GUI 对象。
    {
        if (severity <= Severity::kWARNING) std::fprintf(stderr, "TensorRT: %s\n", message);
    }
};
RuntimeLogger &runtimeLogger() { static RuntimeLogger logger; return logger; }
}

// 私有资源的整个生命周期只发生在检测工作线程；logger 保持进程级寿命。
struct GpuFaceDetector::Resources final {
    Microsoft::WRL::ComPtr<ID3D11Device> device;          // 输入所属设备，严禁跨适配器直接注册。
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; // 共享立即上下文，只用于 CopyResource/Flush。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;      // 固定尺寸的互操作中间纹理，采集/渲染不访问它。
    cudaGraphicsResource_t registered = nullptr;        // 与中间纹理一一对应的 CUDA 注册。
    cudaTextureObject_t sampler = 0;                    // 当前映射数组的点采样对象，同步完成才销毁。
    bool mapped = false;                                // 失败清理需要知道是否仍由 CUDA 占有纹理。
    QSize size;                                         // 中间纹理当前大小，变化时重新注册。
    int cudaDevice = -1;                                // 来自 D3D11 adapter 的 CUDA ordinal。
    cudaStream_t stream = nullptr;                      // 预处理、推理和 D2H 使用同一非阻塞 stream。
    std::array<cudaEvent_t, 4> events{};                 // 两对 event 分别测预处理和 enqueue。
    std::unique_ptr<nvinfer1::IRuntime> runtime;         // 晚于 engine/context 释放，logger 此时仍存活。
    std::unique_ptr<nvinfer1::ICudaEngine> engine;       // 本轮加载的固定形状 SCRFD 引擎。
    std::unique_ptr<nvinfer1::IExecutionContext> infer;   // 单工作线程串行执行，不跨线程使用。
    std::map<std::string, void *> buffers;              // 复用各命名张量的 device 地址。
    FaceTensors outputs;                                // CPU 后处理读取模型输出，不读取视频像素。
    float *input = nullptr;                             // 固定 Float32 RGB NCHW 输入显存。
    quint64 registrations = 0;                          // 本资源注册次数，供调试验证复用。

    ~Resources() // 任何部分初始化失败也走同一条幂等清理路径；不调用 cudaDeviceReset。
    {
        if (cudaDevice >= 0) cudaSetDevice(cudaDevice);
        if (stream) cudaStreamSynchronize(stream);
        if (sampler) cudaDestroyTextureObject(sampler);
        if (mapped) { cudaGraphicsUnmapResources(1, &registered, stream); cudaStreamSynchronize(stream); }
        if (registered) cudaGraphicsUnregisterResource(registered);
        infer.reset();
        for (auto &entry : buffers) if (entry.second) cudaFree(entry.second);
        for (auto event : events) if (event) cudaEventDestroy(event);
        if (stream) cudaStreamDestroy(stream);
        engine.reset(); runtime.reset();
    }
    void initialize(ID3D11Device *sourceDevice, const QByteArray &plan) // 沿源设备选 CUDA，再反序列化。
    {
        // 1. D3D11 与 CUDA 必须落在同一 NVIDIA adapter；不支持的设备明确失败并回退原预览。
        device = sourceDevice;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        winrt::check_hresult(device.As(&dxgi));
        winrt::check_hresult(dxgi->GetAdapter(&adapter));
        checkCuda(cudaD3D11GetDevice(&cudaDevice, adapter.Get()));
        checkCuda(cudaSetDevice(cudaDevice));
        device->GetImmediateContext(&context);
        Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
        winrt::check_hresult(context.As(&protection)); protection->SetMultithreadProtected(TRUE);
        checkCuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        for (auto &event : events) checkCuda(cudaEventCreate(&event));
        // 2. OBS 只加载已有 engine；不链接 ONNX parser、OpenCV 或 ONNX Runtime。
        runtime.reset(nvinfer1::createInferRuntime(runtimeLogger()));
        if (!runtime) throw std::runtime_error("Cannot create TensorRT runtime");
        engine.reset(runtime->deserializeCudaEngine(plan.constData(), size_t(plan.size())));
        if (!engine) throw std::runtime_error("Cannot load engine; check TensorRT version and GPU compatibility");
        infer.reset(engine->createExecutionContext());
        if (!infer || engine->getNbIOTensors() != 10) throw std::runtime_error("Unexpected SCRFD engine I/O");
        // 3. 校验输入形状、输出名称/长度，复用十个显存缓冲和一个执行上下文。
        for (int i = 0; i < engine->getNbIOTensors(); ++i) {
            const std::string name = engine->getIOTensorName(i);
            if (engine->getTensorDataType(name.c_str()) != nvinfer1::DataType::kFLOAT ||
                engine->getTensorLocation(name.c_str()) != nvinfer1::TensorLocation::kDEVICE)
                throw std::runtime_error("Engine requires Float32 device I/O");
            const auto shape = engine->getTensorShape(name.c_str());
            const size_t count = tensorCount(shape);
            if (engine->getTensorIOMode(name.c_str()) == nvinfer1::TensorIOMode::kINPUT) {
                if (input || shape.nbDims != 4 || shape.d[0] != 1 || shape.d[1] != 3 || shape.d[2] != 640 || shape.d[3] != 640)
                    throw std::runtime_error("Input must be [1,3,640,640]");
            } else outputs[name].resize(count);
            checkCuda(cudaMalloc(&buffers[name], count * sizeof(float)));
            if (!infer->setTensorAddress(name.c_str(), buffers[name])) throw std::runtime_error("Cannot bind engine tensor");
            if (engine->getTensorIOMode(name.c_str()) == nvinfer1::TensorIOMode::kINPUT) input = static_cast<float *>(buffers[name]);
        }
        if (!input) throw std::runtime_error("Missing engine input");
        decodeScrfd(outputs, QSize(640, 640)); // 零输出用于启动时验证九个头的形状；不产生人脸。
    }
    void ensureTexture(const QSize &newSize) // 首帧/尺寸变化才创建和注册，不能逐帧注册采集快照。
    {
        if (texture && size == newSize) return;
        // 1. 上一帧已同步；先撤销旧注册，再释放其 D3D11 资源。
        if (registered) { checkCuda(cudaGraphicsUnregisterResource(registered)); registered = nullptr; }
        texture.Reset(); size = {};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(newSize.width()); desc.Height = UINT(newSize.height());
        desc.MipLevels = 1; desc.ArraySize = 1; desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // 2. 只在本 worker 使用中间纹理；D3D copy 和 CUDA map/unmap 严格轮流占有。
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, &texture));
        checkCuda(cudaGraphicsD3D11RegisterResource(&registered, texture.Get(), cudaGraphicsRegisterFlagsNone));
        checkCuda(cudaGraphicsResourceSetMapFlags(registered, cudaGraphicsMapFlagsReadOnly));
        size = newSize; ++registrations;
    }
    FaceFrame process(const VideoFrame &frame) // 拷贝快照 → map/预处理/unmap → enqueue → 解码。
    {
        QElapsedTimer timer; timer.start();
        D3D11_TEXTURE2D_DESC desc{}; frame.texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
            throw std::runtime_error("Detection requires single-layer BGRA8 texture");
        const QSize source(int(desc.Width), int(desc.Height));
        const auto resized = scrfdResize(source);
        ensureTexture(source);
        // 1. GPU → GPU 拷贝；map 将等待此前 D3D 命令完成，源快照仍可独立供预览使用。
        context->CopyResource(texture.Get(), frame.texture.Get()); context->Flush();
        checkCuda(cudaGraphicsMapResources(1, &registered, stream)); mapped = true;
        cudaArray_t array = nullptr;
        checkCuda(cudaGraphicsSubResourceGetMappedArray(&array, registered, 0, 0));
        cudaResourceDesc resource{}; resource.resType = cudaResourceTypeArray; resource.res.array.array = array;
        cudaTextureDesc sampling{}; sampling.addressMode[0] = sampling.addressMode[1] = cudaAddressModeClamp;
        sampling.filterMode = cudaFilterModePoint; sampling.readMode = cudaReadModeElementType;
        checkCuda(cudaCreateTextureObject(&sampler, &resource, &sampling, nullptr));
        // 2. CUDA 直接生成 RGB/NCHW；补边和归一化与独立优化工程一致。
        checkCuda(cudaEventRecord(events[0], stream));
        checkCuda(launchFacePreprocess(sampler, source.width(), source.height(), resized.width(), resized.height(), input, stream));
        checkCuda(cudaEventRecord(events[1], stream));
        // 映射数组和纹理采样对象只在 map 期间有效；先等 kernel 完成再撤销对象。
        checkCuda(cudaEventSynchronize(events[1]));
        checkCuda(cudaDestroyTextureObject(sampler)); sampler = 0;
        checkCuda(cudaGraphicsUnmapResources(1, &registered, stream)); mapped = false;
        // 3. 在同一 stream 执行，只有模型输出张量读回 CPU；图像不离开 GPU。
        checkCuda(cudaEventRecord(events[2], stream));
        if (!infer->enqueueV3(stream)) throw std::runtime_error("TensorRT enqueue failed");
        checkCuda(cudaEventRecord(events[3], stream));
        for (auto &entry : outputs)
            checkCuda(cudaMemcpyAsync(entry.second.data(), buffers.at(entry.first), entry.second.size() * sizeof(float), cudaMemcpyDeviceToHost, stream));
        checkCuda(cudaStreamSynchronize(stream));
        // 4. NMS 和逆缩放完成后打包同一帧；两对 GPU event 排除 map/unmap 和读回。
        FaceFrame result; result.video = frame; result.faces = decodeScrfd(outputs, source);
        float preprocessing = 0, inference = 0;
        checkCuda(cudaEventElapsedTime(&preprocessing, events[0], events[1]));
        checkCuda(cudaEventElapsedTime(&inference, events[2], events[3]));
        result.gpuMs = preprocessing + inference; result.processingMs = timer.elapsed();
        result.registrations = registrations;
        return result;
    }
};

GpuFaceDetector::GpuFaceDetector(QObject *parent) : QThread(parent) {}
GpuFaceDetector::~GpuFaceDetector() { stop(); wait(); }

bool GpuFaceDetector::begin(const QString &enginePath)
{
    // 1. 上一轮 finished 由控制器处理后再启动；此处不加载引擎、不等待 GPU。
    if (isRunning()) return false;
    QMutexLocker lock(&m_mutex);
    m_enginePath = enginePath; m_pending = {}; m_latest = {}; m_replaced = 0; m_accepting = true;
    start(); return true;
}
void GpuFaceDetector::submit(const VideoFrame &frame)
{
    QMutexLocker lock(&m_mutex);
    if (!m_accepting || !frame.texture) return;
    if (m_pending.texture) ++m_replaced;
    m_pending = frame; m_available.wakeOne();
}
FaceFrame GpuFaceDetector::latestResult() const { QMutexLocker lock(&m_mutex); return m_latest; }
void GpuFaceDetector::stop()
{
    // 2. 清空两个邮箱并唤醒；正在执行的一帧完成后也不能重新发布结果。
    QMutexLocker lock(&m_mutex);
    m_accepting = false; m_pending = {}; m_latest = {};
    requestInterruption(); m_available.wakeOne();
}
void GpuFaceDetector::run()
{
    QString error;
    try {
        // 1. 只读取本地指定引擎；大小有界，不在 GUI 线程触发磁盘/反序列化操作。
        QFile file(m_enginePath);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 512LL * 1024 * 1024)
            throw std::runtime_error("Cannot read engine; choose an existing local .engine file (up to 512 MiB)");
        const auto plan = file.readAll();
        if (plan.size() != file.size()) throw std::runtime_error("Engine read was incomplete");
        std::unique_ptr<Resources> gpu;
        quint64 previousRegistrations = 0;
        while (!isInterruptionRequested()) {
            VideoFrame frame;
            {
                // 2. 单帧邮箱，慢处理时丢旧待处理帧；停止无需等待下一帧到来。
                QMutexLocker lock(&m_mutex);
                while (m_accepting && !m_pending.texture) m_available.wait(&m_mutex);
                if (!m_accepting) break;
                frame = std::move(m_pending); m_pending = {};
            }
            Microsoft::WRL::ComPtr<ID3D11Device> device; frame.texture->GetDevice(&device);
            if (!gpu || gpu->device.Get() != device.Get()) {
                // 3. 换源设备时先在旧 CUDA device 上清理，再创建新的独占执行上下文。
                if (gpu) previousRegistrations += gpu->registrations;
                gpu.reset(); gpu = std::make_unique<Resources>();
                gpu->initialize(device.Get(), plan);
                if (!isInterruptionRequested()) emit opened();
            }
            auto result = gpu->process(frame);
            {
                // 4. 结果与原帧一起原子发布；stop 后结果被丢弃，避免退出/重启闪回旧帧。
                QMutexLocker lock(&m_mutex);
                if (!m_accepting) break;
                result.replacedFrames = m_replaced; result.registrations += previousRegistrations;
                m_latest = std::move(result);
            }
        }
        // 5. gpu 在本线程作用域中释放；finished 发出前已完成 CUDA 同步和资源注销。
    } catch (const winrt::hresult_error &e) {
        error = QString::fromStdWString(e.message().c_str());
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
    bool report = false;
    {
        QMutexLocker lock(&m_mutex);
        report = m_accepting && !error.isEmpty(); m_accepting = false; m_pending = {};
        if (!error.isEmpty()) m_latest = {};
    }
    if (report) emit failed(tr("人脸检测失败：%1；已恢复原画面。停止采集后可重试。").arg(error));
}
}
