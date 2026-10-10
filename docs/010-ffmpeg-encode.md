# 第 010 步：GPU 编码与本地 MP4

本步完成推流端的 **GPU 输出 → H.264 NVENC / AAC → MP4**。既有同步层直接提供输入，Controller 继续协调启停。播放端仍只有直播/点播页面，本步没有实现播放器解码、RTMP 或 WebRTC 传输；测试中的解码只用于检验生成文件。

## 目录和库目标

`OBSClientCore` 原来是 CMake 静态库目标，并不是目录，且混合了界面和媒体类。现在拆为：

| 目标 | 源码位置 | 职责 |
| --- | --- | --- |
| OBSNetwork | `OBS client/` | HTTP、TCP 信令客户端 |
| OBSMediaCore | `OBS client/` | 采集、重采样、混音、MediaTimeline、TensorRT、GPU预览 |
| OBSFFmpegCore | `OBS client/ffmpeg/` | 编码输出纹理、音视频编码、MP4封装与编码线程 |
| OBSClientUi | `OBS client/` | MainWindow、登录/工作台 Model 和 Controller |

原有类名保持其职责：VideoCapture、AudioMixer、MediaTimeline 等。新目录只有 GpuVideoOutput、MediaEncoder、MediaPublisher 三组 h/cpp 和 CMakeLists.txt，没有额外采集 MVC。

## 对象、线程与资源

| 对象 | 谁拥有 | 调用线程 / 持有资源 |
| --- | --- | --- |
| SessionController | 应用主流程 | GUI；持有采集/检测/编码 QThread 对象和串行混音、同步对象 |
| MediaPublisher | SessionController | 对象属于 GUI；`begin/push/stop` 快速入队或改状态，`run` 在独立编码线程执行 |
| MediaEncoder | `MediaPublisher::run` 的局部对象 | 无线程；工作线程独占视频/音频 AVCodecContext、硬件帧池、AAC FIFO |
| GpuVideoOutput | MediaEncoder | 无线程；同采集设备、独立 deferred context、Shader/常量/完成查询，无 SwapChain |
| MP4/AVIO/临时文件 | `MediaPublisher::run` 的局部资源 | 编码线程独占；支持中文目录和文件名 |

## Controller → begin → start → run

1. 界面勾选“采集时保存 MP4”，选择保存目录。默认关闭录制；默认目录是系统视频目录下的 OBS 子目录。
2. 点击“开始采集”。Controller 校验登录/枚举/上轮 finished，启动混音与同步原点，根据实际选择决定音频/视频轨。
3. Controller 生成新的 `录制_日期时间_随机后缀.mp4` 路径，调用 `MediaPublisher::begin`。它清队列/统计、开放入口并调用 QThread::start，不在 GUI 打开编码器、读取引擎或写文件。
4. 随后 Controller 启动实际采集。编码线程 `run` 等待第一张同步视频纹理取得设备，最多5秒；仅音频录制可直接初始化 AAC。GUI 在等待期间仍能停止。
5. `run` 创建同设备编码器与 MP4 流，写 header 后发 opened；Controller 在 GUI 显示录制状态。

## 运行时的数据交接

```text
WinRT/WGC → 独立BGRA GPU快照 → 可选TensorRT同帧框点 ─┐
WASAPI → 重采样 → 双路混音48kHz Float32 ───────────┤
                                                  ↓
                                    MediaTimeline，共同QPC原点
                                      ↓                 ↓
                                视频PTS 1/30       音频PTS 1/48000
                                      ↓                 ↓
                              MediaPublisher有界输入队列
                                      ↓                 ↓
                        GpuVideoOutput / D3D11 AVFrame   AAC FIFO
                                      ↓                 ↓
                               h264_nvenc              AAC
                                      └── AVPacket ─────┘
                                               ↓
                                  时间基换算 → MP4写包
```

