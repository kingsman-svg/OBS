#include "ModelOptimizer.h"
#include "EngineSession.h"
#include <NvOnnxParser.h>
#include <onnxruntime_cxx_api.h>
#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <list>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace nvinfer1;
namespace fs = std::filesystem;
using Outputs = std::map<std::string, std::vector<float>>;
using Scales = std::map<std::string, float>;
constexpr int Side = 640; // 固定 batch=1、640×640；本步不引入动态 shape 和多 stream。
constexpr float Score = 0.5f;
constexpr float Nms = 0.4f;
constexpr float MinIou = 0.90f;
constexpr float MaxPointError = 4.0f; // 输入缩放图上的像素，不是原图像素。
constexpr float MaxScoreError = 0.10f;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T* checked(T* value, const char* message) {
    require(value != nullptr, message);
    return value;
}
std::vector<char> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "Cannot read: " + path.u8string());
    const auto size = fs::file_size(path);
    require(size > 0 && size <= 256 * 1024 * 1024, "Empty or oversized file");
    std::vector<char> bytes(static_cast<size_t>(size));
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<size_t>(file.gcount()) == bytes.size(), "Incomplete read");
    return bytes;
}
void writeFile(const fs::path& path, const char* data, size_t count) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(file.good(), "Cannot write: " + path.u8string());
    file.write(data, static_cast<std::streamsize>(count));
    file.close();
    require(file.good(), "Incomplete write");
}
void writeText(const fs::path& path, const std::string& text) { writeFile(path, text.data(), text.size()); }
std::vector<fs::path> images(const fs::path& directory) {
    require(fs::is_directory(directory), "Image directory does not exist: " + directory.u8string());
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        auto ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (entry.is_regular_file() && (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp")) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    require(!files.empty() && files.size() <= 1000, "Require 1..1000 images in each directory");
    return files;
}
cv::Mat loadImage(const fs::path& path) {
    const auto bytes = readFile(path); // Windows 中文路径交给 filesystem，避免 imread 的窄字符路径限制。
    cv::Mat image = cv::imdecode(std::vector<unsigned char>(bytes.begin(), bytes.end()), cv::IMREAD_COLOR);
    require(!image.empty(), "Cannot decode image: " + path.u8string());
    // 保持比例缩放，右侧/下侧补黑，与官方 SCRFD 预处理一致。
    const double scale = static_cast<double>(Side) / std::max(image.cols, image.rows);
    cv::Mat resized, padded = cv::Mat::zeros(Side, Side, CV_8UC3);
    cv::resize(image, resized, cv::Size(std::max(1, static_cast<int>(image.cols * scale)), std::max(1, static_cast<int>(image.rows * scale))));
    resized.copyTo(padded(cv::Rect(0, 0, resized.cols, resized.rows)));
    return padded;
}
std::vector<float> inputBlob(const cv::Mat& image) {
    cv::Mat blob = cv::dnn::blobFromImage(image, 1.0 / 128.0, cv::Size(Side, Side), cv::Scalar(127.5, 127.5, 127.5), true, false, CV_32F);
    // SCRFD-10G-KPS：RGB、NCHW，(pixel - 127.5) / 128。
    return {blob.ptr<float>(), blob.ptr<float>() + blob.total()};
}

// 每个输出通道单独计算对称 INT8 尺度；全零通道也保留非零尺度。
std::vector<float> channelScales(const std::vector<float>& weights, int channels) {
    require(channels > 0 && !weights.empty() && weights.size() % channels == 0, "Invalid weight channels");
    const size_t stride = weights.size() / channels;
    std::vector<float> scales(channels);
    for (int k = 0; k < channels; ++k) {
        float peak = 0;
        for (size_t j = 0; j < stride; ++j) {
            const float value = weights[static_cast<size_t>(k) * stride + j];
            require(std::isfinite(value), "Non-finite model weight");
            peak = std::max(peak, std::abs(value));
        }
        scales[k] = std::max(peak / 127.0f, 1e-8f);
    }
    return scales;
}

// K×C×H×W 存储：对每个输出通道、每个空间位置，沿 C 连续四项保留绝对值最大的两项。
// 这是 2:4 半结构化幅值剪枝；没有删除通道、没有训练或蒸馏。
bool prune24(std::vector<float>& weights, int outputs, int inputs, int spatial) {
    require(outputs > 0 && inputs > 0 && spatial > 0 && weights.size() == static_cast<size_t>(outputs) * inputs * spatial,
            "Invalid KCHW weight shape");
    if (inputs % 4 != 0) return false; // 首层 C=3 和 depthwise C/group=1 等不满足要求。
    for (int k = 0; k < outputs; ++k) for (int c = 0; c < inputs; c += 4) for (int s = 0; s < spatial; ++s) {
        std::array<size_t, 4> indices{};
        for (int i = 0; i < 4; ++i) indices[i] = (static_cast<size_t>(k) * inputs + c + i) * spatial + s;
        std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return std::abs(weights[a]) > std::abs(weights[b]); });
        weights[indices[2]] = 0;
        weights[indices[3]] = 0;
    }
    return true;
}

