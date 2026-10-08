# 第 006 步：音频重采样与双路混音

在现有推流端接通 WASAPI → AudioResampler → AudioMixer → `SessionController::mixedAudioReady`。统一输出为 **48kHz、立体声、交错 Float32、每包 10ms**，供后续音频编码使用。没有新增 Demo、采集 Model 或 Controller；源码继续平铺在 `OBS client/`。

## 封装与调用顺序

| 文件 / 对象 | 职责 |
| --- | --- |
| AudioPacket.h | 独立的音频数据契约；从 WasapiCapture.h 移出，新增扬声器布局 channelMask |
| WasapiCapture | 在自己的工作线程采集，转换成 Float32，保留设备采样率、声道及 QPC 时间 |
| AudioResampler | 每路独占一个 FFmpeg SwrContext；采样率转换、声道转换、滤波尾部排空 |
| AudioMixTrack | 一路重采样器、时间戳队列、音量和静音状态；直接定义在 AudioMixer.h |
| AudioMixer | 两路按 QPC 对齐、切固定包、缺包补零、增益、静音、相加与削波 |
| SessionController | 复用既有控制器，串行消费两路邮箱并交付混音结果，协调停止和退出 |
| MainWindow | 每路音量 0～200%、静音，以及混音输出电平、包数和削波统计 |

1. `setupCapture` 绑定界面控制及采集回调，33ms GUI 定时器仍沿用原设计。
2. 点击“开始采集”后，`startCapture` 根据选择调用 `AudioMixer::begin`，重置本轮缓存、时钟及统计，再启动工作线程。音量与静音设置保留。
3. WASAPI 发布原始包，`consumeAudio` 在 GUI 线程取出；保留 `audioPacketReady` 诊断信号，再调用 `AudioMixer::push`。
4. 每路 `AudioResampler::convert` 校验格式，首包创建 swr；转换后按实际输出样本数生成时间戳，再放入各自队列。
5. `takeFrames(audioNow())` 为另一设备和滤波留出 80ms 对齐窗口；在每次 GUI 回调中交付多个固定 10ms 包。包长及时间轴不取决于 Qt 定时器触发次数。
6. `mixFrame` 计算两路数据与本包的时间交集，补零、应用音量/静音、相加，最后限制到 [-1,1]，更新 RMS 和削波统计。
7. 控制器发送 `mixedAudioReady` 并更新界面。下游消费者应快速交给有界队列，避免在此信号中执行阻塞编码或文件写入。

类成员、成员函数有中文注释；cpp 的转换、混合和清理过程有编号注释。对应 [类图和时序图索引](uml.md) 已同步。

## 数据与时间约定

- 原始输入为交错 Float32，采样率 8～192kHz、1～32 声道，单包不超过 500ms。单声道复制到左右声道，保持原始电平；多声道按 FFmpeg 默认矩阵下混。多声道必须提供明确的扬声器掩码，声道数与掩码不一致时拒绝处理，不猜测 5.1/7.1 排列。
- 输出 `sampleRate=48000`、`channels=2`、`channelMask=3`；每包 480 个采样帧、960 个 Float32 值、3840 字节。timestamp100ns 指向包内首个采样帧，使用同一 QPC 时间域。
- 每路用累计输入/输出帧数计算时间，避免逐包舍入误差。5ms 内时间抖动吸收到连续样本时钟；明确不连续、格式变化或超过 5ms 的向前跳变会重建滤波状态。重复/倒序包不会再次进入滤波器；声明不连续的时间回退会作为新段处理。
- 首个有效转换包建立混音原点；之后才到达、且位于已输出区间的数据不能重放。未选择的输入不算缺包；已启用、未静音且增益大于零的一路缺失时补零并设置 discontinuity。
- 向前跳变保留此前队列中的有效音频；缺失区间补零。倒退造成重叠时撤掉该路旧队列。timestampEstimated 会传播，混音原点本身是估计值时，本轮输出一直保留此标志。
- 每路最多缓存约 500ms，每次正常交付最多追赶 200ms；更长 GUI 停顿跳过旧时间段并标记不连续，避免无限积压。80ms 是对齐窗口，实际端到端延迟还包含设备缓冲、GUI 调度及后续处理。
- 静音仍消费数据，恢复不会重放旧声音。两路各 100% 会直接相加，可能超过满幅；当前采用硬削波并计数。界面“削波次数”统计的是越界采样值数量，左右声道分别计数，不是削波事件段数。

一路重采样失败时只禁用该混音输入，并在界面保留错误，另一成功路继续输出。采集设备失败时同样移除此路；所有输入都失效时停止混音输出。

## 正常停止与退出

正常“停止采集”先异步请求工作线程结束，收到 finished 后取走最后一批采集包；两个音频线程均退出时调用 `finish`，排空 swr 尾部，最后不足 10ms 补零并标记不连续。排空最多保留末尾 500ms；完成后允许新一轮启动。无需等待视频线程才能排空音频。

退出登录或登录失效立即 `clear`，不发送尾部，迟到包直接丢弃。析构仍等待原采集线程退出，重采样器析构释放 swr。重复 finish 不会重复交付，重启不会复用旧音频。当前关闭程序只保证线程和资源回收；后续有编码器/文件输出时需要增加完整的应用退出排空顺序。

