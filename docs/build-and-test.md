# 构建与验证

## Qt Creator

打开 `CSN/CMakeLists.txt`，选择 Qt 6 + MSVC 64 位 Kit，构建并运行 `CSN`。
本机实际使用 Qt 6.11.2、MSVC 2022 和 C++17。保留了用户原有 Qt Creator 构建目录，命令行验证使用 `CSN/build-agent`。
当前界面默认指向本机 `/auth/login`；需要启动符合 [接口契约](api/auth-login.md) 的服务才能实际登录。

## 命令行构建

在已初始化 MSVC 编译环境的 Developer PowerShell 中执行：

```powershell
cmake -S CSN -B CSN/build-agent -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/software/Qt/6.11.2/msvc2022_64 -DCMAKE_MAKE_PROGRAM=C:/software/Qt/Tools/Ninja/ninja.exe
cmake --build CSN/build-agent --parallel 4
```

根据本机安装位置调整 Qt 和 Ninja 路径。`BUILD_TESTING=OFF` 可关闭测试目标，但正常开发建议保留。

## 行为验证

```powershell
python scripts/run_checks.py --qt-root C:/software/Qt/6.11.2/msvc2022_64
```

脚本仅为测试子进程设置 Qt DLL/插件路径，使用 offscreen 平台运行界面验证；不修改系统或全局环境。
`CSNLoginTests` 是本工程的测试目标，既有网络测试使用动态本机端口的 HTTP fixture；root 登录测试使用实际 LocalAuthServer 开发服务。业务应用 CSN 启动时也会自动启动该服务并填入地址。登录账号 root，密码 root。

测试覆盖：

- 并发请求编号与结果关联、完整 POST JSON、分段响应。
- 无效地址、无效 JSON、401 响应、确定性的请求超时。
- 取消后无结果回调、响应大小限制。
- 点击界面按钮完成 MVC 登录与本地退出，密码输入清空、会话与按钮状态更新。
- 无效会话响应不能登录成功，以及取消后的状态恢复。

界面截图保存到 `out/login-ui.png`，构建产物和临时文件均被 Git 忽略。
实际构建与测试结果另见 [本步记录](steps/001-http-login.md)。

## UML

```powershell
python scripts/render_uml.py
```

绘图脚本需要 Pillow 和 Windows 微软雅黑字体；输出 Mermaid 源码对应的 SVG/PNG 图像。
