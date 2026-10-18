# 第022步：网关 AAC→Opus 与音视频同步

在第021步的视频网关上加入音频：Publisher 的 H.264/AAC 经 SRS RTMP 接入，由自建网关透传 H.264、将 AAC 解码并重采样后编码为 Opus，再通过同一个 WebRTC 会话交给浏览器。SRS 的 RTC 转换没有参与。当前仍为本机 Docker 环境，网关信令为自定义 HTTP JSON。

## 对象、资源与线程

| 对象 | 谁持有，在哪个线程执行 |
| --- | --- |
| `MediaGateway` | main 持有；HTTP、会话表、定时器都在原 EventLoop 线程。 |
| `RtmpSource` | Gateway 持有一个读取线程，原来的 run 同时分发音视频，不为音频另建线程。 |
| `H264Adapter` | run 的局部对象，保持原来的无 B 帧 H.264 码流适配。 |
| `AudioTranscoder` | run 的局部对象，独占 AAC decoder、SwrContext、Float32 stereo FIFO、libopus encoder。 |
| `MediaUnit` | 普通数据：独立拥有的压缩字节、kind、PTS、duration、epoch、IDR/不连续标记；视频与音频共用契约。 |
| `RtcSession` | Gateway 为每位观众持有，公开接口在 loop 执行；拥有可选 Opus Track、视频 Track 和同一个 RtpClock。 |
| `RtpClock` | 普通类，无线程，统一源 PTS 映射、单调会话钟和 NTP 参考。 |
| `ClockSenderReport` | 每轨一个普通 MediaHandler，共享同一个 RtpClock；在该轨打包后的 outgoing 回调统计并发送 SR+SDES。 |

libdatachannel 继续拥有 ICE/DTLS/SRTP 内部线程；它的回调只更新共享原子状态，不碰 FFmpeg。新增文件平铺 `OBS server/code/media/`，没有增加 MVC、独立 Demo 或测试程序。

## 启停和数据流

1. **main → Gateway::start → loop**：与上一阶段相同，构造只绑定回调。start 开 HTTP、读取线程和 5ms 定时器，main 调用 loop。
2. **HTTP → select**：只更新流名/命令序号，唤醒或打断旧 IO。读取线程打开 RTMP、探测流；有视频时创建 H264Adapter，有 AAC 时创建 AudioTranscoder，并发布 hasAudio。不含音频的流继续走单视频轨。
3. **run → 音频 push**：av_read_frame 得到 AAC 包后，在同一读取线程调用 `AudioTranscoder::push()`。内部 send_packet/receive_frame 解出真实采样率、布局、格式和 PTS，再初始化输出段。
4. **SWR → FIFO → libopus**：输入可以是44.1kHz mono或48kHz stereo等解码格式；输出固定48kHz stereo交错Float32，FIFO按960采样帧即20ms送编码器。Opus目标96kbps、默认VBR、lowdelay，当前实际 initial_padding 为120采样帧（2.5ms）。
5. **跨线程交接**：编码器输出包复制为 MediaUnit；音视频共用32单元/8MiB/300ms的队列。跨度使用真正的最小/最大PTS，兼容两路交错到达；loop 每5ms取出并按PTS排序，同PTS视频先行。裁剪会要求所有会话重新等待共同IDR。
6. **协商和发送**：浏览器按上游 hasAudio 申请 recvonly H.264 和可选 recvonly Opus/48000/2；服务端验证两轨和视频profile/level。每轨有独立SSRC、随机RTP原点、序号和512包NACK缓存；两轨共用PeerConnection、MSID、CNAME、RtpClock。
7. **浏览器播放**：ontrack把两轨加入同一个MediaStream，交给同一个video元素；默认静音满足自动播放，点击“开启声音”后播放音频。浏览器依据RTP/RTCP参考钟完成自己的音画调度。
8. **正常EOF**：按decoder→SWR→FIFO尾包→encoder顺序排空，finish只生效一次。RTP不携带文件容器的尾部裁剪信息，最后Opus包可能含不足20ms量级的填充，不把它全部宣称为有效源样本。
9. **stop/换流**：取消、网络超时和换流不排旧段尾包，直接在读取线程释放旧上下文。停止服务时，loop返回main后关闭HTTP/定时器/RTC并通知source.stop，析构join读取线程，再释放其他资源。
10. **重推恢复**：重新打开输入更新epoch；同一SRS订阅连接内重推也可能继续使用原连接和epoch。音频若出现超过5ms的真实时间缺口或格式变化，重建SWR/FIFO/Opus段并标记不连续；会话共同等待下一IDR重新锚定，不补回大段静音。源视频倒序或超过1秒的前跳会重开上游。

## PTS 与编码延迟

解码帧以 best_effort_timestamp/PTS 为源时间。每段以首个解码样本的PTS建立48kHz样本位置，之后根据真实转换/消耗的样本数推进；SWR滤波等待只影响交付时机，不另加到媒体PTS。小于5ms的RTMP毫秒量化差不触发重建。

Opus编码器返回的 packet PTS 已经包含 initial_padding/lookahead 的修正，直接换算为微秒交给 MediaUnit；不能再手动加一次编码延迟，也不能把“编码完成/发包时刻”当成采集时间。最初的负PTS保留到会话入场策略处理。单元验证会检查源首个AAC包PTS与Opus输出首包PTS之间只减去一次实际lookahead。