Controller 的两个 synchronized 信号连接到 Publisher 的快速入队接口，在 GUI 中只复制 GPU 引用/PCM。输入最多 **16 视频帧、50 个10ms音频包**；溢出丢最旧项并计数，stop 后拒绝迟到输入。编码线程取走队列头后立即解锁，再进行 GPU、FFmpeg 或文件操作。两路都有数据时按换算后的 PTS 选较早项；没有输入则条件变量等待。

### 视频

GpuVideoOutput 将原纹理等比绘制到编码器分配的独立 BGRA D3D11 AVFrame；固定 **1280×720 / 30fps / 4Mbps**，源尺寸变化补黑，不改变编码分辨率。Shader 同时绘制同帧框点，因此它们真实进入文件。视频像素不读回 CPU；NVENC 内部将 RGB 转成 YUV420，生成 H.264，无B帧、无lookahead，p4/ll、delay=0。

独立 deferred context 记录绘制，共享 immediate context 开启多线程保护并恢复调用前状态。GPU完成查询在编码线程等待，最多2秒，防止绘制尚未完成就交给 NVENC。采集快照只读，硬件 AVFrame 的引用控制输出纹理寿命；运行中换 D3D11 设备明确失败，可停止后重开。

FFmpeg 7.1 的 NVENC 在本机没有回传 AVFrame.duration。固定30fps时为缺失的视频 packet.duration 补1个编码时间单位，否则 MP4 视频轨会少最后一帧的时长。跨帧PTS间隔仍原样保留，不压缩成连续帧。

RGB转换矩阵按此版 NVENC 的 BT.601 路径标记为 bt470bg，原色/传递函数为 bt709；HDR、色彩管理与其他编码器属于后续范围。

### 音频

混音已输出48kHz立体声交错 Float32，不再重采样。`MediaEncoder::pushAudio` 将左右声道转为平面数据，存入 AVAudioFifo，按 AAC frame_size（本机1024）聚合编码，128kbps。480样本的输入包不能直接当作一帧 AAC。

第一包保持原 PTS 偏移；后续缺口按样本数补静音，超过2秒明确失败，重叠/倒序也失败。编码器报告的 delay 和包 PTS 原样交给 MP4；不另加120ms等待，也不手动抹掉负PTS。

停止时 FIFO 不足1024的尾部按实际样本数提交，再 flush。AAC解码仍可能返回补齐的完整帧，MP4轨道时长/编辑列表保存实际尾长；将来播放器需要处理 codec delay、skip-samples 和容器结束边界，不能把解码总样本数直接当作播放时长。

## stop → 清理/finished → 重启

1. 普通停止先停采集/检测。两路音频 finished 后排空重采样/混音；所有采集/检测 finished 后排空 MediaTimeline，最后才调用 Publisher::stop。
2. `stop` 只关闭入口并唤醒编码线程。`run` 处理已入队数据 → AAC实际尾部 → 编码器空帧 flush → receive剩余包 → MP4 trailer → AVIO/file flush。
3. 始终先写同目录 QTemporaryFile；正常完成且存在媒体时，用 QTemporaryFile::rename 原子改名并拒绝覆盖已有文件。失败或 abort 自动删除临时文件，不把半成品当作成功 MP4。
4. 资源在 run 内释放后发 completed/failed，接着 QThread 发 finished。Controller 处理 finished 后才允许下一轮，采集与录制收尾都结束前不能更换设置。
5. 退出登录立即丢弃混音/同步未交付尾部、停止新输入，编码队列中已经接收的数据仍正常封存，迟到采集信号不会录入新数据。
6. 应用关闭时析构停止并 wait 回收线程，封存已进入编码队列的数据；GUI事件循环结束后未交付的同步尾部不保证补送。程序崩溃或断电无法保证完成 MP4 索引。

录制失败只显示独立错误，采集和预览继续；不在本轮反复重试编码器。视频录制需要支持 D3D11 输入的 NVIDIA/NVENC 和驱动，本步没有软件回退。仅音频录制可使用 AAC。

