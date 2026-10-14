# 第013步：Player硬件解码与GPU颜色转换

## 目标与边界

Player默认优先D3D11VA硬件解码，把NV12/P010硬件帧在GPU转换成独立BGRA纹理，再复用现有非模态窗口、WASAPI音频主时钟和启停流程。编码仍使用已实现的NVENC。

本步属于Windows OBS。用户的Mac SRS、MediaMTX、OpenH264研究项目独立开展，不安排与OBS联调；Windows现有媒体服务仍可作为本机RTMP运行基线。

## 对象、线程与资源

- GUI的`SessionController`持有一个`MediaPlayer`解码/调度线程；工作台没有增加新的MVC层或播放线程。
- `MediaPlayer`持有`WasapiPlayback`音频输出线程。GUI只`begin/stop/takeVideo/stats`，正常停止等待`finished`事件，不调用阻塞`wait`。
- `MediaPlayer::run`局部持有普通`MediaDecoder`。其`Resources`独占AVFormatContext、视频/音频AVCodecContext、AVHWDevice设备引用、SWR/SWS以及普通`GpuVideoConverter`。
- `GpuVideoConverter`没有线程/窗口。它持有同一D3D11设备、VideoDevice、VideoContext1、VideoProcessor和GPU完成查询，方法只在播放工作线程调用。
- FFmpeg的D3D11解码池通常是一张数组纹理，`AVFrame.data[0]`是纹理，`data[1]`是数组层。转换期间借用AVFrame，输出每帧新建BGRA快照；解码池归还/尺寸变化后，GUI保留的画面不受影响。

## Controller → begin → start → run

1. 界面发点播/直播意图，Controller沿用登录、房间、模式、上轮finished的检查，调用`MediaPlayer.begin`。
2. `begin`保存本轮`VideoDecodeMode`，清队列与统计，再`QThread.start`。默认`Automatic`；`Software`用于对照，`RequireHardware`用于验收硬解和GPU转换，后两种是C++接口/测试参数，没有增加界面控件。
3. `run`建立普通Decoder，安装停止回调和视频/音频sink。Decoder打开媒体、探测流并记录共同原点，与旧点播/RTMP输入一致。
4. 视频优先选择NVIDIA适配器，再尝试默认硬件设备；软件上传也可用WARP。查询FFmpeg的D3D11VA配置，建立AVHWDevice并AddRef同一设备，创建GPU转换对象。初始化不成功时自动模式保留软件路径，严格模式报告错误。
5. 视频AVCodecContext设置`opaque/get_format/hw_device_ctx`。`get_format`在首包或流格式变化时由FFmpeg调用，选择实际提供的D3D11格式，否则自动模式选择软件格式。它可能在FFmpeg内部线程调用，提示文本用独立互斥锁保护，回调不向C栈抛异常。
6. Player建立WASAPI输出；有视频时先等首张已转换画面，视频点播预缓冲200ms以保留端点预填后的余量，直播/纯音频维持100ms。视频队列先满时`forceStart`建立时钟；首张视频迟到且PCM队列先满时，声音也会放开等待以继续读包，避免互相等待。短媒体EOF不无限等待预缓冲。

## 读包 → 硬解 → GPU转换 → 显示

1. `readNext`按stream_index调用`send_packet/receive_frame`，音频仍在CPU解码和重采样，视频保留原媒体PTS与帧时长。
2. D3D11视频帧进入`GpuVideoConverter.convert`，校验设备、NV12/P010格式、数组层和可见尺寸；GPU处理器按尺寸/格式重建。
3. 设置BT601/BT709、有限/全范围、可见区域裁剪及不透明输出，禁用自动画质增强。`VideoProcessorBlt`转换到单层BGRA RenderTarget/ShaderResource纹理，生产路径不执行视频像素Map或CPU下载。
4. EVENT查询用明确的BOOL结果确认GPU完成，允许取消并设2秒期限。输入AVFrame在完成后才释放，避免下一帧解码复用数组层。
5. `pushVideo`先通知声音首帧就绪，再将独立纹理快速入有界队列。GUI每10ms按WASAPI/QPC主时钟取到期画面，现有VideoRenderer消费BGRA纹理。
6. 帧记录实际`VideoDecodePath`。工作台已有状态标签展示GPU、软件和硬件回读计数；“GPU”表示硬解及GPU转换都成功，不能把普通GPU显示当作硬件解码。

