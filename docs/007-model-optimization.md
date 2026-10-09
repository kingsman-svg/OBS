# 第 007 步：独立 C++ TensorRT 模型优化工程

本步在 `cpp/` 新建 Visual Studio 原生 C++ 工程，模型改为 **SCRFD-10G-KPS**。只推进离线优化与验证；OBS 客户端没有新增模型依赖，也没有修改采集、音频或渲染流程。

## 模型与优化选择

用户提供的《26-AI部署学习路线(1)(2).xlsx》，`Sheet1 (2)!C11:D20` 涵盖 TensorRT、FP16/INT8、PTQ/QAT、剪枝、层融合和内存优化。本步选其中适合当前模型和 C++ 部署的内容：

| 技术 | 本步实现 | 边界 |
| --- | --- | --- |
| ONNX → TensorRT | C++ parser / builder，序列化 `.engine` | 原始 ONNX 保留，输出不再是便携 ONNX |
| FP32 基线 | ONNX Runtime C++ CPU 对照 TensorRT，关闭 TF32 | 原始张量和检测结果对照；OpenCV DNN 仅诊断 |
| FP16 | 显式转换内部权重和计算类型，保持 Float32 I/O | TensorRT 11 强类型网络，不使用旧版 kFP16 flag |
| INT8 PTQ | 校准图观察卷积输入，max-abs 对称激活尺度；权重按输出通道尺度；显式 Q/DQ | 无 QAT，无训练；激活保留其它非卷积浮点支路，最终执行精度由层报告核查 |
| 2:4 剪枝 | 可选 `--sparse`，沿卷积输入通道四取二；开启 sparse tactic 选择 | 跳过 C/group 不为 4 倍数的层；未删除通道、未微调；稀疏资格不保证加速 |
| 融合与内存 | TensorRT 自动融合和内存规划，复用命名 I/O device 缓冲 | 不为本例重复实现 TensorRT 内存池 |
| 测量 | CUDA event、预热、多组中位数、引擎层 JSON | 时间仅覆盖 enqueue，不包含预处理、传输、后处理及 OBS |

SCRFD-10G-KPS 官方列出约 **4.23M 参数**，含人脸框和五关键点；不带 KPS 的 10G 版本约 3.86M。10G 是计算量级，不能理解为占用 10GB 显存。输入固定为 batch=1、640×640，先测单 stream。更大的 34G、动态尺寸、多 stream、CUDA Graph 与自定义 CUDA 后处理留到有测量依据时再考虑。

官方代码采用 MIT，但**官方预训练权重限非商业研究使用**。当前用于学习与本机验证；商用需要适当授权或自行训练的可用权重。下载的模型、示例图片与引擎都放在 Git 忽略的 `cpp/out/`。

官方来源：