## 运行与验证

Qt Creator 重新配置 `OBS client/CMakeLists.txt` 即可看到新库和 ffmpeg 目录。FFmpeg SDK 仍使用7.1 shared，新增链接 avcodec/avformat；CMake复制 avcodec-61.dll / avformat-61.dll，以及原来的 avutil-59.dll / swresample-5.dll。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_clients.ps1 -Deploy
$env:PATH = "C:/software/Qt/6.11.2/msvc2022_64/bin;C:/software/Qt/Tools/CMake_64/bin;" + $env:PATH
python scripts/run_checks.py
& ".\OBS client\build-agent\OBSEncodingTests.exe" --gpu
python scripts/run_capture_checks.py --record-face
python scripts/run_capture_checks.py --ui-only
```

实际使用：启动 Docker 登录/信令服务 → 打开 OBS_Publisher → 登录 → 选择采集源 → 勾选本地录制/选目录 → 开始 → 停止 → 等“已保存”与按钮恢复。房间信令和本地录制独立，本步无需媒体服务器。OBS_Player 当前不会播放此文件，可用现有系统播放器验证。

本机验证记录（2026-10-10，Qt6.11.2 / MSVC、FFmpeg7.1、RTX5060）：

- 构建通过；默认7组CTest全部通过。新增 client.encode 覆盖AAC FIFO、双声道、100ms缺口补零、实际尾长/单调DTS、中文路径、拒绝覆盖、失败清理、abort/重启、迟到输入与有界队列；WARP验证框点、留黑、尺寸变化、快照只读和上下文恢复。
- 硬件编码：90帧、3秒合成画面，全部H.264软件解码，框点写入文件，源横竖切换正常，音视频结束均为3秒；3次合成脉冲/闪光解码后最大偏差0.167ms，这是合成文件中的时间对齐结果，不是端到端传输延迟。
- 实际 Controller 联调：只采集自建色块窗口，系统回环混音设静音；TensorRT检测与NVENC同时运行，正常停止/重启/退出登录均完成MP4，没有迟到媒体或队列丢弃。一次检测 GPU1.61ms、处理3ms只是样例，不能当作稳定性能基准。
- 原生界面专项在2倍缩放、640×480逻辑窗口通过；新增目录控件与长录制路径可换行/滚动访问。

测试文件和截图在忽略的 out/，不提交采集媒体、SDK或模型二进制。详见 [录制时序图](uml/ffmpeg-record.mmd)、[录制停止时序图](uml/ffmpeg-stop.mmd)。

## 接续顺序与低延迟

后续继续：本地解封装/解码与音频时钟 → 部署媒体服务、RTMP推拉流闭环 → WebRTC/ICE与STUN/TURN部署 → 网络抖动、拥塞和端到端低延迟调优。WebRTC需要协商和实时传输能力，不能把RTMP地址换掉就视为完成；现有信令服务届时扩展SDP/候选交换。

低延迟基础现在已经做：队列有界、保留PTS、工作线程编码/写出、NVENC不使用B帧和lookahead、封装交织缓存限制100ms。同步层当前120ms预算、GUI33ms交付、逐帧GPU完成等待都需要后续实测调节，当前不宣称端到端达到某个延迟。

参考：[FFmpeg7.1编码API](https://ffmpeg.org/doxygen/7.1/avcodec_8h_source.html)、[MP4封装API](https://ffmpeg.org/doxygen/7.1/group__lavf__encoding.html)、[D3D11硬件上下文](https://ffmpeg.org/doxygen/7.1/hwcontext__d3d11va_8h_source.html)、[Qt临时文件原子改名](https://doc.qt.io/qt-6/qtemporaryfile.html#rename)、[ICE规范](https://www.rfc-editor.org/rfc/rfc8445)、[WebRTC原生API](https://webrtc.github.io/webrtc-org/native-code/native-apis/)。