点播和直播视频队列都最多16帧；直播超界丢旧帧，点播等待消费。硬解比首屏音频时钟快时，原8帧直播队列会提前挤掉未来画面，现为启动预留更足的有界余量；取帧仍按同一主时钟，不人为等队列填满。直播和点播均能显示独立预览窗口。

## 回退与停止

自动模式有三条明确路径：

| 路径 | 数据处理 | 统计 |
| --- | --- | --- |
| D3D11Gpu | 硬件解码 → GPU颜色转换 → BGRA纹理 | gpuVideo |
| SoftwareUpload | 不支持/未启用硬解 → 软件解码/SWS → GPU上传 | softwareVideo |
| D3D11Readback | 硬解可用，当前GPU转换不支持 → 明确回读/SWS/上传 | readbackVideo及原因 |

转换失败在本轮锁存，避免逐帧重复初始化。GPU超时、设备移除、用户停止和严格硬件模式不会继续尝试阻塞回读。HDR/PQ/HLG和BT2020未接GPU色彩管理；自动模式可显示回退原因，当前不做HDR色调映射。

正常EOF送空包取完B帧/SWR尾部，音频端点排空，保持最后画面持续时间。用户stop立即停止GUI交付，关队列/中断网络/通知音频退出。Decoder先释放codec和解码池，再归还GPU转换与设备；已交付独立纹理由ComPtr继续持有。Player等声音线程归还后发结果，`run`返回产生`finished`，Controller才允许重启。

## 验证

```powershell
./scripts/build_clients.ps1 -Deploy
python scripts/run_decode_checks.py
python scripts/run_checks.py
```

`OBSHardwareDecodeTests`要求真实硬件环境，独立于默认CTest。全部素材由FFmpeg生成，声音输出音量0，截图用中文文件名保存在`out/GPU解码验证/`；不打开用户摄像头、麦克风或屏幕。

覆盖NV12/P010转换、数组层、对齐裁剪、尺寸重建、独立输出、取消；H.264 B帧EOF、四组601/709与有限/全范围、软硬解PTS对照、解码池销毁后保留纹理、流中尺寸变化、High10软件回退、转换回读计数、真实Player音频主时钟/重复播放/停止和界面GPU状态。色块只比较RGB三通道都稳定的区域，边缘色度上采样不同不要求逐像素完全一致。

本机2026-10-14，RTX 5060 / 驱动616.92 / FFmpeg7.1 / Qt6.11.2：构建和8组CTest通过。硬解专项通过；软硬解帧数/PTS一致，稳定色块最大RGB误差3级，平均误差0.155～0.568级；两轮1.6秒媒体各48张GPU帧、76800有效音频样本，无跳帧/补静音，主动停止28ms。NV12/P010合成转换、分辨率变化、High10软件回退及BT2020转换回读均按预期通过。

真实静音WASAPI回归通过音轨延后、视频延后1.2秒、较长视频尾部；视频延后的用例在修复前能复现队列等待卡住，修复后结束位置2.240秒。Windows既有RTMP回归两轮各132张GPU帧、121张交付画面、无视频丢帧，正常停止117～129ms，握手等待取消93～95ms；实际Publisher测试窗口到Player预览及房间关闭通过。直播短测试各有1次启动补静音，未承诺零卡顿或端到端延迟；测试结束恢复业务服务及OBS-SRS的原停止状态。

调试时可在程序子进程环境设置`OBS_DECODE_TRACE=1`，记录每30张视频帧的媒体PTS/格式以及音频补静音位置，不打印媒体地址或认证数据。P010合成纹理转换不等同于HEVC Main10或HDR电影验收；跨显卡、驱动变化、长时间负载和CPU/功耗收益需要后续单独测量。

时序图：[硬件解码转换](uml/gpu-decode.mmd)、[点播打开](uml/vod-open.mmd)、[音画调度](uml/vod-play.mmd)、[启停](uml/vod-stop.mmd)。参考：[FFmpeg D3D11帧契约](https://github.com/FFmpeg/FFmpeg/blob/n7.1/libavutil/hwcontext_d3d11va.h)、[D3D11 VideoProcessorBlt](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videocontext-videoprocessorblt)。
