#include "ModelOptimizer.h"
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

int wmain(int argc, wchar_t** argv) {
    try {
        // 1. 只解析明确的参数；拼写错误或缺参数时退出，不悄悄退回示例数据。
        std::map<std::wstring, std::filesystem::path> paths;
        bool sparse = false, smoke = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring key = argv[i];
            if (key == L"--help") {
                std::cout << "FaceOptimizer --smoke [--sparse] [--output path.engine]\n"
                          << "FaceOptimizer --onnx path.onnx --calibration image_dir --validation image_dir --output path.engine [--sparse]\n"
                          << "FaceOptimizer --self-test\n"
                          << "Formal mode requires disjoint calibration/validation images. Existing output is never overwritten.\n";
                return 0;
            }
            if (key == L"--self-test") { ModelOptimizer::selfTest(); return 0; }
            if (key == L"--sparse") { sparse = true; continue; }
            if (key == L"--smoke") { smoke = true; continue; }
            if (key != L"--onnx" && key != L"--calibration" && key != L"--validation" && key != L"--output") throw std::runtime_error("Unknown argument");
            if (++i >= argc || paths.count(key)) throw std::runtime_error("Missing or duplicate argument");
            paths[key] = argv[i];
        }
        if (argc == 1) throw std::runtime_error("Specify --smoke or explicit data paths; see --help");
        // 2. F5 默认示例来自 assets；相对路径以 Visual Studio 工作目录 cpp 为基准。
        if (smoke) {
            if (!paths.count(L"--onnx")) paths[L"--onnx"] = L"out/assets/scrfd_10g.onnx";
            if (!paths.count(L"--calibration")) paths[L"--calibration"] = L"out/assets";
            if (!paths.count(L"--validation")) paths[L"--validation"] = L"out/assets";
            if (!paths.count(L"--output")) {
                const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                paths[L"--output"] = std::wstring(sparse ? L"out/scrfd_sparse_smoke_" : L"out/scrfd_smoke_") + std::to_wstring(stamp) + L".engine";
            }
        }
        for (const auto* key : {L"--onnx", L"--calibration", L"--validation", L"--output"})
            if (!paths.count(key)) throw std::runtime_error("Missing required path; see --help");
        // 3. 所有构建、校准、剪枝、验证在这个 C++ 程序内完成，不调用 Python。
        ModelOptimizer optimizer;
        optimizer.run(paths.at(L"--onnx"), paths.at(L"--calibration"), paths.at(L"--validation"), paths.at(L"--output"), sparse, smoke);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
