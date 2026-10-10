# UML 图索引

按用户最新约定，后续不再新增或更新类图；以下类图保留为历史阶段资料，可能与最新成员不同。完整流程仍维护时序图。
`.mmd` 为可编辑 Mermaid 源码，`.svg` 为矢量图，`.png` 为预览图。Qt 库类及自动生成的 UI 类型仅作为关联节点显示。

## 类图

采集新增类图（采集没有单独 Model/Controller）：

| 类 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| CaptureSource | [源码](uml/CaptureSource.mmd) | [采集源数据类图](uml/采集源数据类图.png) | [矢量图](uml/采集源数据类图.svg) |
| VideoFrame | [源码](uml/VideoFrame.mmd) | [视频帧数据类图](uml/视频帧数据类图.png) | [矢量图](uml/视频帧数据类图.svg) |
| AudioPacket | [源码](uml/AudioPacket.mmd) | [音频包数据类图](uml/音频包数据类图.png) | [矢量图](uml/音频包数据类图.svg) |
| VideoCapture | [源码](uml/VideoCapture.mmd) | [视频采集类图](uml/视频采集类图.png) | [矢量图](uml/视频采集类图.svg) |
| WasapiCapture | [源码](uml/WasapiCapture.mmd) | [音频采集类图](uml/音频采集类图.png) | [矢量图](uml/音频采集类图.svg) |
| VideoRenderer | [源码](uml/VideoRenderer.mmd) | [视频GPU渲染类图](uml/视频GPU渲染类图.png) | [矢量图](uml/视频GPU渲染类图.svg) |
| PreviewWindow | [源码](uml/PreviewWindow.mmd) | [独立画面窗口类图](uml/独立画面窗口类图.png) | [矢量图](uml/独立画面窗口类图.svg) |
| AudioResampler | [源码](uml/AudioResampler.mmd) | [音频重采样类图](uml/音频重采样类图.png) | [矢量图](uml/音频重采样类图.svg) |
| AudioMixer | [源码](uml/AudioMixer.mmd) | [音频混音类图](uml/音频混音类图.png) | [矢量图](uml/音频混音类图.svg) |
| AudioMixTrack | [源码](uml/AudioMixTrack.mmd) | [混音输入状态类图](uml/混音输入状态类图.png) | [矢量图](uml/混音输入状态类图.svg) |

MainWindow、SessionController 类图及退出登录时序已同步更新。采集时序图：

- [设备枚举源码](uml/capture-devices.mmd) · [设备枚举图](uml/采集设备枚举时序图.png) · [SVG](uml/采集设备枚举时序图.svg)
- [视频采集源码](uml/capture-video.mmd) · [视频采集图](uml/视频采集预览时序图.png) · [SVG](uml/视频采集预览时序图.svg)
- [双路音频源码](uml/capture-audio.mmd) · [双路音频图](uml/双路音频采集时序图.png) · [SVG](uml/双路音频采集时序图.svg)
- [停止清理源码](uml/capture-stop.mmd) · [停止清理图](uml/采集停止清理时序图.png) · [SVG](uml/采集停止清理时序图.svg)
- [GPU预览渲染源码](uml/gpu-preview.mmd) · [GPU预览渲染图](uml/GPU预览渲染时序图.png) · [SVG](uml/GPU预览渲染时序图.svg)
- [独立画面窗口源码](uml/preview-window.mmd) · [独立画面窗口图](uml/独立画面窗口时序图.png) · [SVG](uml/独立画面窗口时序图.svg)
- [音频重采样源码](uml/audio-resample.mmd) · [音频重采样图](uml/音频重采样时序图.png) · [SVG](uml/音频重采样时序图.svg)
- [双路混音源码](uml/audio-mix.mmd) · [双路混音图](uml/双路音频混音时序图.png) · [SVG](uml/双路音频混音时序图.svg)
- [混音停止源码](uml/audio-finish.mmd) · [混音停止排空图](uml/混音停止排空时序图.png) · [SVG](uml/混音停止排空时序图.svg)

双端工作台新增类图：

