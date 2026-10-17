# 构建与验证

Qt Creator 打开 OBS client/CMakeLists.txt，选择 Qt 6 + MSVC 64 位 Kit。重新配置后有两个运行目标：OBS_Publisher、OBS_Player。程序连接独立 Docker 服务，启动与双端操作见 [第 003 步](003-two-clients.md)。

## Windows 构建和部署

仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_clients.ps1 -Deploy
```

脚本自动通过 vswhere 查找 MSVC，默认 Qt 为 C:/software/Qt/6.11.2/msvc2022_64，支持 -QtRoot 和 -BuildDir。构建 Debug，并用 windeployqt 将两个程序所需的 Qt DLL、插件复制到忽略的构建目录。只影响子进程环境。

音频处理与编码封装依赖 FFmpeg 7.x x64 shared SDK，默认使用 CourseStudio 的 `C:/dev/ffmpeg-7.1-full_build-shared`。更换路径时在 Qt Creator CMake 配置设置 `OBS_FFMPEG_ROOT:PATH`；首次配置也可设置环境变量 `FFMPEG_7_HOME`。当前 Ninja 单配置构建由 CMake 复制 swresample-5.dll / avutil-59.dll / avcodec-61.dll / avformat-61.dll 到可执行文件目录，不提交 SDK 或 DLL。详见 [音频处理](006-audio-mix.md) 与 [编码录制](010-ffmpeg-encode.md)。库目标为 OBSMediaCore、OBSClientUi、OBSFFmpegCore；Qt Creator 重新配置即可，不需要删除源码或改类名。

实时人脸检测增加TensorRT 11 runtime / CUDA 13 SDK，使用`TENSORRT_ROOT`、`CUDA_PATH`或`OBS_TENSORRT_ROOT`、`CMAKE_CUDA_COMPILER`；CMake至少3.24。默认针对RTX 5060的SM120编译，部署匹配运行库与已有示例engine。OBS不依赖OpenCV/ORT/parser，独立CPU对照测试及其构建开关已移除。

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

脚本给测试子进程设置DLL/插件路径，网络与界面使用offscreen；播放和推流控制使用Windows原生平台。测试加载Windows微软雅黑，offscreen默认字体目录警告不影响中文截图。

2026-10-16按用户要求精简：删除独立采集、硬件解码、人脸、美颜阶段测试与模型优化CLI阶段脚本，配套启动脚本及OpenCV对照构建选项一并移除。默认保留下面7组核心回归；阶段文档保留历史验收记录，当前运行入口以本页为准。旧专项可从Git历史恢复。

精简后两个客户端重新配置、构建及部署通过，7组CTest全部通过（18.35秒）；已清理旧测试目标的12个EXE/PDB/ILK编译产物。服务端测试和现有SRS集成入口继续保留。

| CTest | 内容 |
| --- | --- |
| login.http_mvc | 原 7 组 HTTP、MVC、取消、限长、本地认证测试 |
| client.protocol_mvc | TCP 分片粘包、并发编号、事件、心跳、非法帧、请求超时、重连、HTTP 调度和取消 |
| client.audio | 六组人工音频测试；重采样/抗混叠、声道布局、时间戳与跳变、双路对齐、补零、音量/静音、削波、有界缓存与停止排空；不打开设备 |
| client.media_sync | 共同原点、两路PTS、120ms总等待、静态复用、6000次合成闪光/脉冲、有界追赶、停止/退出与重启 |
| client.encode | AAC/MP4软件解码验收：左右声道、缺口补零、尾长、单调DTS、中文路径、文件不覆盖、失败/abort/重启、迟到输入和有界队列；WARP验证编码GPU输出，不打开有效采集源 |
| client.publish_control | 队列数量/时间、阻塞期间完整参数响应、手动/自动范围、ABR人工时钟策略、过期裁剪/共同截止点、AAC主动追赶、停止/重启与界面；不打开有效采集源 |
| client.playback | 合成MP4/M4A，B帧尾部、BGRA上传、44.1kHz重采样/声道/尾长、纯音频/视频、背压、HTTP和读包取消、实际Player按钮/独立窗口/重启/退出；默认关闭声音 |
| client.live_services | 可选；Windows Qt → Docker 调度/登录/信令；两个角色与关闭/断线/过期清理 |

LocalAuthServer 只编译进 CSNLoginTests，不进入业务可执行程序。真实集成测试会短暂创建自己的测试房间，完成后关闭连接，由服务器清理；不要与同账号正式演示混用（当前仅开发账号）。

截图保存到 out/，完整测试还生成 `out/音频重采样混音验证.wav`（人工正弦波、48kHz 双声道 PCM16、1 秒）。构建目录与临时文件被 Git 忽略。Linux 服务测试及 ASan/UBSan 命令见 [服务端说明](server-services.md)。

## UML

```powershell
python scripts/render_uml.py
```

需要 Pillow 与 Windows 微软雅黑。按最新约定，脚本默认只生成时序图，每张同时有中文名 SVG/PNG；历史类图不再更新。渲染后检查新增图像，源码、图与业务行为保持一致。

采集与原生GPU预览直接运行两个客户端调试；WGC示例窗口、人脸/美颜和GPU播放的自动整链路检查保留在单份SRS集成中。独立NV12/P010颜色对照等深度硬件专项已删除，当前7组回归不再宣称覆盖那些历史专项。

`OBSEncodingTests.exe --gpu`仍可显式验证NVIDIA D3D11/NVENC：合成90帧/3秒光音信号，解码核对框点/尺寸变化/时间戳。运行时PATH需包含Qt/bin，输出`out/编码验证.mp4`。详见[第010步](010-ffmpeg-encode.md)。

点播新增libswscale及swscale-8.dll，CMake随其他FFmpeg DLL一起复制。默认测试用SDK/bin/ffmpeg.exe生成素材，不依赖外部视频。`python scripts/run_playback_checks.py --audio-device` 显式验证真实输出设备（音量0），包含音轨延后及长视频尾部；`--mvc-only`支持2倍缩放截图，`--media-file "out/编码验证.mp4"`验证明确选择的录制文件。详见 [第011步](011-player-vod.md)。

本地美颜与GPU解码整链路：现有Windows业务服务和OBS-SRS启动后，执行`python SRS/check.py --beauty`，使用既有示例图/引擎和自建窗口验证WGC、TensorRT、美颜、RTMP与观众GPU解码。不涉及Mac研究项目，详见[第014步](014-local-beauty.md)。

直播延迟对照：`python SRS/check.py --beauty --latency`在同一现有集成程序内增加较宽/低延迟预算两组测量，以画面像素编号识别同帧，统计入队至GUI交付的P50/P95、队列、帧交付比例和音频欠载；真实WASAPI音量0。报告保存`out/低延迟验证/RTMP延迟对照.json`，图片中文命名；不包含真实采集和显示器扫描，也不以PTS直接推算端到端延迟。详见[第018步](018-rtmp-latency.md)。

独立SRS与直播集成：`./SRS/manage.ps1 start`，确认OBS业务服务已运行，再执行`python SRS/check.py`。构建新增`OBSStreamingTests`目标，验证生成GPU画面/合成PCM经NVENC和SRS到Player，WASAPI音量0；该测试依赖外部容器与NVIDIA，独立于默认CTest。部署/源码版本集中在[SRS目录](../SRS/README.md)，FFmpeg生命周期见[第012步](012-srs-live.md)。

推流观测/手动码率：`python scripts/run_publish_checks.py`验证默认范围，`--gpu`显式生成270张限幅噪声帧，验证单会话内3000→400→2000kbps目标、独立重读MP4包字节/PTS/DTS/关键帧。临时媒体自动清理，截图为`out/推流控制验证/手动码率页面.png`；`QT_SCALE_FACTOR=2`额外保存中文二倍缩放截图。现有`python SRS/check.py`增加运行中调节、真实Publisher初始值/应用按钮和发送统计检查，`--beauty`同时覆盖美颜。详见[第015步](015-publish-control.md)。

`python scripts/run_publish_checks.py --gpu --weak-network`在同一现有程序内验证真实NVENC动态码流，以及本机TCP代理/RTMP收流的受阻恢复、ABR自动下降/稳定回升/手动接管、AAC追赶、收到的FLV时间戳与GPU解码。使用生成纹理/静音PCM，不依赖Docker，要求NVIDIA；见[第016步](016-live-queue.md)和[第017步](017-abr.md)。默认回归增加策略与界面检查，SRS集成增加实际按钮的完整配置切换；不恢复已删除阶段测试文件。模型优化工具保留内建`--self-test`和`--smoke`。