## 构建与运行

沿用 CourseStudio 的 FFmpeg **7.1 x64 shared SDK**，当前路径 `C:/dev/ffmpeg-7.1-full_build-shared`。只链接 libswresample / libavutil；SDK、导入库及 DLL 不提交到仓库。

更换 SDK 时，在 Qt Creator 的 CMake 配置中设置：

```text
OBS_FFMPEG_ROOT:PATH=C:/你的目录/ffmpeg-7.1-full_build-shared
```

首次配置也可以设置环境变量 `FFMPEG_7_HOME`；已有 CMake 缓存以 `OBS_FFMPEG_ROOT` 为准。当前使用 Ninja 单配置构建，CMake 将 `swresample-5.dll` 和 `avutil-59.dll` 复制到可执行文件目录，Qt 的 DLL 继续由 windeployqt 处理。

在仓库根目录构建：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_clients.ps1 -Deploy
```

运行 `OBS client/build-agent/OBS_Publisher.exe`，登录后选择麦克风和/或系统声音，点击“开始采集”。设备旁显示输入电平，下面可调音量或静音，“混音输出”显示统一格式、电平、包数及削波统计。两个输入均为“不采集”时不产生音频；仅采音频也能使用整个处理链路。

当前没有扬声器监听、录音按钮或 AAC 编码，避免把回环重新播放形成反馈。可以用测试生成的 WAV 检查合成音频输出；该文件只含人工生成的 440Hz 与 660Hz 正弦波，不包含设备录音。

## 验证与调试

```powershell
$env:PATH = "C:/software/Qt/Tools/CMake_64/bin;" + $env:PATH
python scripts/run_checks.py
python scripts/run_capture_checks.py --audio-mix
python scripts/run_capture_checks.py --ui-only
```

`client.audio` 使用人工数据，不打开设备。检查 44.1kHz 单声道 → 48kHz 立体声、任意分包、累计时间、96kHz 下采样抗混叠、多声道与非法布局、重复包、格式切换、时间跳变、双路对齐、补零、静音消费、音量、削波、有界缓存和停止排空。完整测试会生成 `out/音频重采样混音验证.wav`：48kHz、双声道、PCM16、1 秒；Float32 → PCM16 仅在测试写 WAV 时进行。

`--audio-mix` 在 Windows 正常交互图形会话使用默认输出设备，短暂播放人工零值音频，然后验证真实 WASAPI 回环 → 重采样 → 混音 → 控制器信号，以及正常停止、重启、退出登录。没有可用默认端点时明确 SKIP；不打开麦克风，不保存任何采集音频。

2026-10-08，Qt 6.11.2 / MSVC x64 Debug 实测：

- 两个客户端及四个测试程序构建通过；四个常规 CTest 全部通过，9.34 秒，音频六组测试通过。
- 真实默认 WASAPI 回环链路通过，初次运行交付 41 个固定格式混音包；停止后无继续交付，重启成功，退出登录后无迟到输出。
- 普通及 200% DPI 的 UI 验证通过；两行混音统计和长错误文本完整显示，小窗口可滚动。
- UML 生成 39 张类图、34 张时序图，新增三张音频类图与三张处理时序图；图像使用中文名称。

本步没有真实麦克风与回环同时工作的硬件验收；双路格式、对齐和混合使用合成数据验证。未实现两块设备长期时钟漂移的自适应补偿、降噪、AGC、软限幅器、音视频同步或编码；超过时间跳变阈值时以不连续新段恢复，不能据此宣称长期音画同步。

建议在 Qt Creator 按链路依次打断点：

| 断点 | 观察内容 |
| --- | --- |
| SessionController::startCapture | 本轮选择、begin 之后 enabled、原有音量/静音是否保留 |
| WasapiCapture::decode | sampleRate、channels、channelMask、timestamp100ns 和原始包大小 |
| SessionController::consumeAudio | 两个邮箱取包、原始信号、push；所在 GUI 线程 |
| AudioResampler::configure / resample | 输入布局、m_context、capacity、produced、m_outputFrames |
| AudioMixer::takeFrames / mixFrame | m_origin、m_cursor、ready、两路队列、补零及削波统计 |
| SessionController::deliverCapture | mixedAudioReady 的 3840 字节包和每包 100000 个 100ns 单位增量 |
| SessionController::finishAudio | finished 后排空与退出 clear 的不同分支 |

媒体运行中暂停进程会造成缺包或队列溢出；检查算法时优先单独运行 OBSAudioTests，其时间和输入可重复。实际采集适合看计数/标志或使用条件断点，不输出每个采样值，也不把断点停顿当成正常实时性能。

## API 参考

- [FFmpeg 7.1 libswresample API](https://ffmpeg.org/doxygen/7.1/group__lswr.html)
- [FFmpeg 官方重采样示例](https://ffmpeg.org/doxygen/7.1/resample_audio_8c-example.html)

下一步按已确认顺序选择视觉模型并建立原始基线，再评估量化和是否需要剪枝，接入既有 GPU 视频链路。
