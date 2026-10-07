# 第 001 步：HTTP 登录客户端与 MVC

日期：2026-10-06。

## 本步目标与结果

以用户创建的 CSN Qt Widgets 工程为起点，实现新 HTTP 登录接口的客户端闭环。
完成 HttpClient、LoginModel、MainWindow（View）、LoginController 四个类；提供四张类图和 HTTP 请求、登录、取消、本地退出四张时序图。

需求已明确：单工程、Git 管理、简单 MVC、逐类/逐流程维护 UML，当前不增加云助教相关功能。

## 实际验证

- 使用 Qt 6.11.2 + MSVC 2022，以 C++17 / Debug 构建 CSN 和 CSNLoginTests，通过。
- CTest `login.http_mvc` 通过；内部五组行为检查全部通过：
  `httpSuccessAndIdentity`、`httpErrorsAndTimeout`、`cancelAndResponseLimit`、`mvcLoginAndLogout`、`mvcFailureAndCancel`。
- 测试使用本机动态端口的 HTTP 服务，覆盖并发结果关联、完整 JSON 请求、分段响应、401、无效 JSON、超时、取消和响应大小限制。
- 通过实际界面按钮验证 MVC 登录和退出；会话数据、密码输入框和按钮状态符合预期。
- 已检查界面截图和八张 UML 预览图，确认中文显示、布局和关系箭头。

测试最初遇到子进程运行库加载环境异常。系统 ICU 可正常加载，同版本官方 Qt Core 与本机文件一致。
最终使用 `scripts/run_checks.py` 显式配置测试子进程的 Qt DLL/插件环境完成验证，无需安装或修改系统运行库。
offscreen 平台输出的字体目录与 size-hint 提示不影响验证；测试显式加载本机字体用于截图。

## 本步限制

- 实际账号存储、密码验证和服务端会话签发尚未实现。
- 测试成功响应只验证客户端协议与流程，不代表已接入真实账号认证。
- 退出只清除客户端内存会话；服务端撤销、续期、持久化及受保护 API 尚未实现。
- 当前保存过期时间，尚未增加过期触发的 UI 状态更新。
- 无云助教或课程分析业务。

## 下一步

按照 `docs/auth-login.md` 实现实际认证服务，再验证真实账号的成功、错误密码和会话生命周期。
继续在同一工程/仓库逐步推进；新增类与完成的流程同步补充 UML。
