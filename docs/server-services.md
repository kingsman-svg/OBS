# Linux 定时器、登录、调度与信令

本阶段在同一个 CMake 工程中实现三类独立服务器。登录启动两个实例验证调度，业务进程不依赖 Qt。源码位于 `OBS server/code/`，没有增加 Demo 工程。

## 目录与阅读顺序

```text
code/
├─ net/             通用 Reactor、TCP 连接和定时器
├─ protocol/        HTTP 请求、服务端、健康探测、信令帧
├─ services/        认证、登录、调度、信令、入口和公开配置
├─ tests/           网络、定时器、协议和真实进程联调
├─ CMakeLists.txt
├─ run_services.py  开发进程组启停
└─ README.md
```

先读 `EventLoop` / `TimerQueue`，然后 `HttpParser` / `HttpServer`、`AuthService` / `LoginServer`、`HttpProbe` / `SchedulerServer`，最后读 `FrameCodec` / `SignalServer`。注释着重解释状态、所有权、系统调用约束和失败路径，不逐行翻译语句。

## 构建与启动

在 OBS 容器的 VS Code 终端中：

```bash
cd /workspace/code
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure -V
python3 run_services.py start
python3 run_services.py status
```

```bash
python3 run_services.py stop
# 前台启动，用 Ctrl+C 停止整个进程组
python3 run_services.py run
```

后台日志在 `build/run/services.log`，进程身份记录、编译结果和日志均被 Git 忽略。启动器在 `/root/.config/obs/token-secret` 生成并保留随机签名密钥（权限 0600），root 持久化卷会保留它。可用 `OBS_TOKEN_SECRET` 覆盖；登录和信令必须使用同一密钥。密钥不进入公开配置、启动参数或日志。

启动器只管理自己创建的进程，任一子进程异常退出则停止整个组。容器重启后需要重新执行 `start`，当前容器入口仍只启动 SSH。它是开发工具，生产阶段再接入服务管理器。

新镜像的 Dockerfile 已加入 `nlohmann-json3-dev`，依赖还有 OpenSSL、CMake、GCC 和 Python3。现有 OBS 容器也已安装依赖。

## 服务与接口

| 进程 | 端口 | 接口/职责 |
| --- | --- | --- |
| obs_scheduler | 8080 | `GET /login/server`、`GET /health` |
| obs_login 实例 1 | 8081 | `POST /auth/login`、`GET /health` |
| obs_login 实例 2 | 8082 | 同上，共享签名配置 |
| obs_signal | 9000 | TCP 长度头 JSON：认证、房间、直播、心跳 |

监听地址默认 `0.0.0.0`；Windows Docker 映射已限制在 `127.0.0.1`。开发配置中的公开 URL 也使用 `127.0.0.1`，部署到其他机器时应修改 `services/config.json` 中的 publicUrl 和媒体 URL。探测地址用于容器内部连接，与返回给客户端的地址分开。

单独启动二进制时需先设置 `OBS_TOKEN_SECRET`（至少 32 字符），登录密码可用 `OBS_LOGIN_PASSWORD` 覆盖，默认开发账号为 root / root。例如，在启动器之外已经配置好环境后：

```bash
./build/obs_login --config services/config.json --port 8081
./build/obs_scheduler --config services/config.json
./build/obs_signal --config services/config.json
```

三个进程通过 signalfd 接收 SIGINT/SIGTERM，在事件循环退出后取消定时器、停止监听和连接，正常释放资源。

## 定时器契约

`EventLoop::runAfter(delay, callback)` 返回一次性句柄；`runEvery(interval, callback)` 返回重复句柄；`cancelTimer(id)` 幂等取消。三者都允许跨线程，调用线程必须在 EventLoop 销毁前停止。

- 一个 loop 共用一个 `CLOCK_MONOTONIC` timerfd，不受系统时间校准影响。
- 队列按到期时间排序，只向内核设置最早期限。到期先摘出本批任务，再调用业务，允许回调新增定时器或取消同批任务。
- 零延迟表示下一次分发；负延迟、空回调、非正重复间隔及超过一年的时长会被拒绝。
- 重复采用固定延迟：回调完成后再等待 interval，不补发阻塞期间错过的次数。
- 取消先设置原子标记，再在 loop 中清理。可取消尚未插入、已在本批但尚未执行和正在执行的重复任务；已经开始的回调不会被中断。
- 回调异常按 EventLoop 约定向上传播，本批剩余任务取消。业务应自行处理可恢复错误。

HTTP 使用绝对请求期限和排空后的关闭兜底；健康探测使用 800ms 期限；信令每秒清理认证、空闲和过期会话。

## HTTP 边界

使用 HTTP/1.1、Content-Length 和每连接一次请求，不使用持久连接或请求流水线。头部最大 16 KiB、正文最大 64 KiB、最多 100 个头。拒绝重复头、缺少 Host、折行、非法长度、Transfer-Encoding 和 Expect，避免不同解析方式之间的歧义。

请求不完整时保留 Buffer，不消费半包；从接入起五秒未完成直接断开，不因零碎字节续期。处理后发送 JSON 与 `Connection: close`，排空再半关闭，并用一秒兜底清理不配合关闭的连接。每个 HTTP 服务最多保留 1024 条连接。

## 登录与令牌

登录请求和成功/失败响应继续符合 [HTTP 登录契约](auth-login.md)。当前真实 Linux 开发服务器执行 root 账号校验，默认密码 root，可配置覆盖；没有数据库、注册和完整账号管理。