`RtpClock::map()`先等待可解码IDR，为两轨共同建立：

```text
映射时间 = 本次共同输出起点 + 源PTS - 本次共同源起点
视频RTP = 视频随机原点 + 映射时间 × 90000
音频RTP = 音频随机原点 + 映射时间 × 48000
```

例如源音频比视频晚80ms，即使AAC转码和交接又多等待了10ms，映射后仍保留80ms的源偏移，不能把两轨各自首包都改成0。入场前的完整音频包丢弃；新段输出起点保持两轨向前，分轨检查重复/倒序，32位RTP时间戳使用整数换算和自然回绕。

这里保持并传递输入的音画时间关系，不自动修复内容本身标错PTS、采集端原有不同步或任意时钟漂移。

## 共同 RTCP Sender Report

旧的库内RtcpSrReporter将最新包时间戳与发SR时的墙钟配对；两条不同缓冲/转码路径的等待差可能进入该配对。新增ClockSenderReport在首包及后续每约一秒有媒体发送时，使用同一个RtpClock生成：

```text
SR.NTP = 会话启动UTC + 当前单调钟经过时间
SR.RTP = 本轨随机原点 + 当前单调钟经过时间 × 本轨时钟率
```

SR中的RTP是与NTP同一时刻的理论媒体钟，不要求等于最近一次真正发出的包时间戳。两轨用相同CNAME的SDES关联，浏览器可以将90kHz视频与48kHz音频映射到共同参考时间。依据 [RFC3550 §6.4.1](https://www.rfc-editor.org/rfc/rfc3550.html#section-6.4.1)；Opus RTP时钟/声道协商参见 [RFC7587](https://www.rfc-editor.org/rfc/rfc7587.html#section-4.1)。

本步实现同步所需的基本SR/SDES和统计，继续复用库内NACK响应；未实现完整RTCP带宽调度、PLI到Publisher、TWCC/GCC、FEC或互联网穿透策略。发送统计不代表浏览器已经解码。

## 运行和构建

继续使用现有 `OBS-MEDIA`、HTTP8090、UDP8100–8109；音视频通过同一WebRTC连接复用端口。可选CMake库目标现在为 `obs_gateway_media`，依赖新增显式的libswresample（当前镜像已有配套开发包）。客户端的FFmpeg模块和弱网策略保持原调用方式。

仓库根目录PowerShell执行：

```powershell
& './OBS server/media.ps1' stop
& './OBS server/media.ps1' build
& './OBS server/media.ps1' start
```

打开 <http://127.0.0.1:8090/>，按第021步选择Publisher的流名称播放。画面出现后点击“开启声音”；纯视频输入会自动只协商视频，声音按钮保持不可用。

## 验证与边界

沿用服务端四组CTest，扩展原ProtocolTests：真实AAC编码→生产AudioTranscoder→真实Opus解码，覆盖44.1kHz单声道和48kHz立体声、非零PCM、双声道一致性、SWR排尾、Opus20ms步进、lookahead只修正一次、500ms缺口重建、finish重复调用。人工RtpClock覆盖80ms源偏移、不同交付时机、共同裁剪/重开、15小时RTP回绕；实际序列化SR检查跨轨NTP/RTP配对误差小于一个48kHz采样帧，并验证包/字节计数。

显式浏览器验收仍使用同一个入口：

```powershell
python SRS/check.py --gateway
```

真实MediaPublisher/NVENC生成带像素编号的画面和合成AAC；画面角落与440Hz测试音按同一个31位包络每200ms同步变化。Edge接收网关的H.264/Opus，视频实际呈现时读取像素，静音WebAudio分析远端音轨RMS，检查非零声音、真实静音段和包络关系。元素音量与分析分支增益均为0，不采集用户音视频，不向声卡播放测试音。

该包络验收比较浏览器呈现视频和WebAudio消费分支，搜索步长10ms、允许偏移±150ms，并要求包络匹配率超过88%；它包括两个浏览器分支的额外缓存，不是声卡/显示器物理嘴型或完整端到端延迟测量。重推检查保持同一个音频消费者跨过停播，不重建消费者掩盖恢复问题；另验证双观众、独立退出、再次播放和纯视频回退。报告及中文截图保存在 `out/媒体网关验证/`。

2026-10-18本机验证通过：服务端4组CTest、客户端7组核心回归全部通过；真实Publisher/NVENC→Docker网关→Edge的完整验收通过。首次播放245个样本的包络匹配率为96.33%，音频消费分支相对视频提前约40ms；同一消费者跨重推后185个样本匹配率92.43%，提前约70ms。正常播放观察段的concealedSamples为0，故意停播恢复期间浏览器出现过丢包隐藏补偿，不能据此宣称全过程零欠载。纯视频回退实际解码32帧、无音频轨；跨轨SR时钟校验误差约11.8μs，小于一个48kHz采样帧。

综合弱网、跨机/NAT、长时间漂移和真实设备听感验收仍按约定后续共同进行。WebRTC基础知识将结合这条已有音视频链路继续学习。
