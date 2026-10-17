# 第018步：RTMP低延迟预算与编号画面测量

接续[第017步ABR](017-abr.md)，在现有线程中统一直播编码、交织和播放预算，并加入可核对的实际媒体路径测量。只扩展现有代码与测试，不增加生产线程、测试程序或类图。

## 配置与对象归属

`SessionController`拥有Publisher/Player线程；`MediaPublisher::run`独占编码器、ABR及FLV/RTMP资源；`MediaPlayer::run`独占解码器，并持有一个WASAPI输出线程。GUI只交付数据、提交配置与复制统计。

`RecordingSettings::lowLatency`默认true，仅对直播生效。`MediaPlayer::begin`、`MediaDecoder::open`与`WasapiPlayback::begin`增加末尾可选的同名参数，向下传递本轮固定预算。false保留较宽预算，供开发对照或显式回退，不自动掩盖初始化失败。界面不增加一组高级参数控件，正常直播直接使用新预算。

| 环节 | 较宽预算对照 | 当前直播默认 |
| --- | --- | --- |
| NVENC VBV | 目标码率×2秒 | 目标码率×200ms |
| 关键帧间隔上限 | 60帧/2秒 | 30帧/1秒 |
| 交织队列PTS跨度阈值 | 100ms | 40ms |
| 输入探测字节/媒体时长预算 | 256KiB/300ms | 128KiB/100ms |
| RTMP接收缓冲请求 | 100ms | 50ms |
| WASAPI应用预缓冲 | 100ms | 50ms |
| WASAPI端点容量请求 | 100ms | 30ms，并限制待播padding最多30ms |
| 已解码PCM队列 | 200ms | 120ms |
| 已解码视频队列 | 最多16帧 | 另按300ms的队头/新帧PTS差裁剪，仍保留16帧数量兜底 |

这些数值分别约束率控模型、媒体跨度、协议请求或应用缓冲，不可相加当作端到端延迟保证。RTMP接收缓冲请求也不代表服务器必然按同样时长缓存。

## Controller → begin → start → run

1. Controller按原流程开始直播。Publisher的begin保存设置、清状态并start；run等首帧设备，再调用`avio_open2`建立TCP/RTMP连接。建连耗时单独记录，包含协议握手与发布交互。
2. `MediaEncoder::open`在`avcodec_open2`前设置直播VBV、峰值目标和1秒GOP，继续沿用无B帧、关闭lookahead、tune=ll、zerolatency及delay=0。初始化失败仍走RAII清理并报错。
3. FLV交织阈值设40ms；继续使用`AVFMT_FLAG_FLUSH_PACKETS`和显式`avio_flush`，RTMP继续启用TCP_NODELAY。flush只推进IO缓冲，不能清除交织队列或TCP里已排队的数据。
4. 手动或ABR产生新目标后，原推流线程在下一视频前调用`setVideoBitrate`。VBV按本轮200ms预算随目标缩放；不因调码率重新变成2秒，不重建连接或更改PTS。
5. Player的begin清统计并start。解码线程open时缩短直播探测，保留探测阶段读到的首个关键帧；不盲目用nobuffer丢掉它后等待下一个GOP。点播参数保持原流程。
6. WASAPI线程等首张转换后视频及50ms PCM预缓冲，再启动设备时钟。记录`GetBufferSize`实际容量；即使驱动给了更大的容量，每次也只维持最多30ms待播padding。PCM最多120ms，溢出丢未播放旧PCM，并用`m_audioGap`明确通知输出线程跳到首个保留PTS。启动直播同样直接对齐首个保留PCM，不补播加入前的历史静音；普通短缺口仍按原策略补零。
7. 硬解可能提前交付未来画面，视频仍按音频主时钟到期显示。新窗口使用300ms，给探测、交织和声音播放留余量；试验中的150ms窗口会过早丢掉未来帧，已由帧交付比例检查发现并修正。只裁解码后纹理，不任意丢参考压缩包。
8. stop仍关闭入口、中断IO、唤醒等待、释放编码/解码及COM资源；finished后才允许重启。begin清除旧初始化时间、队列及追赶计数。

## 可观测项

- Publisher：`connectUs`是avio_open2耗时；`readyUs`为run入口到封装头就绪；`firstMediaUs`为同一起点到首次媒体IO写出，0表示尚未发生。三个时长存在包含关系，不相加。
- 编码：`encoderVbvMs`、`encoderGopFrames`及`muxInterleaveMs`说明当前配置预算，不能代替码流与实际延迟验收。
- Player：`openUs`、`firstDecodedUs`、`firstDeliveryUs`分别从begin计算；交付时间截至GUI取走纹理，不包括SwapChain/显示器扫描。另显示解码视频队列跨度、PCM队列时长、端点padding/实际容量和音频追赶。
- 音频区分解码样本、实际PCM提交、主动丢弃与应用补零。验收发现旧逻辑在主动丢弃后的短缺口仍补零，会持续追逐前移的队头；新增显式追赶解决它。新旧预算对照均使用此修复，避免以较宽预算下的错误播放充当基线。
- 诊断文本最多5Hz刷新，GUI取帧仍按原定时器进行。现有界面滚动显示，不新增拥挤的参数面板。