| 类 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| SignalClient | [源码](uml/SignalClient.mmd) | [客户端信令类图](uml/客户端信令类图.png) | [矢量图](uml/客户端信令类图.svg) |
| SessionModel | [源码](uml/SessionModel.mmd) | [工作台模型类图](uml/工作台模型类图.png) | [矢量图](uml/工作台模型类图.svg) |
| SessionController | [源码](uml/SessionController.mmd) | [工作台控制器类图](uml/工作台控制器类图.png) | [矢量图](uml/工作台控制器类图.svg) |

ClientRole 是区分两个入口的枚举，不含手写类。MainWindow、LoginController、HttpClient 及相关时序已同步更新；LocalAuthServer 只保留为测试支持。

公共网络库类图：

| 类 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| UniqueFd | [源码](uml/UniqueFd.mmd) | [文件描述符管理类图](uml/文件描述符管理类图.png) | [矢量图](uml/文件描述符管理类图.svg) |
| Buffer | [源码](uml/Buffer.mmd) | [字节缓冲类图](uml/字节缓冲类图.png) | [矢量图](uml/字节缓冲类图.svg) |
| Channel | [源码](uml/Channel.mmd) | [事件通道类图](uml/事件通道类图.png) | [矢量图](uml/事件通道类图.svg) |
| Poller | [源码](uml/Poller.mmd) | [事件轮询类图](uml/事件轮询类图.png) | [矢量图](uml/事件轮询类图.svg) |
| EventLoop | [源码](uml/EventLoop.mmd) | [事件循环类图](uml/事件循环类图.png) | [矢量图](uml/事件循环类图.svg) |
| Acceptor | [源码](uml/Acceptor.mmd) | [连接接收器类图](uml/连接接收器类图.png) | [矢量图](uml/连接接收器类图.svg) |
| TcpConnection | [源码](uml/TcpConnection.mmd) | [连接管理类图](uml/连接管理类图.png) | [矢量图](uml/连接管理类图.svg) |
| TcpServer | [源码](uml/TcpServer.mmd) | [服务入口类图](uml/服务入口类图.png) | [矢量图](uml/服务入口类图.svg) |

- [HttpClient 源码](uml/HttpClient.mmd) · [SVG](uml/网络请求类图.svg)

![HttpClient](uml/网络请求类图.png)

- [LoginModel 源码](uml/LoginModel.mmd) · [SVG](uml/登录模型类图.svg)

![LoginModel](uml/登录模型类图.png)

- [MainWindow 源码](uml/MainWindow.mmd) · [SVG](uml/主窗口类图.svg)

![MainWindow](uml/主窗口类图.png)

- [LoginController 源码](uml/LoginController.mmd) · [SVG](uml/登录控制器类图.svg)

![LoginController](uml/登录控制器类图.png)

- [LocalAuthServer 类图源码](uml/LocalAuthServer.mmd) · [SVG](uml/本地认证服务类图.svg)

![LocalAuthServer](uml/本地认证服务类图.png)

## 时序图

双端工作台新增流程：

| 流程 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| 信令连接、认证与重连 | [源码](uml/client-session.mmd) | [客户端信令连接时序图](uml/客户端信令连接时序图.png) | [矢量图](uml/客户端信令连接时序图.svg) |
| 帧收发与错误收敛 | [源码](uml/client-frames.mmd) | [客户端信令收发时序图](uml/客户端信令收发时序图.png) | [矢量图](uml/客户端信令收发时序图.svg) |
| 双端房间操作 | [源码](uml/client-room.mmd) | [双端房间操作时序图](uml/双端房间操作时序图.png) | [矢量图](uml/双端房间操作时序图.svg) |
| 直播 / 点播切换 | [源码](uml/client-mode.mmd) | [播放模式切换时序图](uml/播放模式切换时序图.png) | [矢量图](uml/播放模式切换时序图.svg) |
| 登录到期 | [源码](uml/client-expiry.mmd) | [客户端会话到期时序图](uml/客户端会话到期时序图.png) | [矢量图](uml/客户端会话到期时序图.svg) |

