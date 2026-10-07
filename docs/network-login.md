# 客户端网络与简单 MVC

当前实现和运行入口见 [两个客户端工作台](003-two-clients.md)。

| 类 | 职责 |
| --- | --- |
| HttpClient | 异步 HTTP GET/POST JSON；请求编号、超时、取消、响应限长 |
| LoginModel | 登录状态、消息、内存会话及到期时间 |
| MainWindow | View：登录输入、推流端/播放端工作台和直播/点播模式 |
| LoginController | 校验输入、发现登录节点、登录响应校验、取消、退出和显示同步 |
| SignalClient | 异步 TCP 帧收发、认证、心跳、关联响应和连接收敛 |
| SessionModel | 工作台连接状态、房间、列表和操作状态 |
| SessionController | 登录后的连接生命周期、房间流程、事件、分页与会话到期 |
| LocalAuthServer | 原登录测试的本机 fixture，业务程序不再自动启动 |

main.cpp 在一个 Qt 事件循环线程装配对象，两个目标共享 OBSNetwork 与 OBSClientCore 静态库。Controller 借用对象，在对象销毁前先销毁 Controller。HttpClient 拥有 QNetworkAccessManager；回复对象拥有其超时定时器。SignalClient 拥有当前 socket 和每请求定时器，重连时释放旧对象。

网络采用 [QNetworkAccessManager](https://doc.qt.io/qt-6/qnetworkaccessmanager.html) 和 [QAbstractSocket](https://doc.qt.io/qt-6/qabstractsocket.html) 的异步信号，不在 GUI 线程阻塞等待。取消/关闭先解除回调并停止定时器，旧结果不进入新会话。

登录流程为 GET /login/server → 校验 loginUrl → POST /auth/login → 内存会话 → TCP auth。直接填写登录节点时省略调度。协议见 [HTTP 登录契约](auth-login.md) 和 [服务端信令](server-services.md)。

注册、找回密码、自动续期、持久凭据和中心令牌撤销尚未实现。当前不增加云助教或远控功能。每类及每个流程的图见 [UML 索引](uml.md)。
