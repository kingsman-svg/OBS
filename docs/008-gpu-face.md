# 第 008 步：实时 GPU 人脸检测

本步把独立优化工程输出的 SCRFD-10G-KPS `.engine` 接入 OBS 推流端。实现检测定位和框点叠加；磨皮、提亮、精细变形、编码及实际推流后续推进。按最新约定不再绘制类图，继续维护流程时序图。

## 打开并运行

1. Qt Creator 打开 `OBS client/CMakeLists.txt`，使用 MSVC x64 Kit，重新运行 CMake 并构建 `OBS_Publisher`。
2. 本步额外需要 TensorRT **11.2.1.2** 和 CUDA **13.3**，与本机生成引擎的环境一致。设置 `TENSORRT_ROOT`、`CUDA_PATH` 后重启 Qt Creator；也可以在 CMake 配置直接设置 `OBS_TENSORRT_ROOT` 和 `CMAKE_CUDA_COMPILER`。CMake 至少 3.24，本机使用 3.30.5。
3. 默认 `OBS_FACE_ENGINE` 为仓库的 `cpp/out/scrfd_final_smoke.engine`。文件存在时，构建自动复制到可执行目录的 `models/scrfd_10g.engine`。SDK、DLL 和引擎都不提交 Git。换正式引擎可以设置该 CMake 路径或在界面选择其它 `.engine`。
4. 按原方式启动 Docker 登录服务，登录 `root`，选择视频源，勾选 **显示人脸框与五点**，再点击 **开始采集**。
5. 首个检测结果到达后自动打开独立预览：绿色人脸框、红色五点。主窗口显示人脸数、GPU 时间、处理时间、帧龄、待处理帧替换数和 CUDA 注册次数。停止后可修改源/引擎；关闭预览仅隐藏，采集和检测继续运行。

检测默认关闭。引擎不存在、版本/设备不兼容或 GPU 执行失败时，保留独立错误信息并恢复原画面；其它采集路和音频混音继续。不能在运行中修改模型或检测开关，先停止本轮。

检测开关、引擎文件和耗时状态放在独立的“人脸检测”区域。窗口较小时向下滚动查看；完整引擎路径可悬停查看，状态和错误详情自动换行增高。

命令行构建：

```powershell
./scripts/build_clients.ps1 -Deploy
```

当前实测 Qt 6.11.2 / MSVC 14.51 / RTX 5060 8GB。CMake 的 `CMAKE_CUDA_ARCHITECTURES` 默认 120；换 GPU 需要使用支持的编译架构并在目标环境重新生成/验证 engine。D3D11 和 CUDA 必须匹配同一 NVIDIA 适配器；输入属于集显或 WARP 时明确回退，尚未实现跨显卡搬运。摄像头设备及其 WinRT 分配的适配器仍需逐台验证。

官方预训练权重的研究用途边界见 [第 007 步](007-model-optimization.md)。本步沿用通过本机示例验证的 FP16 引擎，不表示正式数据集精度已验收。

## 简单文件分配

应用源码继续平铺在 `OBS client/`：

| 文件 | 职责 |
| --- | --- |
| `GpuFaceDetector.h/.cpp` | 一个公开工作类，负责单帧邮箱、D3D11/CUDA 互操作、引擎加载、推理、结果及 GPU 生命周期 |
| `FacePreprocess.h/.cu` | 一个 CUDA kernel，BGRA→RGB、缩放补边、归一化及 NCHW |
| `FaceDetection.h/.cpp` | 简单帧/人脸数据、SCRFD 解码、NMS、坐标还原；不建立额外 MVC |
| `VideoRenderer.h/.cpp` | 在原有 PixelShader 中叠加同帧框点，仍使用原 SwapChain |
| `SessionController` | 启停检测、定时输入/结果交付、失败回退、登录退出协调 |
| `MainWindow / PreviewWindow` | 开关、路径、状态和非模态展示，不执行模型或 CUDA 操作 |