公共网络库时序图：

| 流程 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| 接入 | [源码](uml/tcp-accept.mmd) | [连接接入时序图](uml/连接接入时序图.png) | [矢量图](uml/连接接入时序图.svg) |
| 收发 | [源码](uml/tcp-io.mmd) | [数据收发时序图](uml/数据收发时序图.png) | [矢量图](uml/数据收发时序图.svg) |
| 关闭 | [源码](uml/tcp-close.mmd) | [连接关闭时序图](uml/连接关闭时序图.png) | [矢量图](uml/连接关闭时序图.svg) |
| 任务 | [源码](uml/reactor-task.mmd) | [跨线程任务时序图](uml/跨线程任务时序图.png) | [矢量图](uml/跨线程任务时序图.svg) |

- [容器开发连接源码](uml/docker-dev.mmd) · [SVG](uml/容器开发连接时序图.svg)

![容器开发连接](uml/容器开发连接时序图.png)

- [本地开发认证源码](uml/local-auth.mmd) · [SVG](uml/本地认证时序图.svg)

![本地开发认证](uml/本地认证时序图.png)

- [HTTP 请求源码](uml/http-request.mmd) · [SVG](uml/网络请求时序图.svg)

![HTTP 请求](uml/网络请求时序图.png)

- [登录源码](uml/login.mmd) · [SVG](uml/登录时序图.svg)

![登录](uml/登录时序图.png)

- [取消登录源码](uml/cancel-login.mmd) · [SVG](uml/取消登录时序图.svg)

![取消登录](uml/取消登录时序图.png)

- [本地退出源码](uml/logout.mmd) · [SVG](uml/退出登录时序图.svg)

![本地退出](uml/退出登录时序图.png)

## 更新图像

安装了 Pillow 的 Python 环境中执行 `python scripts/render_uml.py`。
脚本默认只生成时序图；历史类图不重新绘制。
工程脚本支持本目录当前使用的 Mermaid 子集。修改源码后重新生成，并检查图像和代码是否一致。

## 实时 GPU 人脸检测

- [流程源码](uml/gpu-face.mmd) · [GPU人脸检测时序图](uml/GPU人脸检测时序图.png) · [SVG](uml/GPU人脸检测时序图.svg)

## 编码前音视频同步

- [流程源码](uml/media-sync.mmd) · [音视频同步时序图](uml/音视频同步时序图.png) · [SVG](uml/音视频同步时序图.svg)
- 采集、检测、混音和停止时序图已更新同步入口与尾部交付；本步不新增类图。
- 采集、GPU 预览与停止时序同步更新；本步不绘制新类图。实现与验收见 [第 008 步](008-gpu-face.md)。

## 独立模型优化工程

| 对象/流程 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| TrtLogger | [源码](uml/TrtLogger.mmd) | [推理日志类图](uml/推理日志类图.png) | [矢量图](uml/推理日志类图.svg) |
| EngineSession | [源码](uml/EngineSession.mmd) | [推理引擎会话类图](uml/推理引擎会话类图.png) | [矢量图](uml/推理引擎会话类图.svg) |
| ModelOptimizer | [源码](uml/ModelOptimizer.mmd) | [模型优化器类图](uml/模型优化器类图.png) | [矢量图](uml/模型优化器类图.svg) |
| model-optimize | [源码](uml/model-optimize.mmd) | [独立模型优化时序图](uml/独立模型优化时序图.png) | [矢量图](uml/独立模型优化时序图.svg) |
| engine-infer | [源码](uml/engine-infer.mmd) | [推理引擎执行时序图](uml/推理引擎执行时序图.png) | [矢量图](uml/推理引擎执行时序图.svg) |

## 独立服务端类图