- [SCRFD 模型规模、KPS 与导出说明](https://github.com/deepinsight/insightface/tree/master/detection/scrfd)
- [预训练模型授权](https://github.com/deepinsight/insightface#license)
- [官方 model-zoo 发布包](https://github.com/deepinsight/insightface/releases/tag/model-zoo)，只提取 buffalo_l 中的 `det_10g.onnx`
- [官方 SCRFD 预处理和解码](https://github.com/deepinsight/insightface/blob/master/detection/scrfd/tools/scrfd.py)
- [TensorRT C++ 构建与运行](https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/c-api-docs.html)
- [显式量化](https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/work-with-quantized-types.html)

## 文件分配

```text
cpp/
  FaceOptimizer.sln / .vcxproj   Visual Studio 入口，Debug/Release x64
  main.cpp                      CLI 参数和错误出口
  TrtLogger.h                   TensorRT 日志
  ModelOptimizer.h / .cpp        图转换、校准、剪枝、对照与候选选择
  EngineSession.h / .cpp         最小固定形状 TensorRT 推理会话
  out/                          模型、图片、构建产物、报告、engine（忽略）
```

手写类保持三个。成员函数和变量均有中文注释，cpp 的主要流程按 1/2/3 编号。三张类图、优化和推理两个时序图在 [UML 索引](uml.md)。

## 安装与打开

当前工程适配 Windows x64、Visual Studio 2026 / v145、TensorRT **11.2.1.2**、CUDA **13.3**、OpenCV **4.13.0**、ONNX Runtime C++ CPU **1.30.0**。工程链接对应的 `nvinfer_11.lib`、`nvonnxparser_11.lib`、`cudart.lib`、`onnxruntime.lib` 与 `opencv_world4130[d].lib`。换 SDK 时需要同步项目库名和工具集；不能只改环境变量后假定任意版本兼容。

环境变量：

```text
TENSORRT_ROOT = TensorRT SDK 根目录（有 include、lib、bin）
CUDA_PATH = CUDA SDK 根目录
```

设置后重启 Visual Studio，先准备模型与参考 SDK，再打开 `cpp/FaceOptimizer.sln`。`OpenCVRoot` 默认 `C:/dev/opencv/opencv/build`，可在项目属性或 MSBuild 参数中覆盖。`OnnxRuntimeRoot` 默认指向 `cpp/out/sdk/onnxruntime-win-x64-1.30.0`。F5 的工作目录为 cpp，默认参数为 `--smoke`，调试 PATH 加入 SDK 的 DLL 目录。构建后把匹配的 ONNX Runtime DLL 复制到 exe 目录，优先于系统旧 DLL；代码在使用 ORT C++ 包装器前检查 API 指针，版本不匹配明确报错退出，防止空指针访问。没有将 SDK、DLL 或模型放入源码或 Git。

从仓库根目录使用 PowerShell：

```powershell
./scripts/prepare_face_model.ps1 -SampleImage 'C:/你的目录/人脸示例.jpg'
./scripts/build_face_optimizer.ps1 -Configuration Release
./scripts/run_face_optimizer.ps1 --self-test
./scripts/run_face_optimizer.ps1 --smoke
```

下载脚本首次下载官方 ONNX Runtime C++ SDK 与 buffalo_l 包，提取唯一 `det_10g.onnx` 为 `cpp/out/assets/scrfd_10g.onnx`，校验 SHA256 `5838f7fe053675b1c7a08b633df49e7af5495cee0493c7dcf6697200b85b5b91`。`-SampleImage` 只将本地图片复制成中文文件名；不提供时需自行在该目录放入图片。Git 不提交图片或预训练模型。示例默认输出名包含时间戳，可以反复 F5。显式指定已存在的输出时程序拒绝覆盖；可用新名称，例如：

```powershell
./scripts/run_face_optimizer.ps1 --smoke --output out/第二次验证.engine
./scripts/run_face_optimizer.ps1 --smoke --sparse --output out/稀疏实验.engine
```

`--smoke` 明确允许共用示例图，只验通路；这不是正式量化数据或精度验收。两组正式图片准备好后运行：

```powershell
./scripts/run_face_optimizer.ps1 --onnx out/assets/scrfd_10g.onnx --calibration 'C:/数据/校准' --validation 'C:/数据/验证' --output out/scrfd_checked.engine
```

每个目录直接放 jpg/jpeg/png/bmp，不递归；各允许 1～1000 张。正式模式拒绝相同目录和字节相同的跨集图片，但不能识别重新编码/裁剪后的近重复，仍需要人工正确划分。校准集应覆盖实际摄像头、光照、肤色、姿态、遮挡、背景及无脸画面；验证集包含独立人脸和背景。当前指标是相对基线的一致性，**没有标注时不能宣称 AP 或召回率达标**。

## 优化与交付过程

1. 检查数据和输出。已有 engine 拒绝覆盖。
2. 解析原 ONNX，固定输入 `[1,3,640,640]`，构建 FP32。用 ONNX Runtime CPU 运行原 ONNX，与 TensorRT 原始张量及检测结果对照，排除输入/解码错误；每个张量最大绝对误差不得超过 `0.001 + 0.001 × 参考峰值`，检测结果还需通过下述门限。额外记录 OpenCV DNN 的差异。
3. 独立解析原图构建 FP16。所有候选都从原模型开始，避免叠加修改。
4. 构建校准观察图，输出卷积输入；只使用校准图片统计各激活 max-abs。`--sparse` 时观察图使用相同剪枝权重。
5. 独立解析后加入 INT8 Q/DQ；卷积权重 per-output-channel、激活 per-tensor，尺度非零。`--sparse` 只改变这条候选和对应校准图，不改变 FP32/FP16 基线。
6. 对照验证集：人脸数一致、匹配框 IoU ≥ 0.90、五点最大漂移 ≤ 4 个输入图像像素、置信度差 ≤ 0.10。没有检测到任何人脸的验证集拒绝通过。门限仅是工程初始设置，不代替业务精度要求。
7. 每个候选预热后测五组、每组 30 次 GPU enqueue，取组均值的中位数。选择通过一致性检查的最快候选；量化或剪枝失败门限时不会强行选它，FP32 也可以作为回退。
8. 保存报告及层 JSON，暂存引擎，从暂存文件重载并真实推理，再重命名为最终 `.engine`。失败删除本轮暂存，保留诊断；不删除已有交付。

输出包括：

- `<name>.engine`：本机通过本次相对一致性检查的最快候选。
- `<name>.engine.report.txt`：设备、TensorRT、数据量、量化尺度、各候选偏差、大小、测速和最终选择。
- `<name>.engine.fp32/fp16/int8[或int8_sparse].layers.json`：实际层信息，可检查融合、数据类型与 tactic。插入 Q/DQ 的层数不能直接当作实际 INT8 kernel 数。

输入保持 Float32 RGB NCHW，像素先保持比例缩放，右/下补黑，再 `(pixel-127.5)/128`。输出统一命名 `score_8/16/32`、`bbox_8/16/32`、`kps_8/16/32`；每网格两个 anchor。框距离和关键点偏移按 stride 解码，CPU 使用 OpenCV NMS。检测坐标位于补边后的 640×640 图，未来接 OBS 时还需逆缩放到视频坐标。

## 与未来 OBS GPU 链路的关系

OpenCV DNN / FaceDetectorYN 不能直接加载 `.engine`。OBS 后续由 TensorRT Runtime 加载交付，复用原始 ONNX、预处理契约和后处理，不能只拿 engine 文件而忽略输入输出定义。

预期链路为采集 D3D11 纹理 → D3D11/CUDA 互操作 → GPU 缩放与 BGRA→RGB/NCHW → TensorRT → 人脸框/五点 → 美颜 Shader → SwapChain 或编码器。五点只是基础定位；精细脸部塑形可能需要更多关键点或分割模型，不能把五点检测当成完整美颜网络。

本步尚未实现纹理互操作或实时调度。未来须验证 CUDA 与 D3D11 使用同一 NVIDIA 适配器、纹理格式和资源同步，避免纹理仍在使用时重写；推理线程保持有界队列，允许低频检测复用最近位置。引擎默认与平台、GPU 和 TensorRT 版本相关，不承诺换机器直接可用；换部署环境应使用原 ONNX 重建。

## 验证记录

2026-10-09，Windows / RTX 5060 8GB / SM 12.0 / 驱动 616.92，使用前述 SDK 实测：

- Release x64 与 Debug x64 构建通过，无编译警告；两个配置均通过算法自测及 CLI/文件/数据隔离检查。
- 与 Visual Studio F5 相同的 Debug 默认 `--smoke` 路径实跑通过，自动生成带时间戳的引擎和报告；示例不依赖 Qt 或 OBS 服务端。
- 算法自测覆盖非平面相邻的 KCHW 2:4 布局、跳过 depthwise、每输出通道尺度和全零尺度、人脸数/关键点验收、SCRFD 第二个 anchor 和距离解码。
- 已有交付保持原字节；正式模式拒绝不同目录中的相同图片字节；无效 CLI 明确非零退出。
- 故意缺少 exe 旁的 ORT DLL，让 Windows 加载系统 1.17.1：明确输出 `DLL/API mismatch` 并返回 1，没有再次解引用空 API。构建部署匹配 1.30.0 DLL 后正常运行。
- 本机 OpenCV 4.13 DNN 与 ORT 的示例检测相差约 5.17 像素，关闭 Winograd 后仍存在。本步不将 OpenCV DNN 作为该模型的精度参考，不推断具体算子缺陷；ORT 与 TensorRT 的原始张量最大差异约 `6e-6`，人脸框 IoU≈1、关键点约 `3e-5` 像素。

最终 Release 稠密示例运行（单张已有工程图片同时用于校准和验证，仅 smoke）：

| 候选 | 引擎字节 | GPU enqueue 毫秒 | 检测一致性 |
| --- | ---: | ---: | --- |
| FP32 | 26,238,892 | 3.1464 | ORT 对照通过 |
| FP16 | 9,903,668 | 1.53405 | 通过 |
| INT8 Q/DQ | 7,682,348 | 1.89791 | 通过，关键点最大偏移约 0.41px |

这次选择 **FP16**；相对本轮 FP32，GPU enqueue 约快 2.05 倍。结果文件为 `cpp/out/scrfd_final_smoke.engine`，对应报告和实际层 JSON 同目录；交付文件已重新加载并推理。INT8 插入 58 个卷积的 Q/DQ，层信息可见真实 Int8 张量和 i8i8 tactic；INT8 更小但本轮没有更快。

单独的 `--sparse` 示例运行对 57 个符合条件的卷积做 2:4 剪枝，层信息出现实际 sparse tactic；INT8+稀疏候选从 1 张人脸变成 0 张，**未通过**，最终选择未剪枝 FP16。这是组合候选失败，不是单独的剪枝 AP 评估，也没有执行精度恢复训练。不能把该稀疏候选用于 OBS。

构建时间和 tactic 会受设备负载影响，不同运行的大小/耗时会变化。以上不是完整视频链路速度，也不是通用精度结论。没有独立正式数据或标注，因此未测 WIDER FACE AP、召回率及跨人群表现。下阶段先准备独立数据验收，然后再接 OBS 的 GPU 推理链路。

复测命令：

```powershell
./tests/FaceOptimizerTests.ps1 -Configuration Release
./tests/FaceOptimizerTests.ps1 -Configuration Debug
./scripts/run_face_optimizer.ps1 -Configuration Release --smoke
./scripts/run_face_optimizer.ps1 -Configuration Release --smoke --sparse
```

Python 仅用于仓库既有 UML 绘图工具，不参与模型优化或推理；工程可直接用 Visual Studio 构建、F5 调试，模型相关计算全部是 C++。
