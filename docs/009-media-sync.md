# 第 009 步：编码前音视频同步

本步在现有工程增加一个 `MediaTimeline` 类，复用 `SessionController`，不增加采集 MVC 或 Demo。音频仍由 `AudioMixer` 完成麦克风/系统声音对齐与混合；本层让混音 PCM 和视频结果进入同一输出时间线。

## 当前接法

```text
VideoCapture → 可选 GpuFaceDetector → 同帧纹理/框点 → MediaTimeline
WasapiCapture → AudioResampler → AudioMixer → 48kHz PCM → MediaTimeline
MediaTimeline → synchronizedVideoReady / synchronizedAudioReady → MediaPublisher 有界编码队列
              └ 视频 → 现有独立 GPU 预览窗口
```

`videoFrameReady`、`audioPacketReady`、`mixedAudioReady` 保留为原始/处理数据诊断出口。后续编码使用两个 **synchronized** 信号，避免绕过媒体时间线。信号当前仍在 GUI 线程发出，消费者只做有界入队，编码、封装和网络写入在工作线程执行。

同步层不修改纹理像素、不读回 CPU。`TimedVideoFrame.source` 保存原始纹理和同帧检测框点；预览Shader和第010步的GpuVideoOutput分别绘制到预览/编码输出，编码使用独立GPU硬件帧。人脸耗时状态中的帧龄表示检测结果交付时刻，不包含新增的同步等待或最终显示器扫描。

## 时间和调度

1. `startCapture` 在启动采集线程前用 QPC 记录本轮共同起点 `T0`；音频/视频哪一路先到，都不改变它。原始 `timestamp100ns` 保留，PTS 是另一字段。
2. 视频时间基为 `1/30` 秒。按 QPC 计算到期时间格，选择该时间格之前最新完成的纹理及同帧框点。静止 WGC 无新帧时复用当前图像，PTS 继续推进。在等待预算内到达的新帧，内容量化到输出格子的偏移最多约一个视频帧周期；重复帧的来源时间不会改成新时间。首次 GPU 初始化或后续处理超过预算时可能延后首画面、丢帧或暂时复用旧画面，120ms 并非任意处理负载下的同步保证。
3. 音频时间基为 `1/48000` 秒，`pts = round((timestamp100ns - T0) × 48000 / 10000000)`。每包 480 个采样帧，即 10ms。视频第 3 格与音频第 4800 个采样帧都表示本轮 100ms。两路独立交付，不按一帧对应一个音频包配对。
4. 初始总等待 **120ms**，包含混音已有的 80ms，不额外叠加为 200ms。音频整包到期才输出；视频也使用相同截止线。可在 `MediaTimeline::Delay100ns` 调整预算。实际延迟还包含设备、GUI 调度、GPU、后续编码/网络/播放。
5. 视频待处理最多 16 张纹理，音频最多 50 包（500ms）；普通调度最多追赶约 200ms。积压时跳过过时数据、保留原时间间隔并设置 discontinuity，避免迟到数据让延迟无限增加。
6. 重复/倒序/原点之前的数据被拒绝；已经交付视频时间格之前的迟到检测帧不覆盖新输出。音频 PTS 缺口设置 discontinuity，估计时间标志保留。视频失败后撤掉画面路；所有音频源失效后撤掉声音路；另一成功路继续。

调度统计中的视频丢弃数包含输入迟到/溢出与跳过的输出格子，不能直接当作采集源帧数；复用数可以帮助观察静止画面或处理跟不上的情况。初始化较慢时可能先有一条路输出，尚无首帧的路保留实际起始偏移。

## 启停

正常停止暂停普通交付并清空预览。两路 WASAPI 退出后先排空重采样/混音尾部，再等采集与检测全部 finished，最后排空同步层已经接受的有界媒体数据。尾部同步信号交付给后续编码器，预览不会重新弹出；最后视频时间格可能比音频尾部多不到一个视频帧周期。重复 finish 不重复输出。

退出登录立即 `clear`，丢弃尾部和 GPU 引用，迟到数据不发信号。重新开始建立新原点、清统计。第010步新增编码器flush和MP4封存：已进入编码队列的数据正常收尾，采集/检测/录制全部finished后才允许重启；程序退出封存已入队媒体，GUI未交付尾部不保证补送。

## 验证和调试

```powershell
./scripts/build_clients.ps1 -Deploy
python scripts/run_checks.py
python scripts/run_capture_checks.py --media-sync
python scripts/run_capture_checks.py --window-preview
./scripts/run_face_checks.ps1 -Gpu
```

