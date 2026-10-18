# SRS 独立副项目

SRS负责媒体接收、分发与协议桥接，OBS负责采集处理、编码、播放及业务信令。这个目录集中放SRS部署、上游版本、集成验证和二开路线；OBS的通用RTMP代码仍放在`OBS client/ffmpeg/`，不包含SRS内部类型。

2026-10-18：Windows OBS 增加[自建媒体网关](../docs/021-media-gateway.md)。在该路径中SRS只接收RTMP，独立OBS-MEDIA通过FFmpeg拉取并自行适配H.264、经libdatachannel送浏览器，不调用SRS的RTC输出。`python SRS/check.py --gateway`是同一集成入口的可选验证；Mac上的SRS二开仍独立。原有SRS→Player路径继续使用。

最新网关已支持[H.264+Opus与音画同步](../docs/022-gateway-av-sync.md)：AAC在自建进程转码、同一个RTC会话发送，SRS仍只做RTMP接入。同一验证入口扩展合成音画包络和重推同步检查。

## 文件与Git归属

| 文件 | 用途 |
| --- | --- |
| `compose.yaml` | 独立`OBS-SRS`容器；固定官方6.0.193镜像摘要 |
| `srs.conf` | RTMP、HTTP-FLV、管理API和原生RTMP→WebRTC配置 |
| `manage.ps1` | Windows端启动、停止、状态、日志 |
| `source.lock.json` | 上游源码提交和运行镜像版本记录 |
| `prepare_source.ps1` | 获取固定源码并建立`obs-bridge`分支，保留已有工作 |
| `upstream/` | SRS原生C++源码及自己的Git历史；由准备脚本生成，OBS仓库忽略 |
| `IntegrationTests.cpp`、`check.py` | OBS→SRS→Player生成媒体集成验证，要求NVIDIA设备 |

主仓库提交这个目录的接入文件。SRS源码内的修改在`upstream/`独立提交，后续可为其配置用户自己的SRS fork远端。当前源码与镜像版本均为6.0.193，具体提交见版本锁。

```powershell
# 在CourseStudioNext根目录
./SRS/prepare_source.ps1
git -C SRS/upstream status
git -C SRS/upstream log -1 --oneline
```

VS Code远程打开`/repo`时，源码位于`/repo/SRS/upstream/trunk`，本机对应`C:\projects\CourseStudioNext\SRS\upstream\trunk`。SRS二开提交可在此目录的终端执行`git diff`、按文件`git add`、`git commit`；OBS提交在`/repo`根目录执行。没有为SRS另建GitHub仓库或设置新远端。

## 独立启停

```powershell
./SRS/manage.ps1 start
./SRS/manage.ps1 status
./SRS/manage.ps1 logs
./SRS/manage.ps1 stop
```

Linux远程也可以执行`docker compose -f /repo/SRS/compose.yaml ...`，前提是当前环境能访问Docker daemon；OBS开发容器默认未挂载Docker socket。本机PowerShell可直接管理Docker Desktop。

| 宿主地址 | 用途 | 容器 |
| --- | --- | --- |
| `127.0.0.1:1935/tcp` | RTMP推流/拉流 | OBS-SRS |
| `127.0.0.1:1985/tcp` | 管理API、WHEP协商 | OBS-SRS |
| `127.0.0.1:8088/tcp` | HTTP-FLV、自带测试页面 | OBS-SRS |
| `127.0.0.1:8000/udp` | WebRTC媒体 | OBS-SRS |

`OBS server/compose.yaml`已经移除原来的三个媒体预留映射。旧环境首次迁移要重建OBS容器，SSH会短暂断开，挂载目录和命名卷保留：

```powershell
docker exec OBS python3 /workspace/code/run_services.py stop
docker compose -f "OBS server/compose.yaml" up -d --build obs
docker exec OBS python3 /workspace/code/run_services.py start
./SRS/manage.ps1 start
```

SRS服务与OBS业务服务各自启停；停止SRS不会停止SSH/登录/信令。SRS健康接口：`http://127.0.0.1:1985/api/v1/versions`。当前不启用HLS切片。

