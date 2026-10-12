# 第012步：独立SRS与OBS直播接入

## 边界与对象持有

`SRS/`是独立副项目，`OBS-SRS`进程运行官方6.0.193；上游源码有自己的`obs-bridge`分支，部署和二开见[SRS说明](../SRS/README.md)。现有`OBS`容器运行登录、调度、信令。两者通过地址和协议协作，客户端通用FFmpeg代码不依赖SRS内部类。

Publisher的`SessionController`在GUI线程拥有采集/检测线程、普通混音/同步对象、本地录制`m_publisher`和RTMP输出`m_livePublisher`。两种输出各持有独立工作线程，其`run`局部持有`MediaEncoder`、`AVFormatContext`、IO。所有同步帧/PCM共用现有信号快速进入两套有界队列；本步同时输出会开两个NVENC/AAC编码器，换取失败和收尾互不影响，尚未做一次编码多路封装。

Player仍只有一个`MediaPlayer`，内部拥有一个`WasapiPlayback`输出线程，`run`局部独占`MediaDecoder`。直播与点播由`begin`的`live`参数选择，不新增Model/Controller。

## Publisher：Controller → begin → start → run

1. GUI创建房间，SignalServer给创建者返回`pushUrl`，房间`streaming=false`；公共房间数据不返回主播推流字段。
2. GUI“开始直播”要求登录/信令可用、房间已创建、旧采集和两个输出均退出。先读取本轮源，空选择直接提示，不启动线程。
3. `startCapture`清混音和同步状态，`MediaTimeline.begin`建立共同QPC原点，再`m_livePublisher.begin`保存参数/清队列/`QThread.start`；最后启动采集。勾选录制时同时开启本地MP4入口。
4. 采集/检测在各自线程输出独立帧。GUI定时器取最新数据，经混音/同步后发` synchronizedVideoReady / synchronizedAudioReady `；输出槽只入队。
5. RTMP线程`run`等首个视频设备→建立FLV上下文→`avio_open2`连接RTMP→创建NVENC/AAC和输出流→写头。`opened`表示初始化完成；写出头之后的媒体字节才发一次`transmitting`。
6. GUI收到`transmitting`后发送`live.start`，SignalServer广播`live.changed`，观众此时才启动拉流。信令请求按编号串行，开始请求在途时发生停止，响应后补发停止；关闭房间在途时排队`room.leave`。

## Player：事件 → begin → run → GUI/WASAPI

1. GUI加入房间；等待中的房间不启动网络媒体连接。加入已开播房间或收到`live.changed=true`后，Controller从`pullUrl`调用`MediaPlayer.begin(source, audioOutput, true)`。
2. 播放线程打开RTMP，探测预算300ms/256KiB、RTMP缓冲100ms；读包超时4秒，AVIO interrupt响应用户停止。打开阶段总操作期限10秒。
3. 解码仍为FFmpeg软件解码→BGRA上传GPU；音频重采样为48kHz立体声Float32。输入媒体共同原点归一化，直播忽略固定音轨尾部信息。
4. 直播视频最多8张已解码帧，溢出丢旧纹理；点播保持16张队列背压。这样丢弃不会破坏解码器参考帧。
5. 直播PCM队列最多200ms，溢出丢旧包；点播保留1秒背压。输出线程遇到大于200ms的正向PTS缺口会跳过过期媒体区间，并把设备时钟映射到新位置；短缺口继续补零。当前是基础追赶策略，弱网听感和长时漂移仍需后续测试。
6. 有声音时用WASAPI实际播放位置调度GPU画面；关闭声音时首帧建立QPC时钟，正向偏移超过300ms时追赶新媒体位置。GUI每10ms取到期画面，独立非模态窗口显示。

## stop → 清理 → finished → 重启

- 主播停止：先撤销媒体发送意图，取消RTMP IO、通知信令停播，再请求采集退出。网络输出不等待AAC尾部，关闭连接；本地MP4继续排空并封存。取消可由GUI立即触发；连接/每次网络写出另有5秒操作期限。
- 观众停止：GUI立刻停交付定时器/清窗口，`MediaPlayer.stop`请求中断，唤醒队列与音频线程。Decoder先销毁，再在播放线程等待WASAPI归还资源。
- 主播断流/读取失败：Player锁存错误并退出，可手动重试；不在循环中无限重连。主动停播事件清媒体，保留房间等待；下一次开播重新启动。
- 房间关闭、退出登录、信令断开：对应媒体停止，迟到画面不会重新打开窗口；直播到点播模式切换会停止媒体并退出房间。
- `QThread.finished`到GUI后才解除本轮pending锁。重开直播从新一轮采集原点开始；正常GUI不调用`wait`。

## 验证与后续

服务端4组和客户端8组回归通过。独立`SRS/IntegrationTests.cpp`验证API、创建者地址、观众等待、实际NVENC/RTMP/解码/WASAPI、同房间两轮启停与双端握手取消；还从真实Publisher按钮捕获测试自建窗口，经WGC/同步/编码/SRS交给后加入的Player，验证关闭房间清理。输出音量0，画面与音频均为测试生成，不读取用户采集数据。具体结果见[SRS验证记录](../SRS/README.md#验证与边界)。

本步交付RTMP直播；SRS原生RTMP→WebRTC配置和UDP映射已准备，客户端RTC接入/浏览器端协商验收、SRS自定义桥接、弱网与生产鉴权仍是后续。时序图：[推流](uml/live-publish.mmd)、[播放](uml/live-play.mmd)、[停止](uml/live-stop.mmd)。