内部 `Resources` 管理本轮 GPU 资源，运行和销毁均在检测线程；日志对象具有进程级寿命，跨越线程重启。成员和流程使用中文注释，主要步骤按编号说明。

OBS 只链接 TensorRT runtime 与 CUDA，不链接 OpenCV、ONNX Runtime 或 ONNX parser，也不在实时运行时优化模型。现有 FFmpeg 音频与网络依赖保持原职责。

## 每一帧做什么

1. `VideoCapture::publishTexture` 仍发布独立 BGRA8 GPU 快照。`SessionController::deliverCapture` 每 33ms 取最新帧；原 `videoFrameReady` 仍表示原始采集帧，尚未接编码器。
2. 启用检测时交给 `GpuFaceDetector::submit`，输入邮箱最多一帧。较新输入替换旧待处理帧，正在处理的帧独立持有纹理；不积压逐帧 Qt 信号。
3. 首帧沿纹理取得 D3D11 设备和 adapter，选择对应 CUDA device，加载固定 engine，创建执行上下文、stream、event 和命名张量显存。运行中换设备先清理旧资源再初始化。
4. 创建专用中间纹理，CUDA 注册一次。每帧用 `CopyResource` 做 GPU 拷贝并 `Flush`；同尺寸后续帧复用注册，尺寸变化才注销并重建。采集快照和预览纹理不直接暴露给 CUDA，避免并发占用同一映射资源。
5. `map` 后得到 CUDA array 与采样对象。kernel 使用半像素双线性缩放，右/下补黑，BGRA→RGB，`(pixel - 127.5) / 128`，写入 Float32 NCHW `[1,3,640,640]`。内部 FP16 引擎的外部 I/O 仍是 Float32。
6. 等预处理 kernel 完成，在仍映射期间销毁采样对象，再 `unmap`。同一 stream 执行 `enqueueV3`，将九个模型输出读回 CPU 并同步。**视频像素没有 CPU 往返，但模型输出有 D2H；当前不宣称全 GPU 后处理或零拷贝。**
7. `decodeScrfd` 检查形状和有限值，按 stride 8/16/32、每格两 anchor 解码。置信度门限 0.5、NMS IoU 0.45；候选最多 5000，展示最多 16 张脸。按实际整数缩放尺寸分别还原 x/y，并裁剪到原视频边界。
8. `FaceFrame` 将检测结果与同一原始纹理、序号、时间戳一起发布。控制器只取最新完成结果；预览绝不把旧检测框点贴到较新视频帧。
9. `VideoRenderer` 更新框点常量缓冲。原图坐标和纹理共用等比居中视口，缩放/留边/高 DPI 不改变对应关系。框宽和点半径按视口比例保持屏幕大小；Draw 后沿原 SwapChain Present。

CUDA 实现与 CPU OpenCV resize 的 uint8 插值舍入可能相差一个灰度级。本步用独立 CPU 预处理运行同一引擎，对照方形、宽幅、竖幅输入的检测结果，门限为同数量、框 IoU≥0.97、原图五点距离<4px；正式数据仍应扩大覆盖。

## 停止与失败

`GpuFaceDetector::stop` 只在 GUI 线程清空两个邮箱、停止接收、请求中断并唤醒。已提交的一帧可以完成，但不能发布迟到结果。检测线程先同步 GPU，再注销纹理、释放推理上下文、显存、event 和 stream，之后发出 `finished`。控制器等待采集与检测的 `finished` 都已处理才解锁下一轮；析构时才 `wait`。

初始化、读取文件、I/O 契约、D3D11 或 CUDA 错误统一由工作线程捕获。本轮只报错一次，立刻使用已采集的最新原帧恢复预览，不必等静止采集源产生新帧。下一轮停止/开始才重新尝试检测。没有调用 `cudaDeviceReset`，避免影响同进程其它 GPU 使用者。

## 如何调试

