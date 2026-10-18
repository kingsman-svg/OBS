# 第021步：自建 RTMP→WebRTC 视频网关

2026-10-18，完成第一阶段：真实 Publisher/NVENC → SRS 接收 RTMP → 我们的 Linux 网关拉流/适配 H.264 → libdatachannel → Edge 解码。SRS 的 RTC 输出没有参与这条播放链路。当前仅视频，AAC 在网关读取时忽略；后续再加入 AAC→PCM→Opus 和音画同步。

这是 Windows OBS 工程内的媒体服务增量，与 Mac 上独立进行的 SRS、MediaMTX、OpenH264 研究无关。网关没有修改 SRS 上游源码。

## 目录和职责

全部业务代码平铺在 `OBS server/code/media/`，没有加入 Qt、MVC 或 Windows GPU 依赖。

| 文件 | 职责及持有的资源 |
| --- | --- |
| `GatewayMain.cpp` | 解析配置、读取现有签名密钥、创建 main 线程 EventLoop/signalfd 和网关；调用 start、loop、stop。 |
| `MediaGateway.h/.cpp` | 持有 HTTP 服务、一个 RtmpSource、观众会话表、5ms 定时器及有界压缩帧队列；在 loop 编排流程。 |
| `RtmpSource.h/.cpp` | 独占一个 `std::thread`，在线程中持有 AVFormatContext、AVPacket、H264Adapter；支持异步换流、重试和停止。 |
| `H264Adapter.h/.cpp` | 普通类，读取 avcC 的真实 profile-level-id；使用 h264_mp4toannexb 转换完整 access unit，保留源 PTS，识别真实 IDR。 |
| `RtcSession.h/.cpp` | 普通类，一个观众一条 PeerConnection/Track；持有独立 RTP 序号、时间戳、SSRC、SR/NACK 处理链。 |
| `player.html` | 简单浏览器页面：登录、选择流名称、播放/停止、查看去敏统计；令牌只保存在页面内存。 |
| `config.json` | 固定 RTMP 基址、HTTP/UDP 端口、可达 candidate 地址和观众上限，不含密钥或真实密码。 |

`OBS server/Dockerfile.media` 固定 libdatachannel v0.24.6/提交 `6b1e2e620f1e37f0eafeee702eaea0043cb305fd` 及其子模块。Linux 使用 Ubuntu 的 FFmpeg 6.1 系列开发库，Windows Publisher 仍使用原来的 FFmpeg 7.1；两个进程通过 RTMP 通信。

## 从启动到结束

1. **main → start**：main 在任何工作线程启动前屏蔽 SIGINT/SIGTERM，创建 EventLoop 和 signalfd。网关构造只绑定回调；`start()` 启动 HTTP 监听、读取线程和 5ms 定时器，随后 main 调用 `loop.loop()`。
2. **选择源**：浏览器登录后 `POST /api/source` 提交流名。只允许 `[A-Za-z0-9_-]{1,96}`，基址由配置固定，不能传任意 URL。`RtmpSource::select()` 更新命令序号并唤醒线程，不在 HTTP 回调 join 或等待 RTMP。
3. **读取线程 run**：`avformat_open_input → avformat_find_stream_info → av_read_frame → H264Adapter::convert`。FFmpeg interrupt callback 检查 stop、换流序号和三秒 IO 期限；不存在的直播会重试，HTTP loop 仍可处理健康检查。AAC 本阶段不转发。
4. **浏览器发起协商**：创建 recvonly 视频 transceiver，限制 H.264 编码，收集本机 ICE 候选后提交完整 offer。网关验证现有格式的 Bearer 登录令牌，按真实码流的 profile/level 选择兼容的 payload type；不兼容返回 406。
5. **异步产生 answer**：RtcSession 注册库回调，回调只写共享原子状态，不访问网关或 FFmpeg。HTTP 请求保留异步 Reply；网关 tick 看到 ICE 收集完成后才在原 loop 回复 answer，等待期间不阻塞 epoll。当前为自定义 JSON 协商 API，尚未实现标准 WHEP。
6. **数据交接**：读取线程把独立拥有的 Annex B 压缩帧放入共享队列；不逐帧向 EventLoop 投递无界任务。tick 取出后给每个观众的 RtcSession，后者调用 H264RtpPacketizer → RTCP SR/NACK 链 → DTLS-SRTP/ICE 传输。浏览器按协商解码并显示。
7. **stop/清理**：浏览器停止会 DELETE 自己的会话，关闭本地 PeerConnection；其他观众和 Publisher 继续运行。服务端令牌过期、连接失败或十秒仍未连接都会清理会话。进程结束时 loop 先返回 main，随后停止 HTTP/定时器、发出 source.stop 并关闭 RTC 会话，再在析构中 join 读取线程，最后释放信号通道和 EventLoop。
8. **重启**：浏览器可新建会话；上游重开增加 epoch，新会话或新段从 IDR 开始。SRS 也可能在同一 RTMP 订阅连接内接收重推并连续化时间戳，此时 epoch 不一定改变，不能把 epoch 改变作为恢复的唯一证据。