Weights kernelOf(INetworkDefinition& network, IConvolutionLayer& conv) {
    Weights weights = conv.getKernelWeights();
    if (weights.count) return weights;
    // ONNX parser 也可能把卷积权重表示为 Constant 输入；只接受静态常量。
    if (conv.getNbInputs() > 1 && conv.getInput(1)) {
        for (int i = 0; i < network.getNbLayers(); ++i) {
            auto* layer = network.getLayer(i);
            if (layer->getType() == LayerType::kCONSTANT && layer->getOutput(0) == conv.getInput(1))
                return static_cast<IConstantLayer*>(layer)->getWeights();
        }
    }
    throw std::runtime_error("Convolution kernel is not a static constant");
}
void replaceConvolution(INetworkDefinition& network, IConvolutionLayer& original, ITensor& activation, ITensor& kernel) {
    // TensorRT 11.2 不能把已创建卷积的静态 kernel 通过 setKernelWeights 清空。
    // 因此从空 kernel 创建等价卷积、接入权重张量，再重接消费者；原层无消费者后由 Builder 删除。
    auto* replacement = checked(network.addConvolutionNd(activation, original.getNbOutputMaps(), original.getKernelSizeNd(),
                                   Weights{DataType::kFLOAT, nullptr, 0}, original.getBiasWeights()), "Cannot recreate convolution");
    replacement->setInput(1, kernel);
    if (original.getNbInputs() > 2 && original.getInput(2)) replacement->setInput(2, *original.getInput(2));
    replacement->setStrideNd(original.getStrideNd());
    replacement->setPrePadding(original.getPrePadding());
    replacement->setPostPadding(original.getPostPadding());
    replacement->setPaddingMode(original.getPaddingMode());
    replacement->setDilationNd(original.getDilationNd());
    replacement->setNbGroups(original.getNbGroups());
    const std::string layerName = original.getName();
    original.setName((layerName + "_unused").c_str());
    replacement->setName(layerName.c_str());
    auto* before = original.getOutput(0);
    auto* after = replacement->getOutput(0);
    const std::string tensorName = before->getName();
    before->setName((tensorName + "_unused").c_str());
    after->setName(tensorName.c_str());
    for (int i = 0; i < network.getNbLayers(); ++i) {
        auto* layer = network.getLayer(i);
        for (int j = 0; j < layer->getNbInputs(); ++j)
            if (layer->getInput(j) == before) layer->setInput(j, *after);
    }
    if (before->isNetworkOutput()) {
        network.unmarkOutput(*before);
        network.markOutput(*after);
    }
}
std::vector<float> copyWeights(Weights weights) {
    require(weights.type == DataType::kFLOAT && weights.count > 0 && weights.values, "Require FP32 source weights");
    auto* start = static_cast<const float*>(weights.values);
    return {start, start + weights.count};
}

std::vector<char> build(INetworkDefinition& network, IBuilder& builder, bool sparse) {
    std::unique_ptr<IBuilderConfig> config(checked(builder.createBuilderConfig(), "Cannot create builder config"));
    config->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE, 256ULL * 1024 * 1024);
    config->setProfilingVerbosity(ProfilingVerbosity::kDETAILED);
    config->clearFlag(BuilderFlag::kTF32); // FP32 基线不偷偷使用 TF32；其它候选由图中明确类型决定。
    if (sparse) config->setFlag(BuilderFlag::kSPARSE_WEIGHTS); // 允许稀疏 tactic；不保证最终选用。
    std::unique_ptr<IHostMemory> plan(checked(builder.buildSerializedNetwork(network, *config), "Engine build failed"));
    const auto* bytes = static_cast<const char*>(plan->data());
    return {bytes, bytes + plan->size()};
}