`client.media_sync` 使用人工时钟、PCM 和 WARP 小纹理，不使用采集设备。模拟 10 分钟，每 100ms 同时产生一次画面变化与声音脉冲，GPU 结果晚到 50ms，调用方每 30ms 取批次，检查 6000 次事件的 PTS 对齐及连续输出。它验证时间计算，**不代表真实设备长期时钟漂移或实际播放唇音同步已验收**。其它检查覆盖 120ms 总等待、不同时间基、静止重复、估计时间、迟到/重叠拒绝、队列溢出、200ms 追赶、停止尾部、单路失败、退出丢弃与重启。

`--media-sync` 只捕获测试自己创建的变化色块窗口，短暂播放人工静音驱动默认输出的 WASAPI 回环；检查真实控制器的两路同步输出、单调 PTS、共同起点、正常停止、重启与退出丢弃。不打开摄像头/麦克风/整个屏幕，不保存采集音视频；没有默认输出端点时报告 SKIP。

在主窗口“音视频采集”区查看新增的两行同步状态；小窗口向下滚动。推荐断点：`startCapture` → `MediaTimeline::begin` → `pushVideo/pushAudio` → `takeFrames/takeUntil` → `deliverMedia` → `finishAudio/finishMedia`。观察 `m_origin`、源时间、两路 PTS、`m_nextVideo`、两个队列和 stats。调试停顿会主动触发积压丢弃，算法测试用人工时钟更容易复现。

本机验证记录（2026-10-10，Qt 6.11.2 / MSVC、FFmpeg 7.1、RTX 5060）：

- Debug 构建完成；六项 CTest 全部通过，总耗时 10.48s；其中时间线检查 0.16s。
- 人工 10 分钟测试的 6000 次闪屏/声音脉冲事件，最大 PTS 偏差为 0ms；该数字仅描述上述人工输入与时钟。
- 真实 WGC 测试窗口 + WASAPI 回环联合检查通过：交付 39 个同步音频包、12 帧同步视频；正常停止、重启、退出后无迟到交付。输入使用人工静音，不作听感验收。
- WGC 窗口预览专项通过；TensorRT GPU 检测/预览及控制器失败回退、重启、退出检查通过。本轮构建关闭 OpenCV 参考测试。
- 普通缩放与 200% 缩放的紧凑窗口、状态换行、独立预览检查通过。图像取证只使用测试窗口和人工界面状态。

硬件长时间漂移补偿、编码/推流输出和播放端音频时钟仍待后续对应步骤验证。

## 后续 FFmpeg 接入设计

继续采用 C++ 调用现有 FFmpeg 7.1 库，新增职责清晰的少量类：

| 模块 | 职责 |
| --- | --- |
| MediaEncoder | 持有音/视频编码上下文；同步帧转 AVFrame，输出 AVPacket，正常停止 flush |
| MediaPublisher | 推流工作线程，拥有编码器和输出 AVFormatContext；MP4本地验证后改FLV/RTMP，处理超时/中断及有界输入 |
| MediaPlayer | 文件或URL输入、解封装、解码；复用同一播放实现支持直播和点播 |
| AudioOutput | WASAPI 扬声器播放；播放端以实际音频播放时钟调度视频，无音频时使用单调时钟 |

第010步已实现并验证本机 `h264_nvenc` D3D11互操作与AAC：GPU输出独立BGRA硬件AVFrame，音频10ms包通过FIFO按编码器frame_size聚合，保留时间缺口与实际停止尾部。不能假定每个输入必定产生一个编码包，详见 [当前编码实现](010-ffmpeg-encode.md)。

RTMP 协议读写交给 libavformat，现有 TCP 网络库继续负责信令。媒体服务另行部署，建议 SRS 独立 Docker 服务；已有信令返回 pushUrl/pullUrl，但当前 live.start 提前设置 streaming，接入媒体时应改为准备地址与实际开播确认两个阶段。部署时核对既有端口占用。播放端优先验证 Windows D3D11VA 硬解，并让渲染器支持解码后的 NV12 纹理；软件解码保留兼容路径。

顺序：GPU处理输出 → 同步帧编码并保存本地MP4 → 本地解码播放与音频时钟 → RTMP双端闭环 → 超时/重连/权限与服务状态联动。WebRTC 的传输另设后续步骤，复用媒体处理与时间约定。

参考：[FFmpeg 编解码 API](https://ffmpeg.org/doxygen/7.1/avcodec_8h_source.html)、[封装 API](https://ffmpeg.org/doxygen/7.1/group__lavf__encoding.html)、[RTMP 协议](https://ffmpeg.org/ffmpeg-protocols.html#rtmp)、[SRS 部署](https://ossrs.io/lts/en-us/docs/v7/doc/getting-started)。
