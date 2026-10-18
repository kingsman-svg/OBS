# 第023步：音视频阶段总结与简历表述

日期：2026-10-18。项目名称建议使用“CourseStudioNext：C++ 音视频直播与点播系统”，说明基于CourseStudio技术栈重构，避免与开源OBS Studio源码二开混淆。

## 收尾结论

音视频模块可以作为可运行、可演示的阶段成果结束功能开发：采集、处理、编码、推拉流、解码、播放、同步和发送/播放缓冲控制均已形成闭环。不以实现所有播放器功能、AI Agent或生产化部署作为本次收尾前提。

阶段收尾表示停止增加功能，不表示所有环境和指标已经验收。综合弱网、跨机/NAT、真实设备长期漂移与听感测试仍可按约定共同进行；本次没有重跑硬件或网络实验，沿用各阶段已记录的验证结果。

SRS二开不再限定为Mac独立项目，后续在本仓库SRS目录推进；当前仍运行官方镜像，本次不改源码、不迁移媒体路径。MediaMTX、OpenH264原Mac独立项目约定不变。

## 当前三条媒体路径

### 推流、录制与原生播放主链路

```text
WinRT摄像头 / WGC窗口屏幕
  → D3D11纹理 → CUDA/TensorRT人脸定位 → Shader磨皮/提亮
WASAPI麦克风 / 系统回环
  → SWR重采样 → QPC对齐 → 双路混音与DSP
两路 → MediaTimeline共同原点与PTS
  → MediaPublisher有界队列 → NVENC H.264 + AAC
  ├─ MP4本地录制
  └─ FLV/RTMP → SRS → RTMP → 原生Player
       → D3D11VA解码 / 明确软件回退
       → GPU颜色转换、纹理显示 + WASAPI音频输出
       → 音频主时钟驱动视频调度、直播自适应播放缓冲
```

### 点播路径

```text
本地文件 / HTTP(S)媒体
  → FFmpeg解封装/解码 → 原生Player同一套GPU显示与音频时钟
```

当前具备播放、停止、音量、EOF排空与重复播放；暂停、跳转未实现。

### 可选浏览器研究链路

```text
Publisher → RTMP → SRS
  → 自建OBS-MEDIA网关（作为RTMP拉流客户端）
  ├─ H.264 AVCC→Annex B适配，视频不重编码
  └─ AAC→PCM→48kHz立体声→20ms Opus
  → 共享MediaUnit有界队列
  → 每观众RtpClock、双Track、共同RTCP SR/SDES
  → libdatachannel RTP打包、基础NACK、ICE/DTLS/SRTP
  → 浏览器同一MediaStream/video元素播放
```

自建网关不是RTMP接收服务器，没有改动SRS转换源码。SRS自身也能直接转换RTMP→WebRTC；外置网关用于掌握、验证可控的转换与同步策略，不是业务必需中间层，也没有证据证明它优于SRS原生实现。当前原生Player仍以RTMP播放，浏览器网关是可选路径。

## 已完成的工作与代码归属