// 构建中的权重由 list 持有直到 build 完成，不能把局部 vector 的指针留给 TensorRT。
std::vector<char> makeEngine(const std::vector<char>& source, TrtLogger& logger, const std::string& mode,
                            bool sparse, const Scales& activationScales, std::vector<std::string>* observed,
                            std::ostream& report) {
    // 1. 独立解析原 ONNX；每个候选从原模型开始，不累积前一次图修改。
    std::unique_ptr<IBuilder> builder(checked(createInferBuilder(logger), "Cannot create builder"));
    std::unique_ptr<INetworkDefinition> network(checked(builder->createNetworkV2(0), "Cannot create network"));
    std::unique_ptr<nvonnxparser::IParser> parser(checked(nvonnxparser::createParser(*network, logger), "Cannot create ONNX parser"));
    require(parser->parse(source.data(), source.size()), "ONNX parse failed");
    require(network->getNbInputs() == 1, "Require single input SCRFD-10G-KPS");
    auto* input = network->getInput(0);
    const auto originalDims = input->getDimensions();
    require(originalDims.nbDims == 4 && (originalDims.d[0] == 1 || originalDims.d[0] == -1) && originalDims.d[1] == 3 && input->getType() == DataType::kFLOAT,
            "Require NCHW batch=1 RGB Float32 model");
    input->setDimensions(Dims4(1, 3, Side, Side));
    input->setName("images");
    require(network->getNbOutputs() == 9, "Require SCRFD with 3 scales and 5 keypoints (9 outputs)");
    // 官方 SCRFD 导出顺序：三层 score、三层 bbox、三层 kps。统一命名供后处理绑定。
    for (int i = 0; i < 9; ++i) {
        const char* head = i < 3 ? "score" : i < 6 ? "bbox" : "kps";
        network->getOutput(i)->setName((std::string(head) + "_" + std::to_string(8 << (i % 3))).c_str());
    }
    std::list<std::vector<float>> floatStorage;
    std::list<std::vector<cv::float16_t>> halfStorage;
    const int originalLayers = network->getNbLayers();
    auto keepFloat = [&](std::vector<float> values) {
        floatStorage.push_back(std::move(values));
        const auto& saved = floatStorage.back();
        return Weights{DataType::kFLOAT, saved.data(), static_cast<int64_t>(saved.size())};
    };
    auto keepHalf = [&](Weights values) {
        const auto original = copyWeights(values);
        auto& saved = halfStorage.emplace_back();
        saved.reserve(original.size());
        for (float value : original) saved.emplace_back(value);
        return Weights{DataType::kHALF, saved.data(), static_cast<int64_t>(saved.size())};
    };
    auto constant = [&](Dims dims, Weights weights) {
        return checked(network->addConstant(dims, weights), "Cannot add constant")->getOutput(0);
    };
    auto qdq = [&](ITensor& tensor, const std::vector<float>& scales, int axis) {
        const Dims shape = scales.size() == 1 ? Dims{0, {}} : Dims{1, {static_cast<int64_t>(scales.size())}};
        auto* scale = constant(shape, keepFloat(scales));
        auto* q = checked(network->addQuantize(tensor, *scale, DataType::kINT8), "Cannot add Quantize");
        q->setAxis(axis);
        auto* dq = checked(network->addDequantize(*q->getOutput(0), *scale, DataType::kFLOAT), "Cannot add Dequantize");
        dq->setAxis(axis);
        return dq->getOutput(0);
    };

    // 2. FP16 使用显式类型转换。TensorRT 11 为强类型网络，不使用旧版 kFP16/kINT8 flag。
    if (mode == "fp16") {
        for (int i = 0; i < originalLayers; ++i) {
            auto* layer = network->getLayer(i);
            if (layer->getType() == LayerType::kCONSTANT) {
                auto* c = static_cast<IConstantLayer*>(layer);
                if (c->getWeights().type == DataType::kFLOAT && c->getWeights().count) c->setWeights(keepHalf(c->getWeights()));
            } else if (layer->getType() == LayerType::kCONVOLUTION) {
                auto* conv = static_cast<IConvolutionLayer*>(layer);
                if (conv->getKernelWeights().count) conv->setKernelWeights(keepHalf(conv->getKernelWeights()));
                if (conv->getBiasWeights().count) conv->setBiasWeights(keepHalf(conv->getBiasWeights()));
            }
        }
        auto* cast = checked(network->addCast(*input, DataType::kHALF), "Cannot cast input");
        for (int i = 0; i < originalLayers; ++i) {
            auto* layer = network->getLayer(i);
            for (int j = 0; j < layer->getNbInputs(); ++j)
                if (layer->getInput(j) == input) layer->setInput(j, *cast->getOutput(0));
        }
        std::vector<ITensor*> outputs;
        for (int i = 0; i < network->getNbOutputs(); ++i) outputs.push_back(network->getOutput(i));
        for (auto* output : outputs) {
            const std::string name = output->getName();
            network->unmarkOutput(*output);
            output->setName((name + "_half").c_str());
            auto* out = checked(network->addCast(*output, DataType::kFLOAT), "Cannot cast output")->getOutput(0);
            out->setName(name.c_str());
            network->markOutput(*out);
        }
    }

    // 3. 可选 2:4 剪枝；校准引擎也使用相同的剪枝权重，保证统计对应候选图。
    int prunedLayers = 0, quantizedLayers = 0;
    std::map<ITensor*, ITensor*> sharedQdq;
    std::set<ITensor*> marked;
    for (int i = 0; i < originalLayers; ++i) {
        auto* layer = network->getLayer(i);
        if (layer->getType() != LayerType::kCONVOLUTION) continue;
        auto* conv = static_cast<IConvolutionLayer*>(layer);
        auto* activation = conv->getInput(0);
        const std::string activationName = activation->getName();
        if (observed && marked.insert(activation).second) {
            observed->push_back(activationName);
            if (activation != input && !activation->isNetworkOutput()) network->markOutput(*activation);
        }
        if (!sparse && mode != "int8") continue;
        auto weights = copyWeights(kernelOf(*network, *conv));
        require(conv->getNbOutputMaps() > 0 && conv->getNbOutputMaps() <= 4096, "Unsupported convolution size");
        const int k = static_cast<int>(conv->getNbOutputMaps());
        const auto kernel = conv->getKernelSizeNd();
        require(kernel.nbDims == 2, "Require 2D convolution");
        const int spatial = static_cast<int>(kernel.d[0] * kernel.d[1]);
        const int c = static_cast<int>(weights.size() / (static_cast<size_t>(k) * spatial));
        if (sparse && prune24(weights, k, c, spatial)) ++prunedLayers;
        if (mode != "int8") {
            // 覆盖 Constant 输入或普通 kernel 权重，避免修改可能共享的原常量。
            auto* kernelTensor = constant(Dims4(k, c, static_cast<int>(kernel.d[0]), static_cast<int>(kernel.d[1])), keepFloat(std::move(weights)));
            replaceConvolution(*network, *conv, *activation, *kernelTensor);
            continue;
        }
        // 4. PTQ：激活 per-tensor、权重 per-output-channel，对称 INT8，显式 Q → DQ。
        const auto found = activationScales.find(activationName);
        require(found != activationScales.end(), "Missing activation calibration: " + activationName);
        auto& converted = sharedQdq[activation];
        if (!converted) converted = qdq(*activation, {found->second}, 0);
        const auto scales = channelScales(weights, k);
        auto* weightTensor = constant(Dims4(k, c, static_cast<int>(kernel.d[0]), static_cast<int>(kernel.d[1])), keepFloat(std::move(weights)));
        replaceConvolution(*network, *conv, *converted, *qdq(*weightTensor, scales, 0));
        ++quantizedLayers;
    }
    report << mode << ": Q/DQ conv=" << quantizedLayers << ", 2:4 eligible conv=" << prunedLayers << '\n';
    // 5. TensorRT 负责融合、tactic 选择和内存规划；序列化后才允许释放 parser/权重。
    return build(*network, *builder, sparse);
}

