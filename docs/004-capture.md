# 第 004 步：推流端音视频采集

本步沿用 CourseStudio 的 Windows 技术：摄像头用 WinRT MediaCapture / MediaFrameReader，窗口和显示器用 WGC，麦克风与系统声音用 WASAPI。不使用 Qt Multimedia 替换采集实现。

## 保持封装简单

采集模块只有两个工作类，不单独建立采集 Model、Controller，也没有设备继承树。

| 文件 | 职责 |
| --- | --- |
| CaptureSource.h | 一个设备/窗口/显示器选择项：类型、ID、名称、句柄 |
| VideoCapture.h/.cpp | 摄像头、窗口、屏幕采集；单帧邮箱；GPU 纹理及预览 |
| WasapiCapture.h/.cpp | 麦克风和回环共用的实现；PCM 包、音量、有界邮箱 |
| SessionController.h/.cpp | 复用工作台控制器，枚举设备、协调三路启停、交付媒体数据 |
| mainwindow.h/.cpp | 选择设备、开始/停止、预览、音量和状态说明 |

界面继续使用现有 MVC。SessionModel 保存信令和房间业务状态；采集线程状态由工作台控制器直接协调，不额外增加一套 MVC。以上源码仍平铺在 OBS client。

## 使用

1. Qt Creator 打开 OBS client/CMakeLists.txt，使用 Windows Qt 6 / MSVC x64 和 Windows SDK。工程升级为 C++20，与原工程和当前 C++/WinRT 工具链保持一致。
2. 构建并运行 OBS_Publisher，启动 Docker 登录服务，使用 root / root 登录。
3. 视频源可选摄像头、窗口、屏幕；麦克风和系统声音分别选择。初始全部为“不采集”，不会自动打开设备。
4. 点击“开始采集”。各路独立启动，一路失败会展示原因，其他成功的路继续工作。允许仅采音频或仅采视频。
5. 视频显示预览，音频分别显示音量。当前不播放麦克风或回环声音。
6. 点击“停止采集”，待停止完成后切换源或刷新设备。退出登录、登录到期、关闭程序也会停止采集。

创建房间与本地采集相互独立。信令未连通时仍可在登录有效期内预览；本步不发送 live.start，不做 FFmpeg 编码，也不连接 RTMP/WebRTC 媒体服务。

WGC 桌面互操作要求 Windows 10 1903 或更高版本。窗口关闭、最小化后无法开始采集时重新选择目标；摄像头/麦克风拒绝访问时检查 Windows 隐私设置及设备占用。插拔设备后手动刷新，不建立额外设备热插拔模块。

## 数据契约

**视频**：VideoFrame.texture 是独立持有的 D3D11 BGRA8 纹理快照，可通过 GetDevice 获得所属设备。WGC surface 或摄像头 GPU surface 通过 GPU 复制保存，系统复用采集帧池不会修改消费者保留的纹理。摄像头驱动若提供 SoftwareBitmap，则按真实步长上传；不强行承诺所有摄像头零拷贝。

VideoFrame.timestamp100ns 使用系统相对 QPC 时间域，sequence 区分新帧。邮箱只保留最新一帧。现阶段 CPU 预览最多 10fps、尺寸不超过 960×540；GPU 帧约每 33ms 交付一次，但静止窗口可能没有新帧。当前 QLabel 预览有 GPU→CPU 读回，尚不是 GPU 渲染/美颜链路。

preview 是缓存的最近一次读回图像，previewTimestamp100ns 单独记录其采样时刻，可能早于当前 GPU 帧。编码及后续 GPU 处理使用 texture 与 timestamp100ns，不使用低帧率预览代替原始帧。

**音频**：AudioPacket.pcm 是交错 Float32 PCM，保留端点的采样率和声道数；不在采集层偷偷重采样或混音。支持 PCM 8/16/24/32 位及 IEEE Float 32/64 位，EXTENSIBLE 根据 SubFormat 区分 PCM 与 Float。静音包转换为零值，异常浮点值归零。