| 对象/流程 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| TimerQueue | [源码](uml/TimerQueue.mmd) | [定时器队列类图](uml/定时器队列类图.png) | [矢量图](uml/定时器队列类图.svg) |
| HttpRequest | [源码](uml/HttpRequest.mmd) | [请求数据类图](uml/请求数据类图.png) | [矢量图](uml/请求数据类图.svg) |
| HttpParser | [源码](uml/HttpParser.mmd) | [请求解析器类图](uml/请求解析器类图.png) | [矢量图](uml/请求解析器类图.svg) |
| HttpServer | [源码](uml/HttpServer.mmd) | [公共请求服务类图](uml/公共请求服务类图.png) | [矢量图](uml/公共请求服务类图.svg) |
| HttpProbe | [源码](uml/HttpProbe.mmd) | [健康探测类图](uml/健康探测类图.png) | [矢量图](uml/健康探测类图.svg) |
| FrameCodec | [源码](uml/FrameCodec.mmd) | [信令帧编解码类图](uml/信令帧编解码类图.png) | [矢量图](uml/信令帧编解码类图.svg) |
| AuthService | [源码](uml/AuthService.mmd) | [认证签名类图](uml/认证签名类图.png) | [矢量图](uml/认证签名类图.svg) |
| LoginServer | [源码](uml/LoginServer.mmd) | [登录服务器类图](uml/登录服务器类图.png) | [矢量图](uml/登录服务器类图.svg) |
| LoginNode | [源码](uml/LoginNode.mmd) | [登录节点类图](uml/登录节点类图.png) | [矢量图](uml/登录节点类图.svg) |
| SchedulerServer | [源码](uml/SchedulerServer.mmd) | [负载调度服务器类图](uml/负载调度服务器类图.png) | [矢量图](uml/负载调度服务器类图.svg) |
| SignalSession | [源码](uml/SignalSession.mmd) | [信令会话类图](uml/信令会话类图.png) | [矢量图](uml/信令会话类图.svg) |
| LiveRoom | [源码](uml/LiveRoom.mmd) | [直播房间类图](uml/直播房间类图.png) | [矢量图](uml/直播房间类图.svg) |
| SignalServer | [源码](uml/SignalServer.mmd) | [信令服务器类图](uml/信令服务器类图.png) | [矢量图](uml/信令服务器类图.svg) |

## 独立服务端时序图

| 对象/流程 | Mermaid | PNG | SVG |
| --- | --- | --- | --- |
| timer-schedule | [源码](uml/timer-schedule.mmd) | [定时器调度时序图](uml/定时器调度时序图.png) | [矢量图](uml/定时器调度时序图.svg) |
| timer-cancel | [源码](uml/timer-cancel.mmd) | [定时器取消时序图](uml/定时器取消时序图.png) | [矢量图](uml/定时器取消时序图.svg) |
| http-server | [源码](uml/http-server.mmd) | [服务端请求处理时序图](uml/服务端请求处理时序图.png) | [矢量图](uml/服务端请求处理时序图.svg) |
| login-service | [源码](uml/login-service.mmd) | [独立登录时序图](uml/独立登录时序图.png) | [矢量图](uml/独立登录时序图.svg) |
| login-schedule | [源码](uml/login-schedule.mmd) | [登录节点调度时序图](uml/登录节点调度时序图.png) | [矢量图](uml/登录节点调度时序图.svg) |
| signal-auth | [源码](uml/signal-auth.mmd) | [信令认证时序图](uml/信令认证时序图.png) | [矢量图](uml/信令认证时序图.svg) |
| signal-room | [源码](uml/signal-room.mmd) | [信令房间管理时序图](uml/信令房间管理时序图.png) | [矢量图](uml/信令房间管理时序图.svg) |
| signal-live | [源码](uml/signal-live.mmd) | [信令开播停播时序图](uml/信令开播停播时序图.png) | [矢量图](uml/信令开播停播时序图.svg) |
| signal-cleanup | [源码](uml/signal-cleanup.mmd) | [信令心跳清理时序图](uml/信令心跳清理时序图.png) | [矢量图](uml/信令心跳清理时序图.svg) |
| server-stop | [源码](uml/server-stop.mmd) | [服务器启停时序图](uml/服务器启停时序图.png) | [矢量图](uml/服务器启停时序图.svg) |