cv::Mat decode(const Outputs& output) {
    std::vector<cv::Rect2d> boxes;
    std::vector<float> scores;
    std::vector<std::array<float, 10>> points;
    for (int stride : {8, 16, 32}) {
        const std::string suffix = "_" + std::to_string(stride);
        const auto& cls = output.at("score" + suffix);
        const auto& bbox = output.at("bbox" + suffix);
        const auto& kps = output.at("kps" + suffix);
        const int width = Side / stride;
        const size_t count = static_cast<size_t>(width) * width * 2; // 每个网格两个 anchor。
        require(cls.size() == count && bbox.size() == count * 4 && kps.size() == count * 10,
                "Unexpected SCRFD head shape");
        for (size_t i = 0; i < count; ++i) {
            const float score = cls[i];
            if (score < Score) continue;
            const float x = static_cast<float>((i / 2) % width), y = static_cast<float>((i / 2) / width);
            const float w = (bbox[4 * i] + bbox[4 * i + 2]) * stride, h = (bbox[4 * i + 1] + bbox[4 * i + 3]) * stride;
            require(std::isfinite(w) && std::isfinite(h) && w > 0 && h > 0, "Invalid decoded box");
            boxes.emplace_back((x - bbox[4 * i]) * stride, (y - bbox[4 * i + 1]) * stride, w, h);
            scores.push_back(score);
            std::array<float, 10> landmarks{};
            for (int j = 0; j < 5; ++j) {
                landmarks[2 * j] = (x + kps[10 * i + 2 * j]) * stride;
                landmarks[2 * j + 1] = (y + kps[10 * i + 2 * j + 1]) * stride;
            }
            points.push_back(landmarks);
        }
    }
    std::vector<int> kept;
    cv::dnn::NMSBoxes(boxes, scores, Score, Nms, kept, 1.0f, 5000);
    cv::Mat faces(static_cast<int>(kept.size()), 15, CV_32F);
    for (int row = 0; row < faces.rows; ++row) {
        const int i = kept[row];
        auto* face = faces.ptr<float>(row);
        face[0] = static_cast<float>(boxes[i].x); face[1] = static_cast<float>(boxes[i].y);
        face[2] = static_cast<float>(boxes[i].width); face[3] = static_cast<float>(boxes[i].height);
        std::copy(points[i].begin(), points[i].end(), face + 4);
        face[14] = scores[i];
    }
    return faces;
}
float iou(const float* a, const float* b) {
    const float w = std::max(0.0f, std::min(a[0] + a[2], b[0] + b[2]) - std::max(a[0], b[0]));
    const float h = std::max(0.0f, std::min(a[1] + a[3], b[1] + b[3]) - std::max(a[1], b[1]));
    const float area = a[2] * a[3] + b[2] * b[3] - w * h;
    return area > 0 ? w * h / area : 0;
}
bool agree(const cv::Mat& reference, const cv::Mat& candidate, std::ostream& report) {
    if (reference.rows != candidate.rows) {
        report << "face count mismatch " << reference.rows << " -> " << candidate.rows << '\n';
        return false;
    }
    std::set<int> matched;
    for (int i = 0; i < reference.rows; ++i) {
        const auto* a = reference.ptr<float>(i);
        int best = -1;
        float overlap = 0;
        for (int j = 0; j < candidate.rows; ++j) if (!matched.count(j)) {
            const float value = iou(a, candidate.ptr<float>(j));
            if (value > overlap) { overlap = value; best = j; }
        }
        if (best < 0 || overlap < MinIou) { report << "box IoU=" << overlap << " below " << MinIou << '\n'; return false; }
        matched.insert(best);
        const auto* b = candidate.ptr<float>(best);
        float drift = 0;
        for (int p = 0; p < 5; ++p) drift = std::max(drift, std::hypot(a[4 + 2 * p] - b[4 + 2 * p], a[5 + 2 * p] - b[5 + 2 * p]));
        const float scoreError = std::abs(a[14] - b[14]);
        report << "IoU=" << overlap << ", landmark_max_px=" << drift << ", confidence_delta=" << scoreError << '\n';
        if (drift > MaxPointError || scoreError > MaxScoreError) return false;
    }
    return true;
}
float timeEngine(EngineSession& session, const std::vector<float>& input) {
    // 多组取中位数，减小初始温度和后台任务影响；仍然只是当前 GPU 的微基准。
    std::array<float, 5> times{};
    for (float& time : times) time = session.benchmark(input);
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}
}

