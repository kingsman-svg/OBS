# CourseStudioNext

基于 CourseStudio 进行技术栈提取、模块重构与 AI 能力扩展的新工程。

创建日期：2026-10-06。

## 目标

- 提取网络、音视频采集、FFmpeg 编解码、GPU 渲染与美颜等关键技术。
- 在同一个工程中逐步重新设计模块封装，每个阶段增加一项可运行、可验证的能力。
- 简化 Qt 界面，让界面通过应用层调用模块。
- 将 GPU 美颜接入实际媒体链路，使预览、录制和推流使用明确的处理结果。
- 补充 AI 模型部署和 AI Agent；具体场景与部署边界待讨论。

## 当前进度

已实现同一 CMake 工程中的两个客户端：OBS_Publisher 推流端、OBS_Player 播放端。简单 MVC，共享异步 HTTP 登录与 TCP 信令，已完成节点调度、认证、心跳、房间创建/列表/加入/退出、成员更新、断线重连和会话到期。每个类和流程均提供 UML。

Linux 服务端已实现公共 Reactor 与 timerfd 定时器、独立 HTTP 登录（8081/8082）、健康节点调度（8080）和 TCP JSON 信令（9000）。注释、类图、时序图与真实进程测试同步维护，启动方法见 [服务端说明](docs/server-services.md)。当前使用 root 开发账号，生产账号系统和媒体转发后续实现。

先在 OBS 容器启动 `python3 /workspace/code/run_services.py start`，再用 Qt Creator 打开 `OBS client/CMakeLists.txt`，构建并分别运行 OBS_Publisher 和 OBS_Player。两端使用开发账号 `root`、密码 `root`，默认连接本机 Docker 的 8080 调度与 9000 信令。详见 [双端运行说明](docs/003-two-clients.md)。

推流端已接入 WinRT 摄像头、WGC 窗口/屏幕、WASAPI 麦克风/系统声音，提供设备选择、启停、独立非模态预览和双路音量。主窗口仅放操作面板，小窗口可滚动；首帧打开预览，关闭画面后采集继续，可用“打开画面”恢复。底层只有 VideoCapture、WasapiCapture 两个采集类，由现有工作台控制器协调，预览已改为 D3D11 Shader + 双缓冲 SwapChain，生产路径不读回 CPU；采集与渲染成员及流程有中文编号注释。详见 [采集说明](docs/004-capture.md) 与 [GPU 渲染说明](docs/005-gpu-preview.md)。播放端支持直播/点播页面切换，本地文件与在线地址输入已预留；解码、编码、实际推拉流、GPU 美颜与 AI 继续逐步接入。LocalAuthServer 仅保留为测试支持；生产账号系统和中心令牌撤销尚未实现。当前不增加云助教相关功能。

音频已接入 FFmpeg 7.1 libswresample，统一为 48kHz / 立体声 / Float32 / 10ms，按 QPC 对齐麦克风与系统回环，支持各路音量、静音、缺包补零和削波统计。正常停止排空尾部，退出登录丢弃迟到数据；提供混音后的媒体信号，后续接编码器。构建需配置 FFmpeg shared SDK，详见 [音频重采样与混音](docs/006-audio-mix.md)。

## 工作方式

目录保持简单分级：

```text
CourseStudioNext/
├─ OBS client/       应用源码、界面和 CMakeLists.txt（源码平铺）
├─ OBS server/ Linux 服务端源码和 Docker 开发环境（挂载到 /workspace）
├─ tests/     行为测试
├─ docs/      需求、设计、接口和阶段记录
│  └─ uml/   Mermaid 源码、中文命名的 SVG/PNG 图片
├─ scripts/   构建验证、UML 绘图工具
└─ out/       自动生成的中文页面截图（Git 忽略）
```

Qt Creator 继续打开 `OBS client/CMakeLists.txt`。构建缓存由 CMake/Qt Creator 自动生成，不放入 Git。

- 所有功能在本工程逐步实现，既有 Demo 用作参考。
- 每次只推进一个明确的小步骤：说明设计、实现、运行验证、记录结果。
- 使用 Git 保存完成并验证的小步骤；阶段成果通过提交历史回溯。
- 后续功能在已有版本上继续增加，Qt 界面随核心能力逐步完善。
- UI 采用简单 MVC；每个类配 UML 类图，每个完整流程配 UML 时序图。

## 文档

- [需求与待定事项](docs/requirements.md)
- [现有技术栈初查](docs/technical-baseline.md)
- [分阶段实施路线](docs/roadmap.md)
- [首期网络与登录设计](docs/network-login.md)
- [HTTP 登录接口契约](docs/auth-login.md)
- [UML 图索引](docs/uml.md)
- [构建与验证](docs/build-and-test.md)
- [第 001 步完成记录](docs/001-http-login.md)
- [第 002 步：默认账号与主页](docs/002-root-home.md)
- [第 003 步：两个客户端工作台](docs/003-two-clients.md)
- [第 004 步：音视频采集](docs/004-capture.md)
- [第 005 步：GPU 渲染与 SwapChain](docs/005-gpu-preview.md)
- [第 006 步：音频重采样与混音](docs/006-audio-mix.md)
- [OBS Docker 与 VS Code 连接](OBS%20server/README.md)
- [公共网络库：结构、接口与验证](OBS%20server/code/README.md)
- [定时器与登录、调度、信令服务](docs/server-services.md)

## 参考工程

- 主工程：`C:/projects/CourseStudio`
- 已有技术示例：`MediaCaptureDemo`、`WasapiCaptureDemo`、`WGCDemo`、`DX11PipelineDemo`、`SwapChain2DDemo`、`TexturePoolDemo`、`BeautyEffectDemo`、`BeautyEffectGpuDemo`。

这些工程作为分析和验证的参考；新模块在本目录逐步实现。