可按以下顺序打断点，注意调用线程：

1. GUI：`SessionController::startCapture` → `GpuFaceDetector::begin`，检查开关、路径和待退出线程集合。
2. 检测线程：`GpuFaceDetector::run` → `Resources::initialize`，查看源设备、CUDA ordinal、十个命名 I/O。
3. GUI：`deliverCapture` → `submit`，检查视频序号和单帧邮箱。
4. 检测线程：`Resources::process`，查看 `ensureTexture`、map、kernel、unmap、enqueue、同步和 `decodeScrfd`。
5. GUI：`latestResult` → `showFacePreview` → `VideoRenderer::render`，检查框点与 `video.sequence`、原图尺寸和 viewport。
6. 停止：`stopCapture` → `GpuFaceDetector::stop` → `Resources` 析构 → 检测 `finished`，确认旧结果为空、按钮重新解锁。

普通 C++ 断点无法逐行进入 CUDA kernel；可先对照 CPU 测试与结果坐标，需要 kernel 调试时另用支持本机 CUDA/VS 版本的 GPU 调试工具。

## 验证和实际结果

普通回归不打开有效采集设备：

```powershell
python scripts/run_checks.py
./scripts/run_face_checks.ps1
```

显式硬件验收只显示/捕获测试自己创建的窗口和项目本地示例图片，不打开摄像头、麦克风或整个屏幕：

```powershell
./scripts/build_clients.ps1 -FaceReferenceTests -Deploy
./scripts/run_face_checks.ps1 -Gpu
```

`-FaceReferenceTests` 仅为测试程序增加 OpenCV CPU 预处理和独立 `EngineSession` 对照；业务可执行文件没有该依赖。默认 build 不要求 OpenCV。测试脚本设置 QtTest/可选 OpenCV DLL 路径并抑制 Windows 错误弹窗；硬件 SwapChain 需要正常交互图形会话。

2026-10-10 本机验证：

- Debug 构建通过；登录、协议、采集、音频和人脸五个 CTest 均通过。
- SCRFD 第二 anchor、宽幅逆缩放、NMS、形状/NaN 拒绝、缺失引擎、空闲停止和三次重启通过。
- RTX 5060 的 GPU 纹理 → CUDA → TensorRT → Shader → SwapChain 通过。连续五帧的 CUDA 注册次数均为 1；宽/竖输入各重建一次。CPU 对照、慢消费者替换、清空结果、重新初始化通过。
- 测试窗口的 WGC 输出检出绿色框像素 2992 个、红点像素 192 个；已保存并检查 `out/GPU人脸框点预览.png`。这张测试截图才读回 CPU，业务处理不走截图路径。
- 真实控制器流程覆盖 WGC、缺失引擎回退、再启动有效引擎、独立预览和退出登录；退出后检测邮箱为空。
- 一次记录中，首帧 GPU 预处理+推理 5.41ms，后续四帧为 1.34 / 1.81 / 2.04 / 3.46ms，工作线程处理 3～9ms。控制器首个显示结果帧龄约 59ms。只是短时单人脸示例记录，不能作为稳定帧率或端到端显示延迟保证。

GPU 时间为两对 CUDA event 的总和，覆盖预处理与 enqueue；处理时间从 GPU 纹理拷贝前到 CPU 解码后，排除本轮 engine 初始化；帧龄为控制器交付时的 QPC 差，包含取帧定时和处理，未测显示器扫描完成时间。

原模型的大样本标注精度、不同摄像头/适配器、长期运行及资源评估仍未验收。下一步在检测定位上加入可调磨皮/提亮，精细瘦脸/大眼需要补密集关键点，准确皮肤区域需要分割能力。

时序图：[GPU 人脸检测](uml/GPU人脸检测时序图.png)、[采集](uml/视频采集预览时序图.png)、[GPU预览](uml/GPU预览渲染时序图.png)、[停止](uml/采集停止清理时序图.png)。