void ModelOptimizer::run(const fs::path& onnx, const fs::path& calibration, const fs::path& validation,
                         const fs::path& output, bool sparse, bool smoke) {
    // 1. 校验文件和数据边界；已有交付文件不覆盖，失败候选只保留诊断报告。
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING); // Debug 也只显示可行动的诊断。
    // 手动初始化 ORT：旧 DLL 的 GetApi 会返回 nullptr，不能继续调用 C++ 包装器。
    const auto* ortBase = OrtGetApiBase();
    const auto* ortApi = ortBase ? ortBase->GetApi(ORT_API_VERSION) : nullptr;
    require(ortApi != nullptr, std::string("ONNX Runtime DLL/API mismatch; loaded ") +
            (ortBase ? ortBase->GetVersionString() : "unknown") + "; rebuild to deploy matching DLL beside exe");
    Ort::InitApi(ortApi);
    require(output.extension() == L".engine", "Output must end with .engine");
    require(!fs::exists(output), "Output exists; choose a new output filename");
    const auto source = readFile(onnx);
    const auto calibrationFiles = images(calibration), validationFiles = images(validation);
    if (!smoke) {
        require(!fs::equivalent(calibration, validation), "Calibration and validation must be separate");
        // 先用大小和进程内指纹筛选，再比较实际字节；不在 n×m 循环中重复读所有图片。
        std::multimap<std::pair<size_t, size_t>, fs::path> fingerprints;
        for (const auto& a : calibrationFiles) {
            const auto bytes = readFile(a);
            fingerprints.emplace(std::make_pair(bytes.size(), std::hash<std::string_view>{}({bytes.data(), bytes.size()})), a);
        }
        for (const auto& b : validationFiles) {
            const auto bytes = readFile(b);
            const auto key = std::make_pair(bytes.size(), std::hash<std::string_view>{}({bytes.data(), bytes.size()}));
            const auto range = fingerprints.equal_range(key);
            for (auto match = range.first; match != range.second; ++match)
                require(bytes != readFile(match->second), "Calibration/validation contain the same image bytes");
        }
    }
    fs::create_directories(output.parent_path().empty() ? fs::path(L".") : output.parent_path());
    const fs::path reportPath = fs::path(output.wstring() + L".report.txt");
    std::ostringstream report;
    cudaDeviceProp device{};
    require(cudaGetDeviceProperties(&device, 0) == cudaSuccess, "CUDA device unavailable");
    report << "FaceOptimizer / TensorRT " << NV_TENSORRT_MAJOR << '.' << NV_TENSORRT_MINOR << '.' << NV_TENSORRT_PATCH
           << " / GPU " << device.name << " / SM " << device.major << '.' << device.minor << '\n';
    report << "ONNX Runtime " << ortBase->GetVersionString() << ", source=" << onnx.u8string() << ", source_bytes=" << source.size() << '\n';
    report << (smoke ? "SMOKE ONLY: shared example images; NOT accuracy acceptance.\n" : "DISJOINT SETS: baseline agreement only; NOT ground-truth AP.\n");
    report << "input=Float32 RGB NCHW [1,3,640,640], scale=1/128, mean=127.5, right/bottom zero pad\n"
           << "calibration_images=" << calibrationFiles.size() << ", validation_images=" << validationFiles.size() << '\n'
           << "gate: same count, IoU >= " << MinIou << ", landmark <= " << MaxPointError << "px, confidence delta <= " << MaxScoreError << '\n';
    const fs::path temporary = fs::path(output.wstring() + L".candidate.tmp");
    require(!fs::exists(temporary), "Temporary output exists; choose a new output filename");
    bool createdOutput = false;
    try {
        // 2. FP32 原始引擎与 ORT 参考相互检查；OpenCV DNN 仅作第三方诊断。
        std::cout << "[1/5] Build FP32 baseline\n" << std::flush;
        auto bestPlan = makeEngine(source, m_logger, "fp32", false, {}, nullptr, report);
        EngineSession baseline(bestPlan, m_logger);
        auto detector = cv::dnn::readNetFromONNX(source.data(), source.size());
        detector.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        detector.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        detector.enableWinograd(false); // 对照路径优先数值稳定性，避免 CPU Winograd 累积误差。
        std::vector<cv::Mat> references;
        Ort::Env ortEnv(ORT_LOGGING_LEVEL_WARNING, "reference");
        Ort::SessionOptions ortOptions;
        ortOptions.SetIntraOpNumThreads(1);
        Ort::Session ortSession(ortEnv, source.data(), source.size(), ortOptions);
        Ort::AllocatorWithDefaultOptions ortAllocator;
        auto ortInputName = ortSession.GetInputNameAllocated(0, ortAllocator);
        std::vector<Ort::AllocatedStringPtr> ortNames;
        std::vector<const char*> ortOutputNames;
        require(ortSession.GetOutputCount() == 9, "Unexpected ONNX Runtime output count");
        for (size_t i = 0; i < 9; ++i) ortNames.push_back(ortSession.GetOutputNameAllocated(i, ortAllocator));
        for (const auto& name : ortNames) ortOutputNames.push_back(name.get());
        int referenceFaces = 0;
        for (const auto& path : validationFiles) {
            const auto image = loadImage(path);
            const auto input = inputBlob(image);
            const int shape[] = {1, 3, Side, Side};
            detector.setInput(cv::Mat(4, shape, CV_32F, const_cast<float*>(input.data())));
            std::vector<cv::Mat> raw;
            detector.forward(raw, detector.getUnconnectedOutLayersNames());
            Outputs referenceOutputs;
            // OpenCV 按层拓扑返回输出，以元素数识别尺度/分支，避免依赖数字节点名。
            for (const auto& tensor : raw) {
                require(tensor.dims >= 2, "Invalid OpenCV output shape");
                const int features = tensor.size[tensor.dims - 1];
                const char* head = features == 1 ? "score" : features == 4 ? "bbox" : features == 10 ? "kps" : nullptr;
                require(head != nullptr, "Unknown SCRFD output branch");
                const size_t anchors = tensor.total() / features;
                int stride = 0;
                for (int s : {8, 16, 32}) if (anchors == static_cast<size_t>(Side / s) * (Side / s) * 2) stride = s;
                require(stride != 0, "Unknown SCRFD output scale");
                referenceOutputs[std::string(head) + "_" + std::to_string(stride)] = {tensor.ptr<float>(), tensor.ptr<float>() + tensor.total()};
            }
            const auto cpuFaces = decode(referenceOutputs);
            const int64_t ortShape[] = {1, 3, Side, Side};
            auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            auto ortInput = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data()), input.size(), ortShape, 4);
            const char* ortInputNames[] = {ortInputName.get()};
            auto ortOutputs = ortSession.Run(Ort::RunOptions{nullptr}, ortInputNames, &ortInput, 1, ortOutputNames.data(), ortOutputNames.size());
            Outputs ortReference;
            for (int i = 0; i < 9; ++i) {
                const std::string key = std::string(i < 3 ? "score" : i < 6 ? "bbox" : "kps") + "_" + std::to_string(8 << (i % 3));
                const auto* values = ortOutputs[i].GetTensorData<float>();
                const auto count = ortOutputs[i].GetTensorTypeAndShapeInfo().GetElementCount();
                ortReference[key] = {values, values + count};
            }
            report << "OpenCV vs ONNX Runtime: " << path.filename().u8string() << '\n';
            const auto expected = decode(ortReference);
            report << "opencv_agreement=" << (agree(expected, cpuFaces, report) ? "PASS" : "FAIL") << '\n';
            const auto& baselineOutputs = baseline.run(inputBlob(image));
            for (const auto& entry : ortReference) {
                const auto& values = baselineOutputs.at(entry.first);
                require(values.size() == entry.second.size(), "Raw baseline shape mismatch");
                double error = 0, peak = 0;
                for (size_t i = 0; i < values.size(); ++i) {
                    require(std::isfinite(entry.second[i]), "Non-finite CPU reference output");
                    error = std::max(error, static_cast<double>(std::abs(values[i] - entry.second[i])));
                    peak = std::max(peak, static_cast<double>(std::abs(entry.second[i])));
                }
                report << "raw " << entry.first << ": max_abs_delta=" << error << ", reference_peak=" << peak << '\n';
                require(error <= 1e-3 + 1e-3 * peak, "FP32 raw tensor mismatch: " + entry.first);
            }
            auto actual = decode(baselineOutputs);
            report << "ONNX Runtime vs FP32: " << path.filename().u8string() << '\n';
            require(agree(expected, actual, report), "ONNX Runtime/baseline mismatch; check preprocessing/decoder");
            referenceFaces += actual.rows;
            references.push_back(actual);
        }
        require(referenceFaces > 0, "Validation set has no detected faces; cannot check landmarks");
        const auto benchmarkInput = inputBlob(loadImage(validationFiles.front()));
        float bestTime = timeEngine(baseline, benchmarkInput);
        std::string bestMode = "fp32";
        report << "fp32 bytes=" << bestPlan.size() << ", GPU enqueue median_ms=" << bestTime << '\n';
        writeText(fs::path(output.wstring() + L".fp32.layers.json"), baseline.inspect());
        auto evaluate = [&](const std::string& name, std::vector<char> plan) {
            EngineSession candidate(plan, m_logger);
            bool passed = true;
            for (size_t i = 0; i < validationFiles.size(); ++i) {
                report << name << " vs FP32: " << validationFiles[i].filename().u8string() << '\n';
                passed = agree(references[i], decode(candidate.run(inputBlob(loadImage(validationFiles[i])))), report) && passed;
            }
            const float time = timeEngine(candidate, benchmarkInput);
            report << name << " agreement=" << (passed ? "PASS" : "FAIL") << ", bytes=" << plan.size() << ", GPU enqueue median_ms=" << time << '\n';
            writeText(fs::path(output.wstring() + L"." + fs::path(name).wstring() + L".layers.json"), candidate.inspect());
            if (passed && time < bestTime) { bestPlan = std::move(plan); bestMode = name; bestTime = time; }
        };
        // 3. FP16 保持 Float32 I/O，内部显式半精度；先验证再比较性能。
        std::cout << "[2/5] Build and validate FP16\n" << std::flush;
        evaluate("fp16", makeEngine(source, m_logger, "fp16", false, {}, nullptr, report));
        // 4. 构建观察图，输出卷积输入激活；统计数据来自校准集，不使用验证集。
        std::cout << "[3/5] Collect activation ranges\n" << std::flush;
        Scales scales;
        std::vector<std::string> observed;
        {
            const auto plan = makeEngine(source, m_logger, "calibration", sparse, {}, &observed, report);
            EngineSession calibrationSession(plan, m_logger);
            for (const auto& path : calibrationFiles) {
                const auto input = inputBlob(loadImage(path));
                const auto& outputs = calibrationSession.run(input);
                for (const auto& name : observed) {
                    const auto found = outputs.find(name);
                    require(found != outputs.end() || name == "images", "Calibration output missing: " + name);
                    const auto& values = found == outputs.end() ? input : found->second; // 网络输入不是重复的输出 binding。
                    float peak = 0;
                    for (float value : values) peak = std::max(peak, std::abs(value));
                    scales[name] = std::max(scales[name], std::max(peak / 127.0f, 1e-8f));
                }
            }
        }
        for (const auto& entry : scales) report << "activation_scale " << entry.first << '=' << entry.second << '\n';
        std::cout << "[4/5] Build and validate INT8" << (sparse ? " + 2:4 pruning" : "") << '\n' << std::flush;
        evaluate(sparse ? "int8_sparse" : "int8", makeEngine(source, m_logger, "int8", sparse, scales, nullptr, report));
        // 5. 只交付通过对照的最快候选；已有输出拒绝覆盖，候选失败不冒充优化成功。
        report << "SELECTED=" << bestMode << ", GPU enqueue median_ms=" << bestTime << ", engine_bytes=" << bestPlan.size() << '\n';
        report << "Timing excludes image decode, resize, H2D/D2H, CPU postprocess, OBS rendering.\n"
               << "No labeled dataset / AP / recall / production accuracy acceptance in this run.\n";
        writeText(reportPath, report.str());
        writeFile(temporary, bestPlan.data(), bestPlan.size());
        // 再从磁盘加载一次，验证实际交付字节；成功前不报告完成。
        EngineSession delivered(readFile(temporary), m_logger);
        delivered.run(benchmarkInput);
        fs::rename(temporary, output); // Windows 原子重命名；若目的文件已出现则失败，不能覆盖。
        createdOutput = true;
        std::cout << "[5/5] Selected " << bestMode << ": " << bestTime << " ms (GPU enqueue only)\n"
                  << "Engine: " << output.u8string() << "\nReport: " << reportPath.u8string() << '\n' << std::flush;
    } catch (...) {
        report << "RUN FAILED: engine not accepted.\n";
        writeText(reportPath, report.str());
        // 输出只在所有候选完成后才写入；磁盘加载失败时删除本轮新建的输出。
        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (createdOutput) fs::remove(output, ignored);
        throw;
    }
}

