# 公共网络库与独立业务服务器

这是登录、负载调度、信令服务共用的 Linux C++17 网络基础库，目标名 `obs_net`，命名空间 `obs::net`。没有 Qt 依赖，也不复制 yzj 的业务包和裸指针管理方式。

## 目录

```text
code/
├─ CMakeLists.txt
├─ README.md
├─ .clang-format
├─ net/             公共网络类与 timerfd 定时器
├─ protocol/        HTTP 与长度头信令协议
├─ services/        认证、登录、调度、信令和公开配置
├─ tests/           同一工程的行为验证
└─ run_services.py  开发进程组启停
```

`build/` 和 `build-sanitize/` 为生成目录，Git 忽略。容器内源码路径为 `/workspace/code`。三个服务器共用本工程，完整协议与运行方法见 [服务端说明](../../docs/server-services.md)。

## 九个网络类与阅读顺序

| 顺序 | 类 | 职责 | 生命周期与线程 |
| --- | --- | --- | --- |
| 1 | UniqueFd | 移动式 fd 所有权，自动 close | 跟随持有它的对象 |
| 2 | Buffer | 保留未消费 TCP 字节，限制容量 | 由连接持有，loop 线程访问 |
| 3 | Channel | fd 关注事件、读写关闭回调、对象保活 | 不拥有 fd，loop 线程访问 |
| 4 | Poller | 封装 epoll 注册、移除和等待 | 由 EventLoop 持有 |
| 5 | EventLoop | 事件分发、任务队列、eventfd 唤醒 | 在运行它的线程构造和销毁 |
| 6 | Acceptor | IPv4 bind/listen/accept4 | Server 拥有，接入事件期间保活 |
| 7 | TcpConnection | 非阻塞收发、部分写、半关闭、状态与缓冲 | Server 持有活跃连接，事件/任务临时保活 |
| 8 | TcpServer | 监听入口、连接集合和业务回调 | 业务持有，必须先于 loop 销毁 |
| 9 | TimerQueue | 单调时钟、最早期限、到期批次与取消 | loop 持有，接口可跨线程投递 |

代码注释解释资源所有权、线程限制、错误路径和回调中的关闭问题。对应类图和连接接入、收发、关闭、跨线程任务时序图位于工程本地 `docs/uml/`，由根目录绘图脚本生成，图像中文命名。

## 核心约定

1. 单 Reactor：一个 EventLoop 在一个线程内执行，当前不自带线程池。将来可在此基础上添加多个 loop 和线程管理，先把连接生命周期稳定下来。
2. 监听、连接都使用非阻塞 fd，epoll 使用 LT。单轮接入上限 64，连接单轮读/写处理预算各 256 KiB。回调不做数据库查询、文件读取等阻塞工作。
3. `send()`、`shutdown()`、`forceClose()` 允许跨线程，调用方数据立即复制，任务捕获连接 shared_ptr。其他连接状态、缓冲、回调设置及 Server 操作只能在所属 loop 线程执行。
4. Channel 对业务对象保存 weak_ptr；分发前临时锁定。Poller 按注册 token 查找 Channel，避免直接保存裸指针。关闭后先移除订阅，再关闭 fd；一批活跃事件中已关闭的 Channel 会跳过。
5. TCP 是字节流：一次回调可能只有半个消息，也可能有多个消息。业务解析完整消息后消费 Buffer，不完整的数据留待下次。`protocol/` 中的 HttpParser 与 FrameCodec 分别实现 HTTP 和信令帧边界，网络层不承担业务协议。
6. 输入和输出缓冲分别限制 8 MiB，超限断开；send 出错使用 MSG_NOSIGNAL 避免进程因 SIGPIPE 退出。不可把未消费的 Buffer view 保存到回调以外。
7. 对端发送 EOF 时先交付最后一批输入，再停止读取；等待已排队输出发送完后关闭。主动 shutdown 会在排空输出后执行 SHUT_WR，继续等对端关闭；forceClose 立即断开。
8. Server 的 stop 先停止监听，再解除业务回调并强制关闭现有连接；不在停止过程继续通知断开。允许在接入/消息回调中调用 stop，回调副本与事件保活保证当前执行安全。
9. 接入遇到 fd 耗尽等非暂时错误会暂停监听并通知 ErrorCallback，避免 LT 空转。释放资源后上层可 stop/start 恢复；业务服务器选择报告并退出，开发启动器会停止整个进程组，尚未实现自动退避重启。
10. 退出顺序：先停止任务生产线程，再在 loop 线程停止 Server，调用 quit 结束循环，最后在归属线程销毁 loop。不能在 I/O 回调中销毁 loop。回调异常不被吞掉，会退出 loop 并传给调用者；业务回调应自行处理可恢复错误。
11. `runAfter`、`runEvery`、`cancelTimer` 可跨线程调用；重复采用回调完成后重新计时的固定延迟。取消通过原子标记阻止尚未执行的回调，不中断已经开始的任务。timerfd 与 eventfd 共同接入 epoll，没有独立定时器线程。

## 在 VS Code 远程终端构建

```bash
cd /workspace/code
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure -V
```

内存与未定义行为检查：

```bash
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOBS_ENABLE_SANITIZERS=ON
cmake --build build-sanitize -j 4
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-sanitize --output-on-failure -V
```

2026-10-07 在 OBS 容器的 GCC 13.3.0 下，普通 Debug 与 ASan/UBSan 构建均通过。测试包括原有 11 组网络行为、4 组定时器、3 组协议/签名令牌，以及两个登录进程、调度和信令之间的 10 个联调场景。详见 [验证说明](../../docs/server-services.md#验证)。

## 业务接入方式

业务通过 `target_link_libraries(目标 PRIVATE obs_net)` 使用公共库；协议层链接 obs_net，三个业务目标链接 obs_services。以下为底层接口说明，不另建 Demo：

```cpp
#include "EventLoop.h"
#include "TcpServer.h"

obs::net::EventLoop loop;
obs::net::TcpServer server(loop, "0.0.0.0", 8081);
server.setMessageCallback([](const obs::net::TcpConnection::Ptr& conn,
                             obs::net::Buffer& input) {
    // HttpServer 在同样的消息回调中调用 HttpParser；这里只说明字节回显接口。
    const std::string response(input.view());
    input.retrieveAll();
    conn->send(response);
});
server.start();
loop.loop();
```

## 当前边界与下一步

已实现定时器、HTTP 请求解析/服务、异步健康探测、信令长度头编解码，以及登录、调度和房间会话业务。业务层设置连接数/请求限长、HTTP 请求期限及信令认证/空闲期限。测试使用随机端口，不占用预留业务端口。
尚未实现线程池、通用主动连接 TcpClient、TLS、生产账号数据库、中心令牌撤销、媒体服务和压测；任务队列本身仍无容量上限。HttpProbe 是专用健康探测器，不是通用 HTTP 客户端。
下一步将 Qt MVC 接入独立登录、调度和信令，再继续采集与真实媒体推拉流。

技术依据：[epoll](https://man7.org/linux/man-pages/man7/epoll.7.html)、[eventfd](https://man7.org/linux/man-pages/man2/eventfd.2.html)、[send/MSG_NOSIGNAL](https://man7.org/linux/man-pages/man2/send.2.html)、[close/EINTR](https://man7.org/linux/man-pages/man2/close.2.html)、[shutdown](https://man7.org/linux/man-pages/man2/shutdown.2.html)。
