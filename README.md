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

已建立需求初稿、技术栈初查和阶段路线，并明确采用单工程逐步推进、Git 管理阶段成果的方式。当前尚未创建可编译应用，也未完成模型、推理运行时或新构建系统的选型。

## 工作方式

- 所有功能在本工程逐步实现，既有 Demo 用作参考。
- 每次只推进一个明确的小步骤：说明设计、实现、运行验证、记录结果。
- 使用 Git 保存完成并验证的小步骤；阶段成果通过提交历史回溯。
- 后续功能在已有版本上继续增加，Qt 界面随核心能力逐步完善。

## 文档

- [需求与待定事项](docs/requirements.md)
- [现有技术栈初查](docs/technical-baseline.md)
- [分阶段实施路线](docs/roadmap.md)

## 参考工程

- 主工程：`C:/projects/CourseStudio`
- 已有技术示例：`MediaCaptureDemo`、`WasapiCaptureDemo`、`WGCDemo`、`DX11PipelineDemo`、`SwapChain2DDemo`、`TexturePoolDemo`、`BeautyEffectDemo`、`BeautyEffectGpuDemo`。

这些工程作为分析和验证的参考；新模块在本目录逐步实现。
