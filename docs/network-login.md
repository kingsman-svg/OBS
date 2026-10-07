# 首期网络与登录：简单 MVC

## 类与职责

| 类 | 职责 | 所在文件 |
| --- | --- | --- |
| HttpClient | 异步 HTTP JSON POST、请求编号、超时、取消、响应大小限制与结果通知 | `OBS client/HttpClient.*` |
| LoginModel | 未登录/登录中/已登录状态、消息、内存会话和过期时间 | `OBS client/LoginModel.*` |
| MainWindow | View：输入接口地址、账号和密码；显示状态；发出登录/取消/退出意图 | `OBS client/mainwindow.*` |
| LoginController | 校验输入、发起登录、检查响应契约、更新 Model、同步 View | `OBS client/LoginController.*` |
| LocalAuthServer | 本地开发 HTTP 服务；异步接收、协议校验、root 账号验证与临时令牌生成 | `OBS client/LocalAuthServer.*` |

对象由 `main.cpp` 装配，在同一 Qt 事件循环线程中使用。Controller 借用 View、Model 和 HttpClient，不拥有它们；声明顺序保证 Controller 最先销毁。
HttpClient 拥有 QNetworkAccessManager，请求结束或取消后释放 QNetworkReply；超时 QTimer 由回复对象拥有。

## 当前完成的流程

1. HTTP 请求及响应/超时处理。
2. 输入 → 登录请求 → 结果校验 → 状态展示，包含失败分支。
3. 取消当前登录，恢复未登录状态。
4. 本地退出登录，清空内存会话。

每个类与流程的 UML 可从 [图索引](uml.md) 查看。
HttpClient 使用 QNetworkAccessManager 的异步 API，符合当前简单 UI 的使用方式。[Qt 官方文档](https://doc.qt.io/qt-6/qnetworkaccessmanager.html)

## 本步边界

应用现自动启动 LocalAuthServer（127.0.0.1 随机端口），客户端通过 HTTP 校验 root / root，成功显示主页，退出返回登录页。MainWindow 的两个页面只负责展示，切换仍由 Controller 同步 Model 状态驱动。
这是本地开发服务；账号数据库、生产密码存储、令牌验证和服务端会话撤销尚未实现。
新 HTTP 登录不依赖 yzj 的 LoginServer、SigServer 或 LoadBanceServer，也不迁入云助教或远控业务。
注册、找回密码、会话续期在需要时逐步设计。

## 阅读顺序

先读 HttpClient 及其类图/请求时序，再读 LoginModel、MainWindow、LoginController；最后对照登录、取消和退出时序图跟踪一次实际调用。