AudioPacket.timestamp100ns 是首个采样帧的 QPC 时间戳；设备报告时间戳错误时使用近似时间并设置 timestampEstimated。discontinuity 表示设备不连续或应用丢包。每路最多保存 50 包，超过上限丢弃较早数据并标记不连续；500ms 无数据时音量归零。

SessionController 的 videoFrameReady、audioPacketReady 为后续媒体处理入口，当前在 GUI 线程交付，消费者必须快速转交到有界处理队列。后续在采集与编码之间接 GPU 美颜，并让预览使用处理后的帧；音频后续接重采样、混音、编码和同步。

## 生命周期与线程

- 设备枚举使用独立 MTA 线程。视频一路、麦克风一路、系统回环一路，各自拥有采集线程与 COM 资源。
- WinRT 初始化等待、WGC 帧读取、WASAPI GetBuffer/ReleaseBuffer 均在工作线程执行。GUI 只发开始/停止意图并每 33ms 消费邮箱。
- 帧通知只捕获共享通知对象，不捕获采集实例的裸 this。退出前撤销事件、关闭会话/帧池/Reader，释放 COM 资源后反初始化 apartment。
- 没有 SwapChain Present 的采集链路显式 Flush 提交纹理复制命令，避免命令迟迟不提交。
- 正常停止仅 requestInterruption；直到 GUI 收到 finished 才允许重启，避免迟到回调覆盖新状态。析构必须等待线程退出，不能销毁仍在使用的对象。
- WGC 按 ContentSize 处理尺寸变化，归还旧帧后重建帧池。Closed/Failed 事件以及 WASAPI 设备错误均返回界面。

## 验证

常规验证不打开有效采集源：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_clients.ps1 -Deploy
python scripts/run_capture_checks.py
```

完整 CTest 仍使用 scripts/run_checks.py。新增 client.capture 覆盖 PCM/Float/EXTENSIBLE、静音和不连续标志、无效格式、无效窗口重复启停、GUI 枚举响应、退出登录及角色界面。

显式执行硬件验证：

```powershell
python scripts/run_capture_checks.py --hardware
```

此命令显示自建变化色块窗口，验证 WGC 像素、尺寸变化、快照寿命、重复启停、窗口关闭及真实工作台预览；短暂测试可用的屏幕、摄像头、麦克风和回环端点。只有一个输出端点时，用无声音频验证回环 PCM、QPC 及 50 包上限。截图仅保存自建色块窗口的预览到 out/推流端采集页面.png，不保存桌面、摄像头或音频文件。

本机 Qt 6.11.2 / MSVC x64 Debug：两个客户端和三个测试程序构建成功，三个常规 CTest 全部通过；临时启动 Docker 服务后的真实双端登录、认证、房间及退出集成测试也通过，结束后已恢复服务停止状态。

Windows 原生调试器下硬件测试通过：WGC 自建窗口的颜色、缩放、重复启停、窗口关闭、独立 GPU 快照、工作台预览及退出登录；WGC 显示器 GPU 帧；WASAPI 回环真实 Float32 包、QPC 时间戳、有界队列及丢包标记。枚举到 1 个显示器、1 个输出端点、0 个摄像头、0 个麦克风，因此摄像头和麦克风仅完成实现及错误/生命周期框架，尚未硬件验收。

初次测试期间 OBSCaptureTests.exe 出现过内存读取错误。已提前检查失效 HWND，改为共享通知对象回调，并保证正常/异常退出都撤销事件、关闭资源后才退出线程。更新后常规测试及原生调试器下的多轮启停均未复现访问冲突；未取得初次异常的转储，不能据此断言唯一根因。正常 Windows 环境进行真实采集；受限测试沙箱曾返回 WGC 服务不可用，不能把该环境错误当作硬件验收结果。

## API 参考

- [WinRT MediaFrameReader](https://learn.microsoft.com/en-us/windows/apps/develop/camera/process-media-frames-with-mediaframereader)
- [WGC CreateFreeThreaded](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)
- [WGC 桌面窗口互操作及版本要求](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)
- [WASAPI GetBuffer 与 QPC 时间戳](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)

各类图和设备枚举、视频、音频、停止流程的时序图见 [UML 索引](uml.md)。