## OBS使用

1. 登录两个客户端，Publisher创建房间。
2. Publisher选择视频/麦克风/系统音频，点击房间卡片的“开始直播”，它会启动本轮采集。已在预览采集时先停止，再开始直播，保证统一PTS原点。
3. 可同时勾选本地录制；直播和录制各有一个输出线程/编码器，本步优先保持生命周期独立，之后可评估共用编码包。
4. Player在直播模式刷新并加入房间。未开播时等待，开播事件到达后自动拉流；失败可点“播放直播 / 重试”。声音/音量为两种模式共用控件。
5. 主播“停止直播”停止采集及直播，同时封存本轮MP4；观众“停止播放”仅停止自己的媒体，房间仍保留。关闭房间、退出登录、模式切换均清理相应媒体。

OBS信令配置继续使用`OBS server/code/services/config.json`：`rtmpBase=rtmp://127.0.0.1:1935/live`、`playbackBase=http://127.0.0.1:8088/live`。房间创建响应只向主播返回`pushUrl`，观众拿`pullUrl`和`playbackUrl`。媒体写出后才发`live.start`；这个状态来自受控客户端，尚未用SRS回调校验外部推流。

## 副项目二开范围

当前运行官方镜像，原生RTMP→WebRTC桥接已配置开启。OBS Player的WebRTC会话/传输接入与实际RTC播放验收仍为后续步骤。

固定源码中优先阅读：

- `trunk/src/app/srs_app_source.cpp`：RTMP媒体源、消费者与GOP缓存。
- `trunk/src/app/srs_app_rtc_source.cpp`：`SrsRtcRtpBuilder`、音频转码、RTP打包、收发轨道与重传缓存。
- `trunk/src/app/srs_app_rtc_conn.cpp`：RTC连接/播放流程、RTCP NACK/PLI反馈。
- `trunk/conf/rtmp2rtc.conf`、`trunk/conf/full.conf`：桥接与低延迟配置基线。

先测原生桥接基线，再逐项增加：时间戳跳变/重连处理、有限年龄的可解码首屏缓存、按播放期限的重传策略、关键帧请求经OBS信令触发NVENC IDR。保留SRS的SDP/ICE/DTLS/SRTP实现；视频H.264在协商兼容时重打包，AAC转Opus。GOP缓存与RTP重传缓存分别设计，记录首屏、音画偏差、端到端延迟、缓存内存与丢包恢复。

## 验证与边界

```powershell
./scripts/build_clients.ps1
python SRS/check.py
```

2026-10-12：SRS配置检查、API健康、OBS服务端4组和客户端8组回归通过。生成GPU纹理经NVENC H.264/AAC→RTMP→SRS→Player MVC，连续两轮每轮4.5秒；最终回归显示124/123帧，声音输出音量0、时钟约4.24秒、补静音0次。停播线程退出156/118ms；无响应RTMP握手推流取消90ms、拉流取消78ms。这些是本机功能验证数据，生产环境延迟基准另行测量。

当前为本机开发部署，所有映射绑定127.0.0.1，`CANDIDATE`默认127.0.0.1。跨机部署需同时调整媒体基地址、端口绑定和客户端可达Candidate。RTC浏览器播放、ICE穿透、弱网重传/拥塞、长期漂移、生产推拉流鉴权尚未验收；房间权限与SRS媒体权限后续通过鉴权/回调联动。

还验证了实际Publisher按钮启动：只捕获测试自建窗口，WGC→MediaTimeline→NVENC→SRS→后加入的Player显示，直接关闭直播房间后两端媒体与采集完整退出。

详细客户端生命周期见[第012步](../docs/012-srs-live.md)。官方参考：[仓库](https://github.com/ossrs/srs)、[WebRTC与RTMP桥接](https://ossrs.io/lts/en-us/docs/v6/doc/webrtc)、[Docker部署](https://ossrs.io/lts/en-us/docs/v6/doc/getting-started)。
