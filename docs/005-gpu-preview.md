# 第 005 步：GPU 渲染与 SwapChain

独立预览窗口现在直接消费 `VideoFrame.texture`，通过 D3D11 Shader 绘制到双缓冲 flip SwapChain。移除了采集层原先的 staging、Map、QImage 缩放及低频预览缓存。摄像头驱动只提供 SoftwareBitmap 时仍保留 CPU → GPU 上传，这是输入兼容路径。

## 封装与数据流

源码继续平铺在 `OBS client/`，只增加一个 VideoRenderer，不增加采集 Model/Controller 或独立 Demo。

| 对象 | 职责 |
| --- | --- |
| VideoCapture | WinRT/WGC 采集，GPU 复制独立快照，发布最新帧邮箱 |
| VideoFrame | 单层 BGRA8 ShaderResource 纹理、QPC 时间戳、帧序号 |
| SessionController | GUI 每 33ms 取最新帧，交付媒体入口和预览，不积压旧帧 |
| PreviewWindow | 非模态窗口、首帧打开、关闭隐藏、文字占位和错误提示 |
| VideoRenderer | 原生子窗口、同设备纹理采样、等比缩放、SwapChain 提交与资源释放 |

当前：采集 → 独立 GPU 快照 → 最新帧邮箱 → VideoRenderer → Shader → SwapChain → 窗口。

后续：采集 → GPU 特效处理 → 处理后的 VideoFrame → 预览和编码。模型与特效尚未实现，不预先建立多层空接口。现有帧契约保留 GPU 资源所有权和时间信息，供下一阶段沿用。

## 渲染顺序与资源边界

1. Qt 创建原生子窗口，设置 WA_PaintOnScreen，paintEngine 返回空，Qt 只管理布局和事件。
2. 第一帧沿 texture.GetDevice 取得采集设备，避免另建渲染设备造成跨设备拷贝。输入设备或 HWND 变化时释放旧交换链后重建。
3. 创建独立 DeferredContext，编译全屏三角形 VS/纹理采样 PS，创建线性采样和二维绘制状态。
4. 沿 DXGI Device → Adapter → Factory 创建 HWND 双缓冲 flip 交换链，禁止 Alt+Enter 自动全屏。
5. 从 GetClientRect 获取物理像素尺寸；释放旧 RTV 后 ResizeBuffers，为新后缓冲创建 RTV。与 Qt 的逻辑尺寸分开，适配高 DPI。
6. 当前输入创建 SRV，清空后缓冲，计算居中等比视口，绑定 Shader，Draw(3)。不建立顶点缓冲，也不通过 CPU 缩放视频。
7. FinishCommandList(FALSE) 重置录制状态，ExecuteCommandList(TRUE) 恢复共享立即上下文状态。立即上下文启用 ID3D11Multithread 保护，渲染提交与采集复制不会同时执行。
8. Present(0, DO_NOT_WAIT) 避免等待垂直同步；忙碌时最多保留一个 16ms 延迟重试，静态首帧也能再次提交。遮挡时等待下一帧或窗口重绘事件。framePresented 只表示 Present 接受帧，不是显示器扫描完成回调。
9. 停止/退出时清除帧引用和渲染资源。只 ClearState 自己的 DeferredContext，不清空共享立即上下文；释放旧 flip 链后 Flush，允许相同 HWND 绑定下一条链。

GUI 是 VideoRenderer 的唯一调用线程。绘制状态隔离防止影响 WinRT/采集；测试设置生产者 LINESTRIP 状态，绘制后查询确认该状态保留。

关闭预览只隐藏，采集继续且缓存仍更新；再次打开显示最新帧。最小化时跳过绘制。停止/退出登录清理并重置首帧标志。格式、Shader、交换链或设备失败时显示错误，采集与音频继续；本轮错误锁存，停止后重新开始可重试，不逐帧刷错误，也不自动改成 CPU 预览。

## 注释与调试入口

VideoCapture、WasapiCapture、VideoFrame、AudioPacket、CaptureSource 和新增渲染类的成员均有中文用途说明。关键实现使用编号注释，摄像头顺序为：设备 ID → MediaCapture 初始化 → Color FrameSource → BGRA MediaFrameReader → 注册 FrameArrived → StartAsync → 最新帧/转换/复制/输出 → 撤销事件、StopAsync、Close。