void ModelOptimizer::selfTest() {
    // 1. 2:4 必须沿 C 而不是按扁平数组相邻四项；H×W=2 用于捕获布局错误。
    std::vector<float> weights{1, 8, 4, 7, 3, 6, 2, 5};
    require(prune24(weights, 1, 4, 2), "Pruning not applied");
    require(weights == std::vector<float>({0, 8, 4, 7, 3, 0, 0, 0}), "Incorrect 2:4 axis");
    std::vector<float> depthwise{1, 2, 3};
    require(!prune24(depthwise, 3, 1, 1) && depthwise == std::vector<float>({1, 2, 3}), "Depthwise should be skipped");
    // 2. 独立通道与全零通道尺度，不能共用全局最大值或产生除零。
    const auto scales = channelScales({-127, 63, -254, 0, 0, 0}, 3);
    require(scales[0] == 1 && scales[1] == 2 && scales[2] > 0, "Wrong per-channel scales");
    // 3. 门限会拒绝漏脸及关键点漂移，防止只检查网络输出为非空。
    cv::Mat face = cv::Mat::zeros(1, 15, CV_32F);
    face.at<float>(0, 2) = 100; face.at<float>(0, 3) = 100; face.at<float>(0, 14) = 0.9f;
    std::ostringstream report;
    require(agree(face, face, report), "Identical detections rejected");
    require(!agree(face, cv::Mat(), report), "Missing detection accepted");
    auto shifted = face.clone(); shifted.at<float>(0, 4) = MaxPointError + 1;
    require(!agree(face, shifted, report), "Landmark drift accepted");
    // 4. 人工三尺度输出检查第二个 anchor 和距离解码，避免两个后端共用错误解码仍相互通过。
    Outputs raw;
    for (int stride : {8, 16, 32}) {
        const size_t count = static_cast<size_t>(Side / stride) * (Side / stride) * 2;
        const std::string suffix = "_" + std::to_string(stride);
        raw["score" + suffix].resize(count);
        raw["bbox" + suffix].resize(count * 4);
        raw["kps" + suffix].resize(count * 10);
    }
    const size_t anchor = 2 * (3 * (Side / 8) + 4) + 1;
    raw["score_8"][anchor] = 0.9f;
    for (int i = 0; i < 4; ++i) raw["bbox_8"][4 * anchor + i] = static_cast<float>(i + 1);
    std::fill_n(raw["kps_8"].begin() + 10 * anchor, 10, 0.5f);
    const auto decoded = decode(raw);
    require(decoded.rows == 1 && decoded.at<float>(0, 0) == 24 && decoded.at<float>(0, 1) == 8 &&
            decoded.at<float>(0, 2) == 32 && decoded.at<float>(0, 3) == 48 && decoded.at<float>(0, 4) == 36 &&
            decoded.at<float>(0, 5) == 28, "Wrong SCRFD anchor/distance decoder");
    std::cout << "PASS: pruning layout, quantization scales, detection gates, SCRFD decoder\n";
}