## 时间、边界与缓存

- 首版输入要求带 avcC 的 H.264、有效 PTS/DTS、无 B 帧；明确拒绝重排序输入和未知时间戳。实际 Main/Baseline/High 等常见 profile 按 RFC 6184 模式匹配，且输入 level 不得高于浏览器 offer；不伪造更低 level 或偷偷转码。若原始 1080p/4K 超过浏览器声明能力，先在 Publisher 降低输出尺寸，例如 1280×720，再播放。
- 输入 AVPacket 由 bitstream filter 转成独立的完整 Annex B access unit；`VideoUnit` 的字节用 shared_ptr 保活，释放原包不影响 RTC 发送。识别 NAL type 5，而非仅依赖 KEY 标记。
- RTP 视频时钟为 90kHz。每个观众从首个可用 IDR 建立输入 PTS 到会话时钟的映射；输入重开时重新锚定并保持向前推进。音视频共同时间线、音频转码延迟和完整时间跳变策略属于下一阶段。
- 共享交接队列限制 32 帧、8MiB、300ms 媒体跨度，tick 还会丢弃等待超过 300ms 的帧。裁剪后全部会话重新等待 IDR，避免接着发送依赖已丢帧的 P 帧。本阶段没有自适应逐观众缓冲或自定义 pacer。
- 入场直接等待下一个 IDR，没有重放旧 GOP；首屏等待取决于 Publisher 的关键帧间隔，不能承诺固定首屏延迟。
- 复用库的 512 包 NACK 响应缓存和 RTCP SR。这是已有库能力，尚未实现我们自己的重传期限、PLI→Publisher IDR 反馈、TWCC/GCC、FEC 或完整下行拥塞控制。Publisher 原有 ABR 只控制 RTMP 上行。
- 网关状态区分 inputFrames、提交给 RTC 的 submittedFrames 和传输字节；这些不能冒充浏览器已解码或实际显示。验收读取浏览器 inbound-rtp.framesDecoded，并校验画面内编号。

## 身份与部署范围

页面登录入口复用现有 AuthService 和只读挂载的签名密钥，现有登录服务器签发的令牌也可直接用于 API；当前仍是 root 开发账号。观众会话绑定登录用户并检查令牌过期，但还没有将媒体订阅权限与房间成员表联动，也不代表生产媒体鉴权已经完成。不要公开部署当前开发账号入口。

首版本机开发：HTTP 映射 127.0.0.1:8090，UDP 映射 127.0.0.1:8100–8109；与 SRS 的 1935/1985/8088/8000 分离。将容器收集到的 host candidate 地址改为配置中的宿主机可达 IPv4，端口保持真实映射。没有使用公网 STUN/TURN；跨机、NAT 穿透、HTTPS/TURN 部署与网络变化尚未验收。配置的 8 个观众是资源上限，没有做八路并发压测。

## 运行

先启动原来的 OBS 业务服务和 OBS-SRS，确保原签名密钥存在。仓库根目录的 Windows PowerShell 执行：