| 方向 | 已完成内容 | 主要代码/阶段记录 |
| --- | --- | --- |
| 客户端与业务服务 | 同一CMake工程的Publisher/Player、简单MVC、异步HTTP登录、健康节点轮询、房间/成员/心跳/会话管理 | `OBS client/SessionController.*`、`OBS server/code/services/`；第003步、server-services |
| Linux网络库 | Reactor、epoll事件分发、非阻塞TCP、读写缓冲、eventfd任务唤醒、timerfd定时器、HTTP与信令解析 | `OBS server/code/net/`、`protocol/` |
| 系统采集 | WinRT摄像头、WGC屏幕/窗口、WASAPI麦克风/回环；设备枚举、启停、失败处理与独立预览 | `VideoCapture`、`WasapiCapture`；第004步 |
| GPU处理与显示 | D3D11 Shader/SwapChain、D3D11/CUDA互操作、GPU预处理、TensorRT检测、框/五点驱动磨皮提亮，处理纹理用于预览和输出 | `VideoRenderer`、`GpuFaceDetector`、`GpuBeauty`；第005/008/014步 |
| 模型部署 | 独立C++/TensorRT优化工具，FP32/FP16与INT8 Q/DQ对照、可选2:4剪枝、候选门限和engine重载验证；实际示例交付FP16 | `cpp/`；第007步 |
| 音频DSP | 48kHz立体声Float32/10ms、麦克风与系统声QPC对齐、增益/静音平滑、左右联动预读限幅、可信设备时间驱动的有界SWR软补偿 | `AudioResampler`、`AudioMixer`、`StereoLimiter`；第006/020步 |
| 编码前同步 | 共同时间原点、固定视频/音频PTS、有界等待、缺口/尾部处理；Controller统一交付 | `MediaTimeline`；第009步 |
| 编码与输出 | GPU帧输入NVENC H.264、AAC编码/FIFO、FFmpeg时间基转换、MP4正常排尾、RTMP取消与资源收尾 | `OBS client/ffmpeg/MediaEncoder.*`、`MediaPublisher.*`；第010/012步 |
| 解码与播放 | 本地/HTTP(S)点播和RTMP直播复用；D3D11VA、GPU颜色转换、软件回退计数、WASAPI音频主时钟与视频取帧 | `MediaDecoder`、`MediaPlayer`、`GpuVideoOutput`、`WasapiPlayback`；第011/013步 |
| 发送端压力控制 | 运行时手动调码率、完整配置邮箱、基于写出/队列反馈的启发式ABR、直播过期帧裁剪、AAC显式追赶、VBV/GOP/交织预算 | `MediaPublisher`、`AdaptiveBitrate`；第015～018步 |
| 播放端缓冲 | 直播目标50～150ms自适应，欠载冻结主时钟、补足恢复，稳定后降目标并追赶；点播不启用 | `PlaybackBuffer`、`WasapiPlayback`、`MediaPlayer`；第019步 |
| 浏览器网关 | AAC→Opus、H.264适配、共同RTP/NTP参考、每观众会话、异步HTTP协商、基础NACK接入及重推恢复 | `OBS server/code/media/`；第021/022步 |
| 工程验证 | Docker、固定依赖、中文Git记录、编号视频/生成音频、现有测试入口、UML时序图与阶段文档 | `scripts/`、`tests/`、`SRS/check.py`、`docs/` |

GPU路径避免将完整画面反复读回CPU，但存在纹理复制和小量检测结果回传；不称作全链路零拷贝。ABR控制编码负载，TCP拥塞控制由操作系统执行，未自行实现GCC或精确可用带宽估计。

## 可以引用的验证结果

| 结果 | 测量范围与依据 |
| --- | --- |
| 最近客户端7组、服务端4组核心回归通过 | 第022步构建/功能回归，不等于压力测试 |
| 编号帧入队→Player GUI交付P50：350.8→200.5ms；P95：367.4→214.5ms | 第018步，本机RTX5060、Docker/SRS、320×180/30fps生成流，同配置的较宽/低延迟预算对照，各6.5秒、预热2秒；不含真实采集、美颜或显示器扫描，不能写成完整端到端延迟 |
| 实际NVENC同会话目标3000→400→2000kbps，270帧时间戳连续 | 第017步；目标码率与真实窗口包码率分开记录，不代表瞬时输出严格恒定 |
| 背压后自动目标8000→6000→4800→3840kbps，稳定后回升4224kbps | 第017步，本机代理暂停读取904ms、最长写出811.9ms；输出会话只打开一次，不是公网丢包实验 |
| TensorRT FP32 3.1464ms、FP16 1.53405ms，约2.05倍 | 第007步，单图smoke、GPU enqueue基准；不是整个视频链路或正式标注精度测试，不能写成全项目帧率翻倍 |
| 网关生成包络正常/重推后相对偏移约−40/−70ms | 第022步，浏览器实际视频像素对静音WebAudio消费分支；不是物理扬声器/显示器误差或完整传输延迟 |
| 合成采集时钟±100/300ppm、双路相反漂移通过 | 第020步，人工时间轴模拟；不是已经连续运行多小时的真实设备验收 |

