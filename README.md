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

CSN 已实现首期 HTTP 登录客户端：简单 MVC 界面、异步 JSON 请求、超时与取消、响应校验、内存会话和本地退出。每个类和完成的流程均提供 UML。

当前优先网络与登录，不增加云助教相关功能。实际认证服务尚待实现；本轮使用本机 HTTP 测试服务验证客户端，测试响应不代表真实账号认证。

## 工作方式

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
- [HTTP 登录接口契约](docs/api/auth-login.md)
- [UML 图索引](docs/uml/README.md)
- [构建与验证](docs/build-and-test.md)
- [第 001 步完成记录](docs/steps/001-http-login.md)

## 参考工程

- 主工程：`C:/projects/CourseStudio`
- 已有技术示例：`MediaCaptureDemo`、`WasapiCaptureDemo`、`WGCDemo`、`DX11PipelineDemo`、`SwapChain2DDemo`、`TexturePoolDemo`、`BeautyEffectDemo`、`BeautyEffectGpuDemo`。

这些工程作为分析和验证的参考；新模块在本目录逐步实现。