认证服务给每次登录分配安全随机会话标识，并用 HMAC-SHA256 签署版本、用户和 Unix 到期时间。凭据是自定义 v1 格式，不是 JWT，客户端仍视为不透明字符串。默认一小时到期；登录节点和信令通过共享密钥独立验证，不需要把密码发给信令服务。

内存开发密码仅做摘要后的恒时比较，这不是生产密码存储方案。令牌当前无中心撤销表，因此客户端退出不会立即使其他连接上的同一令牌失效；到期由信令请求校验与定时清理执行。登录节点每秒最多 100 次尝试，属于节点级开发限流。

Qt 客户端默认仍启动原来的 LocalAuthServer。可把登录页面地址手动改为 `http://127.0.0.1:8081/auth/login` 使用独立 Linux 服务；客户端自动调度和信令 UI 接入留到下一步，不改动当前 MVC 类。

## 登录节点调度

客户端先 `GET http://127.0.0.1:8080/login/server`：

```json
{"nodeId":"login-1","loginUrl":"http://127.0.0.1:8081/auth/login"}
```

再向返回的 loginUrl 直接发送登录请求。调度器不代理账号密码。启动时与每两秒，HttpProbe 非阻塞请求各节点 `/health`，仅接受 200 且 `service=login, ready=true` 的限长响应；连接失败、响应错误和 800ms 超时均标记不可用。

健康节点之间轮询；首次探测完成前或全部不可用时返回 503 `NO_LOGIN_NODE`。节点恢复后重新参与。检测存在最多约 2.8 秒窗口，客户端在分配之后节点突然下线时仍需重新调度。本阶段不声称实现实时负载采样或分布式调度器高可用。

## 信令协议

每帧为 `4 字节网络序无符号长度 + UTF-8 JSON`，正文长度 1～65536 字节。一次读可解析多个完整帧，半包保留；非法长度关闭连接。每连接单批最多 64 帧，超出且仍有积压则断开；已解析业务每秒最多 100 次请求。

请求的 id 是 1～64 字节字符串，用于关联响应。未认证只允许 auth；默认五秒内未完成认证会断开。

```json
{"id":"1","type":"auth","token":"登录返回的不透明凭据"}
{"id":"2","type":"room.create","title":"我的直播间"}
{"id":"3","type":"room.join","roomId":"创建返回的房间标识"}
{"id":"4","type":"live.start"}
{"id":"5","type":"heartbeat"}
```

| type | 字段 | 行为 |
| --- | --- | --- |
| auth | token | 校验签名与到期时间，绑定当前连接 |
| heartbeat | 无 | 更新时间并返回 serverTime |
| room.list | offset，可省略 | 每页最多 32 个房间，返回 nextOffset，末页为 null |
| room.create | title | 创建并加入房间；每连接最多一个房间，全局最多 128 个 |
| room.join | roomId | 加入房间，返回状态及媒体地址 |
| room.leave | 无 | 观众退出；主播退出则关闭房间 |
| live.start | 无 | 仅创建房间的连接可调用，返回 pushUrl 并广播状态 |
| live.stop | 无 | 仅主播可调用，更新状态并广播 |

成功响应：`{"id":"1","type":"response","ok":true,"data":{...}}`。
失败响应：`{"id":"1","type":"response","ok":false,"error":{"code":"...","message":"..."}}`。无有效 id 的错误响应使用 null，不回显超长或嵌套内容。
事件：`{"type":"event","event":"live.changed","data":{...}}` 或 `room.closed`。事件可能在请求响应之前到达，客户端应按 type 与 id 分发。

认证成功响应给出建议 heartbeatSeconds。默认 45 秒无活动、令牌过期或连接断开都会复用清理路径；主播离开时先删除房间、清空成员关系，再向观众广播。多个 root 客户端可以联调，但主播权限属于创建连接，观众不能借用同一账号停播。

信令最多 1024 个连接；房间、在线状态仅在进程内存中，重启会清空。本阶段的 live.start/stop 是控制状态，不确认真实媒体流是否已经上线。RTMP/HTTP-FLV 地址是约定，1935/8088 的媒体服务器还未部署，也未实现媒体鉴权；推拉地址不能当作已经受保护的媒体权限。

## 验证

```bash
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOBS_ENABLE_SANITIZERS=ON
cmake --build build-sanitize -j 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-sanitize --output-on-failure -V
```

验证包括：原有 11 组网络行为、4 组定时器、3 组协议/令牌以及 10 个真实进程联调场景。集成测试启动两个登录节点、调度和信令，使用随机端口，检查密码错误、请求错误、轮询与下线/恢复、半包/粘包、主播/观众权限、房间离开/断开、认证与空闲期限、请求超时、限流和超长 id。测试日志不打印令牌或认证正文。

2026-10-07：OBS 容器内 GCC 13.3.0 的 Debug 和 ASan/UBSan 构建均通过全部四项 CTest；启动器的 start/status/stop/再次启动通过。Windows 本机经 8080 调度、8081/8082 登录和 9000 信令认证的实际映射链路通过。Qt GUI 本次未重新构建，媒体端口尚无服务。

每个新增 C++ 类及数据结构均有类图，定时器、HTTP、登录、调度、信令及启停流程都有时序图，详见 [UML 索引](uml.md)。

技术依据：[timerfd](https://man7.org/linux/man-pages/man2/timerfd_create.2.html)、[HTTP/1.1 消息边界](https://www.rfc-editor.org/rfc/rfc9112.html)、[OpenSSL HMAC](https://docs.openssl.org/3.0/man3/HMAC/)。