```powershell
& './OBS server/media.ps1' build
& './OBS server/media.ps1' start
& './OBS server/media.ps1' status
& './OBS server/media.ps1' logs
```

`build` 安装固定依赖并在挂载目录 `OBS server/code/build-media` 构建；首次下载需要时间，之后使用 Docker/CMake 缓存。`start` 仅启动新容器 OBS-MEDIA，不重建现有 OBS SSH 容器；停止新网关用 `& './OBS server/media.ps1' stop`。Linux 构建入口仍是原 `code/CMakeLists.txt`，可选开关 `OBS_BUILD_MEDIA=ON`，默认关闭以保持原业务服务的轻量依赖。

打开 <http://127.0.0.1:8090/>，用开发账号登录。在 Publisher 创建房间、开始直播，把推流 URL 最后一个 `/` 后面的流名称填入页面，点击“开始播放”。首版只支持一条共享上游流；同一流可加入多个观众，存在观众时拒绝切到另一条流。关闭浏览器播放不会停止 Publisher。

## 验证

沿用原有四组服务端 CTest，ProtocolTests 内增加异步 HTTP 延迟回复、重复回复/流水请求抑制、析构后回复，以及 H.264 AVCC→Annex B、参数集、原始 PTS、包释放后数据所有权和不支持输入验证；没有新增测试程序。

```powershell
docker compose -f 'OBS server/compose.yaml' --profile media run --rm --no-deps media ctest --test-dir build-media --output-on-failure
python -m pip install --target out/media-check-deps playwright==1.64.0
python SRS/check.py --gateway
```

网关模式扩展同一个 OBSStreamingTests/check.py：真实 MediaPublisher/NVENC 生成 320×180 编号画面和合成 AAC，通过原生 Edge headless 接收自建网关的 H.264。检查未登录拒绝、非法流名、上游不存在时 HTTP 响应、双观众、切源冲突、独立停止、上游重新推流、再次播放、画面编号校验和 400px 窄屏布局；不调用 SRS 的 RTC 协商接口，不采集私有桌面/音视频。

2026-10-18 最终构建已通过服务端四组（17.58秒）和客户端七组（20.66秒）回归。最终浏览器验证：现有登录服务器令牌可直接访问网关，不兼容 offer 返回406；主观众35帧、后加入观众30帧、另一人退出后剩余观众继续到50帧，均为320×180且统计 framesDropped=0，画面编号校验正确。Publisher 停止重推后画面编号172→1，原浏览器会话继续到82帧，证明画面来自新一轮推流；同一 SRS 订阅连接保持，epoch 未变化。浏览器再次播放收到30帧，最终会话表为空。缺失上游时健康接口约3.1ms。以上是短时本机功能结果，不是端到端延迟/互联网弱网/长期可靠性指标。最后一轮结果保存在 `out/媒体网关验证/媒体网关验收.json`。

已检查中文截图 `自建网关浏览器播放.png`、`自建网关窄屏布局.png`；停止容器约2.03秒（包含 Docker 命令开销），退出码0，没有被五秒期限强杀。没有执行综合弱网实验，仍按用户约定最后一起做。

## 接续顺序

下一阶段 AAC→PCM→48kHz Opus、音频编解码延迟与音视频 RTP/RTCP 时钟同步；之后做有时效的 GOP/重传缓存、PLI 到编码端、逐观众反馈和拥塞策略，再接原生 Player。基础链路补齐后，结合实际代码统一学习 WebRTC 信令流程、ICE/STUN/TURN、DTLS-SRTP、RTCP 反馈和拥塞控制。

参考：[SRS RTMP/WebRTC 桥接](https://ossrs.io/lts/en-us/docs/v6/doc/webrtc)、[libdatachannel 固定版本](https://github.com/paullouisageneau/libdatachannel/tree/v0.24.6)、[官方 H.264/Opus 发送示例](https://github.com/paullouisageneau/libdatachannel/tree/v0.24.6/examples/streamer)。
