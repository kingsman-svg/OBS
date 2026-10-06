# 现有技术栈初查

检查日期：2026-10-06。依据本地源码和工程配置；本轮未构建运行，也未完成完整调用图或外部服务审计。

## 已确认的组成

| 领域 | 当前代码/配置 | 重构关注点 |
| --- | --- | --- |
| 工程与界面 | `CourseStudio.pro`：qmake、C++20；Qt Widgets、Network、Multimedia、OpenGL、WebEngine、WebChannel、Concurrent 等模块 | 核对实际依赖；简化界面和构建配置 |
| 业务网络 | `Net/Http/HttpManager.*`：QNetworkAccessManager、REST 请求、文件上传、流式下载；AuthService、ResourceService 等服务 | 分离传输与业务；明确请求关联、取消、超时、错误和重试策略 |
| 摄像头采集 | `Core/CameraCapture.*`：C++/WinRT MediaCapture、MediaFrameReader、CPU/GPU 帧路径 | 设备枚举、格式选择、生命周期和帧所有权 |
| 屏幕/窗口采集 | `Core/WGCCapture.*`：Windows Graphics Capture 与 D3D11 | 与摄像头统一帧契约，保留源类型的差异 |
| 音频采集 | `Core/WasapiCapturer.*`：WASAPI；有麦克风与系统音频相关分支 | 格式描述、时间戳、重采样、混音及启停 |
| GPU 合成与资源 | `Core/DX11Context.*`、`SceneCompositor.*`、`TexturePool.*` | 设备/上下文归属、纹理复用、线程同步及预览路径 |
| 文件解码 | `Core/VideoFileSource.*`、`VideoUtils.*` | 需继续审计解封装、解码、格式转换与播放时序 |
| 编码、录制、推流 | `Record/FFmpegModules.h`、`FFmpegRecorder.*`；VideoEncoder、AudioEncoder、AudioDSP、FileMuxer、StreamMuxer、StreamSender；存在 RTMP/FLV 与 D3D11 硬件帧配置 | 拆分资源管理、编码、封装与输出；验证软硬件路径和音画同步 |
| CPU 美颜与人脸 | `VFX/VideoEffectSystem.*`、`ConcreteEffects.*`、`AdvancedEffects.*`、`FaceTracker.*`；OpenCV FaceDetectorYN | 区分检测/关键点数据与像素效果处理 |
| GPU 滤镜 | `Core/DX11FilterBase.h`、`DX11Filters.h`、`FilterPipeline.h`；D3D11 滤镜与 ping-pong 纹理 | 目前未查到业务接入；需要连接采集、合成和输出 |
| AI 数据准备 | `UI/Live/AIDataBridge.*`：视频采样、音频重采样/切片、队列和上传回调 | 从 UI 目录提取数据适配职责；后续连接明确的 AI 消费者 |

`CourseStudio.pro` 当前引用本机 Qt 6.11.2 路径、FFmpeg 7.1 默认目录和 OpenCV 4.13.0 库名。这是当前工程配置，不代表已验证的新工程依赖组合。

## GPU 美颜接入缺口

1. 在主工程 C++ 源码/头文件中检索 `FilterPipeline`、`DX11BeautyFilter` 等，匹配集中在滤镜定义和管线自身，未查到业务调用点；`Core.pri` 只是将这些头文件纳入工程。
2. `CameraCapture.cpp` 当前获取 Direct3DSurface，但开启 OpenCV 特效时转向 SoftwareBitmap/CPU 路径，再调用 `EffectManager::processPipeline`。
3. `BeautyEffectGpuDemo` 已提供独立 GPU 效果链参考；其 README 描述的演示路径仍包含 CPU 图像上传以及处理后回读，不能直接当作端到端零拷贝链路。

新工程需逐步验证：采集帧 → GPU 效果 → 合成/预览 → 色彩转换 → 编码/输出。
检测模型可异步消费必要的数据，并以明确时间戳关联检测结果；具体实现取决于模型与硬件。

## 可复用的技术示例

| 示例目录（位于 `C:/projects`） | 对应学习/验证主题 |
| --- | --- |
| MediaCaptureDemo | WinRT 摄像头采集 |
| WasapiCaptureDemo | 音频采集 |
| WGCDemo | 屏幕/窗口采集 |
| DX11PipelineDemo | D3D11 渲染阶段 |
| SwapChain2DDemo | GPU 预览与交换链 |
| TexturePoolDemo | 纹理资源复用 |
| BeautyEffectDemo | CPU 美颜参考 |
| BeautyEffectGpuDemo | GPU 效果链与 CPU/GPU 往返成本 |

目前已阅读部分示例说明和工程配置，其余仅确认目录存在。所有示例的运行状态和性能需要后续实际验证。

## 下一轮源码审计

- 画出网络请求、采集、合成、录制/推流和 AI 数据分支的实际调用关系。
- 核对时间戳单位、队列容量、阻塞点、关闭顺序和资源释放。
- 核对硬件编码是否真正使用处理后的 GPU 帧，以及失败时的回退行为。
- 根据需求边界逐个标记模块：直接复用、调整后复用、重新实现、暂不纳入。