简历可以选一项与岗位最相关的指标，保留测量条件；其余留作面试证据。指标来源是历史阶段记录，本次仅整理文档。

## 可直接使用的简历版本

**CourseStudioNext｜C++ 音视频直播与点播系统**

技术栈：C++20、Qt6、FFmpeg、D3D11/D3D11VA、NVENC、WGC/WinRT/WASAPI、CUDA/TensorRT、Linux Reactor、Docker、SRS。

项目介绍：基于CourseStudio技术栈重构Publisher/Player双端系统，完成多源采集、GPU处理、音视频同步、硬件编解码、本地录制及RTMP直播闭环，重点优化发送积压、码率调节和播放缓冲。

- 封装WGC/WinRT视频采集和WASAPI麦克风/回环采集，通过D3D11纹理串联预览、TensorRT人脸定位、Shader磨皮提亮与NVENC输入；Player接入D3D11VA硬解码及GPU颜色转换，区分软件回退路径。
- 实现双路音频48kHz重采样与QPC对齐混音，加入增益平滑、立体声联动预读限幅和有界采样时钟软补偿；建立编码前共同PTS时间线及播放端WASAPI音频主时钟，处理缺口、停止排尾和重复启停。
- 设计有界直播队列，按媒体跨度和等待年龄裁剪过期帧，音频显式跳过缺口；基于写出阻塞、队列状态及处理负载实现启发式ABR，在用户上下限内快降慢升，通过参数邮箱由编码线程完成运行时NVENC调码率。
- 协调直播VBV/GOP、FFmpeg交织与播放缓冲预算，实现欠载冻结/补足恢复及稳定后降缓冲；在本机320×180/30fps生成流对照中，将帧入队至GUI交付P50由350.8ms降至200.5ms、P95由367.4ms降至214.5ms，不含真实采集、美颜与显示器扫描。
- 自建Linux Reactor网络库及登录、健康节点调度、房间信令服务；另实现可选RTMP→WebRTC网关，完成AAC→Opus、共同RTP/RTCP时钟映射，接入libdatachannel的RTP打包与基础NACK，验证浏览器播放和重推恢复。

一页简历可保留前四点，将最后一点按岗位改为服务端或WebRTC侧重点；不要把所有API和全部模块名堆在同一条中。亲自实现、调试、能解释的部分才写为个人职责；来源于开源工程或库的能力明确写重构、封装、接入。

### AI部署岗位可替换的一条

- 编写独立C++/TensorRT引擎优化工具，对SCRFD-10G-KPS进行FP32/FP16、INT8显式Q/DQ及可选2:4剪枝实验，依据检测一致性和GPU基准选择交付并重载验证；本机单图smoke选择FP16，enqueue由3.1464ms降至1.53405ms，接入D3D11/CUDA视频链路。INT8更小但该轮未更快，稀疏组合候选未过门限，未作为最终交付。

## 面试时需要守住的边界

- “接入SRS并扩展外置协议转换网关”是已完成事实；“修改SRS源码实现新的转换模块”尚未完成。
- RTP打包、基础NACK和ICE/DTLS/SRTP来自libdatachannel；共同时间映射、自定义SR时钟逻辑、有界队列和媒体编排由本项目实现。
- 发送压力反馈ABR不是修改FFmpeg/TCP拥塞控制算法，也不是完整WebRTC GCC；不把本机写出字节率当可用带宽。
- 配置的缓冲/VBV预算、浏览器包络偏移、尾部两轨PTS差和完整端到端延迟是不同指标，不能互换。
- 正式模型标注精度、互联网NAT/TURN、全面弱网、长期设备漂移、生产鉴权/账号库、高并发、暂停跳转和Agent未完成；这些不阻止当前学习/展示阶段收尾，也不应出现在已完成职责中。

## 收尾后的维护方式

以现有提交和阶段文档复现，优先熟悉Controller→begin/start→run→数据交接→stop/清理/重启，以及各类持有的线程和资源。继续开发时由用户明确选择SRS源码二开、综合验收或其他方向，不自动新增模块。