所有阶段使用各自进程内的单调时钟求耗时，不把采集QPC、媒体PTS或不同设备的墙钟直接相减。

## 编号画面对照验收

```powershell
./scripts/build_clients.ps1 -Deploy
$env:PATH = "C:/software/Qt/Tools/CMake_64/bin;" + $env:PATH
python scripts/run_checks.py
python scripts/run_publish_checks.py --gpu --weak-network
python SRS/check.py --beauty --latency
```

扩展现有`SRS/IntegrationTests.cpp`和`SRS/check.py`的`--latency`入口，同一SRS配置下依次运行较宽预算和默认预算。每组使用320×180/30fps生成画面、AAC合成声音和真实NVENC/D3D11VA/WASAPI，输出音量为0。

每张画面包含16位编号、8位校验与8位标识，画进黑白块像素。入队前保存编号对应的单调时间，观众`takeVideo`后读回已解码GPU画面的编号，匹配同一帧计算耗时。CPU回读仅存在于此测量代码，不加入产品GPU链路。测量点在GPU回读之前取时，实际测量工作仍会带来少量测试负载。

预热2秒后统计P50/P95和范围，同时检查编号校验、PTS/播放时钟单调、帧交付比例、音频欠载、队列峰值与停止。要求两组均至少交付80%的解码帧、实际PCM提交不少于已解码样本的75%，避免靠大量丢帧或持续补零得到表面正常的结果；另验证调码率后VBV预算保留。JSON报告为`out/低延迟验证/RTMP延迟对照.json`，示例图为`out/低延迟验证/推流编号画面.png`。

“入队→GUI交付”包含本机编码、RTMP、SRS转发、解码和音画等待，不包含真实采集、前置美颜或显示器扫描，不称作完整玻璃到玻璃延迟。对照项保持同一部署配置和硬件，使用旧预算而非旧二进制；同一会话不切换预算，避免把重启或初始化混进运行时数据。

### 2026-10-17本机实测

两组都包含PCM主动裁剪后的显式追赶修复；每组运行6.5秒，以下稳态样本均来自2秒预热之后。

| 指标 | 较宽预算 | 低延迟预算 |
| --- | ---: | ---: |
| 同帧有效样本数 | 124 | 129 |
| 入队→GUI交付 P50 | 350.8ms | 200.5ms |
| 入队→GUI交付 P95 | 367.4ms | 214.5ms |
| Player begin→首张交付 | 275.3ms | 108.2ms |
| PCM队列峰值 | 200.0ms | 110.0ms |
| 设备实际容量/待播峰值 | 100/100ms | 30/30ms |
| 视频队列跨度峰值 | 299.0ms | 166.0ms |
| GPU解码/交付帧数 | 192/185 | 193/189 |
| 实际PCM提交/解码样本 | 295520/309248 | 300960/309248 |
| 主动PCM丢弃样本/追赶次数 | 6144/5 | 5632/4 |
| 应用补零样本/欠载次数 | 640/2 | 0/0 |

较宽预算的应用补零与低延迟预算的主动丢弃分别记录，不能因欠载为0就宣称声音完全连续。本机短测中存在启动追赶；仍需更长时间和真实网络验证音频质量。较早未修复PCM追赶的对照结果不作为最终基线。

同轮SRS两次启停各GPU解码132帧，交付127/129帧、欠载0；停止112/102ms，握手取消94ms。WGC→TensorRT美颜→NVENC→SRS→GPU Player回归通过，美颜53帧。真实NVENC手动调参仍为3299→431→2634kbps；隔离RTMP暂停912ms、最长写出811.8ms后，ABR目标8→6→6.5Mbps并切回手动5Mbps，200ms VBV保持，尾部两轨差42ms、停止6ms。

最终七组核心CTest全通过（18.66秒）；真实WASAPI点播的普通/延后音轨/延后视频/长视频尾部四组通过，欠载均0。二倍缩放下再次运行SRS美颜、ABR按钮切换和两轮启停通过，已查看实际中文诊断截图；52张时序图渲染成功。构建与部署通过，不恢复历史独立测试程序。

## 边界与接续

保持MP4和点播原预算，保留开发对照参数；不修改SRS上游实现，不关联Mac的SRS/MediaMTX/OpenH264研究项目。编码峰值、画质、欠载与延迟存在取舍，当前默认只经过本机实验校准。

主动追赶会跳过一段媒体时间，已经提交到端点或TCP的数据仍按原顺序处理；不能把这些数据从应用队列中撤回。更精细的追赶过渡、感知音画同步和声音质量需要后续实际素材验收。

后续继续做隔离限带宽、RTT/丢包、跨机校时或光学测量、设备兼容和长时间运行验收。TCP有序可靠交付的队头阻塞、自动重连、长期时钟漂移与WebRTC不在本步完成范围。

参考：[FFmpeg7.1 NVENC初始化与重配置](https://github.com/FFmpeg/FFmpeg/blob/n7.1/libavcodec/nvenc.c)、[FFmpeg封装选项](https://ffmpeg.org/ffmpeg-formats.html#Format-Options)、[FFmpeg RTMP选项](https://ffmpeg.org/ffmpeg-protocols.html#rtmp)。流程见[时序源码](uml/rtmp-latency.mmd)与[直播低延迟预算时序图](uml/直播低延迟预算时序图.png)。
