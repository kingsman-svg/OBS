# 构建与验证

Qt Creator 打开 OBS client/CMakeLists.txt，选择 Qt 6 + MSVC 64 位 Kit。重新配置后有两个运行目标：OBS_Publisher、OBS_Player。程序连接独立 Docker 服务，启动与双端操作见 [第 003 步](003-two-clients.md)。

## Windows 构建和部署

仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_clients.ps1 -Deploy
```

脚本自动通过 vswhere 查找 MSVC，默认 Qt 为 C:/software/Qt/6.11.2/msvc2022_64，支持 -QtRoot 和 -BuildDir。构建 Debug，并用 windeployqt 将两个程序所需的 Qt DLL、插件复制到忽略的构建目录。只影响子进程环境。

音频处理依赖 FFmpeg 7.x x64 shared SDK，默认使用 CourseStudio 的 `C:/dev/ffmpeg-7.1-full_build-shared`。更换路径时在 Qt Creator CMake 配置设置 `OBS_FFMPEG_ROOT:PATH`；首次配置也可设置环境变量 `FFMPEG_7_HOME`。当前 Ninja 单配置构建由 CMake 复制 swresample-5.dll / avutil-59.dll 到可执行文件目录，不提交 SDK 或 DLL。详见 [第 006 步](006-audio-mix.md)。

实时人脸检测增加 TensorRT 11 runtime / CUDA 13 SDK，使用 `TENSORRT_ROOT`、`CUDA_PATH` 或 `OBS_TENSORRT_ROOT`、`CMAKE_CUDA_COMPILER`；CMake 至少3.24。默认针对 RTX 5060 的 SM120 编译，构建部署匹配运行库与本机存在的示例 engine。OBS 不依赖 OpenCV/ORT/parser；`-FaceReferenceTests` 仅对照测试需要 OpenCV，详见 [第 008 步](008-gpu-face.md)。

需要真实服务器联调测试时，先启动 Docker 业务服务，再在构建命令中增加 -LiveTests。未启用时测试仅使用动态本机端口，不依赖外部服务。Qt 的 Windows 部署工具用法见 [官方说明](https://doc.qt.io/qt-6/windows-deployment.html)。

已有 MSVC 开发终端也可直接使用 CMake：

```powershell
cmake -S "OBS client" -B "OBS client/build-agent" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/software/Qt/6.11.2/msvc2022_64 -DCMAKE_MAKE_PROGRAM=C:/software/Qt/Tools/Ninja/ninja.exe -DOBS_LIVE_TESTS=ON
cmake --build "OBS client/build-agent" --parallel 4
```

BUILD_TESTING=OFF 可关闭测试，OBS_LIVE_TESTS 默认 OFF。Qt Creator 原构建目录保留；可重新配置运行目标，无需 .pro。

## 行为验证

```powershell
$env:PATH = "C:/software/Qt/Tools/CMake_64/bin;" + $env:PATH
python scripts/run_checks.py --qt-root C:/software/Qt/6.11.2/msvc2022_64
```

脚本给测试子进程设置 DLL/插件路径，网络与界面测试使用 offscreen，采集测试使用 Windows 原生平台，用实际按钮验证 MVC。测试会加载 Windows 微软雅黑；offscreen 的默认字体目录警告不影响本项目中文截图。

| CTest | 内容 |
| --- | --- |
| login.http_mvc | 原 7 组 HTTP、MVC、取消、限长、本地认证测试 |
| client.protocol_mvc | TCP 分片粘包、并发编号、事件、心跳、非法帧、请求超时、重连、HTTP 调度和取消 |
| client.capture | Windows 原生平台；PCM 格式、无效目标重复启停、设备枚举响应、D3D11 WARP 交换链、共享上下文状态恢复、设备切换、独立窗口生命周期、小窗口布局和退出登录；不打开有效采集源 |
| client.audio | 六组人工音频测试；重采样/抗混叠、声道布局、时间戳与跳变、双路对齐、补零、音量/静音、削波、有界缓存与停止排空；不打开设备 |
| client.face | SCRFD anchor、NMS、逆缩放、错误张量、缺失引擎、停止等待首帧、重启与清空邮箱；不打开有效采集源 |
| client.live_services | 可选；Windows Qt → Docker 调度/登录/信令；两个角色与关闭/断线/过期清理 |

LocalAuthServer 只编译进 CSNLoginTests，不进入业务可执行程序。真实集成测试会短暂创建自己的测试房间，完成后关闭连接，由服务器清理；不要与同账号正式演示混用（当前仅开发账号）。

截图保存到 out/，完整测试还生成 `out/音频重采样混音验证.wav`（人工正弦波、48kHz 双声道 PCM16、1 秒）。构建目录与临时文件被 Git 忽略。Linux 服务测试及 ASan/UBSan 命令见 [服务端说明](server-services.md)。

## UML

```powershell
python scripts/render_uml.py
```

需要 Pillow 与 Windows 微软雅黑。按最新约定，脚本默认只生成时序图，每张同时有中文名 SVG/PNG；历史类图不再更新。渲染后检查新增图像，源码、图与业务行为保持一致。

采集使用 C++20、Windows SDK 和 d3d11/dxgi/d3dcompiler/windowsapp/ole32/uuid。硬件验证由 python scripts/run_capture_checks.py --hardware 显式执行，详见 [采集说明](004-capture.md)；不放入默认 CTest，避免常规测试打开设备。

GPU 原生窗口测试需要正常 Windows 交互图形会话。受限沙箱或非交互会话可能没有 expose/paint 事件，不能用 offscreen 代替 SwapChain 验收。`--window-preview` 只捕获测试自建色块和预览窗口，验证四象限方向/颜色、等比留边、ResizeBuffers、设备切换、恢复、真实 WGC 到 GPU 预览与退出；详见 [第 005 步](005-gpu-preview.md)。

`python scripts/run_capture_checks.py --audio-mix` 显式验证默认输出设备的 WASAPI 回环 → 重采样 → 混音信号、停止、重启与退出；短暂播放人工零值音频，不打开麦克风、不保存采集声音。没有默认端点时明确 SKIP，不计为硬件通过。`--ui-only` 可单独验证音量/静音控件、混音统计和长错误文本布局，支持 `QT_SCALE_FACTOR=2`；各专项参数互斥。