| 断点 | 观察内容 |
| --- | --- |
| VideoCapture::publishTexture | source 与输出 texture、GetDevice、时间戳、递增 sequence |
| SessionController::deliverCapture | m_lastSequence 去重，传给预览的是同一张 GPU 快照 |
| VideoRenderer::initialize | m_device、m_window、m_deferred、m_swapChain 的首次创建 |
| VideoRenderer::resizeBuffers | m_bufferSize 与 HWND 物理尺寸，旧 RTV 释放后重建 |
| VideoRenderer::render | SRV、viewport、Draw、CommandList、Present 返回值 |
| VideoRenderer::releaseResources | 停止或换设备时 COM 引用和交换链释放 |

采集线程停在断点时 GUI 仍可能继续；暂停整个进程会改变实时采集时序。先看初始化，再用日志或条件断点检查 sequence，避免逐帧单步误判性能。

## 构建与验证

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_clients.ps1 -Deploy
$env:PATH = "C:/software/Qt/Tools/CMake_64/bin;" + $env:PATH
python scripts/run_checks.py
python scripts/run_capture_checks.py --window-preview
```

需要 Windows 正常交互图形会话。受限沙箱可能没有窗口 expose/paint 事件，原生 SwapChain 不能用 offscreen 代替验收。测试专用 colorFrame 用 WARP 产生可重复纹理；真实采集到预览的测试使用 WGC 的硬件 D3D11 设备。生产渲染始终复用输入设备。

2026-10-08，Qt 6.11.2 / MSVC x64 Debug：

- 两个客户端及三个测试程序构建通过。
- 三个常规 CTest 全通过，9.00 秒；包括共享上下文状态恢复、换设备、非契约纹理失败、清理后恢复、窗口生命周期和登录回归。测试中出现一次 BGRA 格式错误是主动注入的失败分支。
- `--window-preview` 通过：WGC 捕获自建四象限 GPU 窗口，核对实际颜色、方向、留边、ResizeBuffers、设备切换和最小化恢复；并验证真实 WGC 色块到工作台 GPU 预览、关闭继续采集、重开、退出、快照寿命和目标关闭。
- 200% DPI 的 `--ui-only` 通过，无窗口几何尺寸警告。
- 生成并检查 36 张类图、31 张时序图。新增 VideoRenderer 类图和 GPU 预览时序图，旧采集/预览/停止流程同步修改。

截图仅来自测试自建窗口：`out/GPU四象限渲染验证.png`、`out/独立窗口真实采集.png`；没有保存用户桌面、摄像头或音频。Qt QWidget::grab 不包含原生 SwapChain 输出，测试通过 WGC 观察窗口再做测试专用读回，生产代码无该读回。

本步没有新的摄像头/麦克风硬件验收；之前本机没有这两类设备。没有实现 GPU 美颜、模型推理、FFmpeg 编码或实际推流，也未做性能/显存基准。依赖新增 d3dcompiler，windeployqt 已处理 D3DCompiler DLL。

## 下一阶段

先完成音频重采样与混音：统一采样率、声道与固定帧长，按 QPC 对齐麦克风/回环，明确缺包补零、增益/静音、削波和停止排空。采集模块保持原始格式输出，音频处理单独封装。

随后确定视觉任务、模型许可证与目标 GPU 后端，先测原模型的画质、延迟和显存，再尝试 FP16 精度转换及 INT8 量化。剪枝按真实瓶颈决定，验证微调条件及后端是否实际加速。效果优先围绕明确需求收敛：常规磨皮/调色可用 Shader，人脸关键点或分割模型提供区域/几何信息，再把结果接入同一视频 GPU 链路。暂不预定模型或承诺所有后端零拷贝。

## API 参考

- [Qt QWidget 原生绘制与 paintEngine](https://doc.qt.io/qt-6/qwidget.html)
- [ExecuteCommandList 的状态恢复](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist)
- [FinishCommandList 的录制状态](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-finishcommandlist)
- [ResizeBuffers 的后缓冲引用要求](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers)
- [CreateSwapChainForHwnd 与同 HWND 交换链生命周期](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd)
- [ONNX Runtime 量化、校准与精度验证](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)
