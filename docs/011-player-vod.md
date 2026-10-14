# 第 011 步：Player 点播

Player 的点播页已接入本地文件与 HTTP(S) 媒体地址。工作台支持播放、停止、实时音量、声音开关、进度与结束后重复播放，画面继续使用独立非模态窗口。输入选择不自动播放；关闭画面仅隐藏，声音与播放流程继续，可用“打开画面”恢复。

当前先实现播放/停止，不包含暂停、跳转、倍速、播放列表、在线认证或直播拉流。进度条只展示进度，不是可拖动的跳转控件。

## 文件、线程与资源

| 类 | 文件位置 | 谁持有 / 在哪个线程使用 |
| --- | --- | --- |
| MediaPlayer | `OBS client/ffmpeg/` | SessionController拥有的QThread；GUI调用begin/stop/takeVideo/stats，run在播放工作线程 |
| MediaDecoder | `OBS client/ffmpeg/` | MediaPlayer::run的局部普通类，独占AVFormatContext、两个解码上下文、SWR/SWS与上传设备 |
| WasapiPlayback | `OBS client/` | MediaPlayer的成员QThread；端点COM/IAudioClient/IAudioRenderClient/IAudioClock只在音频输出线程 |

没有增加播放Model/Controller，仍由SessionController协调。FFmpeg库目标OBSFFmpegCore新增解封装/解码及libswscale；WASAPI输出属于OBSMediaCore。类成员及cpp主流程有中文注释，不新增类图。

## Controller → begin → start → run

1. 登录后切换“点播”，选择本地文件或在线HTTP/HTTPS地址。若仍在直播房间，先完成room.leave，点播本身不要求信令连接成功。
2. MainWindow发playbackRequested(source, audioOutput)。SessionController::startPlayback检查登录、模式、房间及上轮finished，清预览和状态，设置playbackPending。
3. MediaPlayer::begin在GUI清队列/统计，保存输入和声音开关，调用QThread::start。构造/begin均不读媒体、不打开扬声器。
4. MediaPlayer::run创建普通MediaDecoder。open检查本地文件或HTTP(S)，绑定AVIO中断回调，读取容器/流信息，建立视频和音频软件解码上下文。打开/探测/单次读包有10秒操作上限，stop可打断网络等待。
5. 本轮需要声音且有音轨时，run调用成员WasapiPlayback::begin，启动第二个线程。它在MTA初始化默认输出端点和共享Float32流，PCM预缓冲100ms后Start。MediaPlayer发opened回GUI并开始读包。

## 解封装 → 解码 → 有界交付

```text
本地文件 / HTTP(S)
        ↓
MediaPlayer::run → MediaDecoder::readNext
        ↓ av_read_frame，按stream_index分类
    AVPacket                     AVPacket
        ↓                            ↓
视频send_packet/receive_frame    音频send_packet/receive_frame
        ↓                            ↓
SWS转BGRA，上传独立D3D11纹理      SWR转48kHz立体声交错Float32
        ↓                            ↓
最多16帧的播放器视频队列          最多1秒PCM的WASAPI队列
        ↓                            ↓
GUI每10ms按媒体时钟takeVideo      IAudioRenderClient写出
        ↓                            ↓
VideoRenderer → SwapChain        IAudioClock.GetPosition
        ↑────── 音频实际播放位置作为主时钟 ────┘
```

本步初版使用FFmpeg软件解码，视频通过libswscale转换成BGRA后上传，Renderer采样GPU纹理。后续[第013步](013-gpu-decode.md)已将默认视频路径升级为D3D11VA硬解与GPU颜色转换，软件上传作为明确回退。当前有视频的点播预缓冲200ms并等待首帧就绪，直播/纯音频维持100ms；线程、主时钟与启停契约沿用本页设计。

每张输出纹理由ComPtr独立持有，不引用会被解码器复用的AVFrame字节。支持最高3840×2160；封面图、字幕和其他流忽略；软件解码器用2个内部线程。B帧按best_effort_timestamp调度，不能按读取包的先后当作画面顺序。

音频从解码帧的原始采样格式/布局重采样为48kHz立体声。复用libswresample能力，播放的媒体PTS与采集AudioResampler的QPC状态分别管理。SWR实际输出样本数推进PTS，采样率/布局变化或明显时间跳变时排空并重建。AAC尾部按容器音轨结束位置裁剪，避免把补齐样本当成有效播放时长。

点播队列满时阻塞解码工作线程，保留未来帧；GUI只取到期帧，不等待文件或解码。音轨后置、视频先填满队列时允许音频线程先建立静音时钟，避免等音频/等视频相互卡住。网络暂停或音频不足时补静音并维持进度，尚未加入自动暂停缓冲/重连策略。

## 音频时钟和画面调度

所有PTS减去容器共同原点。PlaybackVideo用微秒，PlaybackAudio用1/48000秒采样位置；VideoFrame.timestamp100ns仍是上传时QPC，文件时间另存，不能与采集原点混用。

- 有音轨并启用“播放声音”：IAudioClock::GetPosition除以GetFrequency得到实际输出进度，GUI读取锁保护的快照；写入端点的帧数用于缓冲管理，不当作已经播放的位置。
- 关闭声音或纯视频：使用QPC单调时钟。关闭声音仍解码音轨并保留其结束时间，文件时长不会被缩短。
- 音频早于视频结束：音轨尾部及SWR最后样本到达后关闭音频入口、排空设备，视频再从音频停止位置按QPC继续走。
- 视频帧PTS还没到就留在队列；明显落后超过80ms且有替代到期帧时跳过旧画面。显示统计表示GUI交付，不等同显示器扫描次数。
- 全部解码EOF后，时钟限在实际媒体结束点，资源清理耗时不会让进度超过时长再倒退。

WASAPI事件通知可写缓冲，按GetCurrentPadding确定可写帧数，GetBuffer/ReleaseBuffer成对。PCM按PTS补区间静音、裁掉已播放重叠，音量0仍消费数据并推进时钟。容器声明的音轨前置静音不计作缓冲不足。

## stop → 清理/finished → 重启

正常EOF：读取结束 → 向两个解码器送空包取完延迟帧 → 排空SWR → 关闭PCM入口 → 播完队列和端点padding → 视频显示到最后一帧的结束时间 → 释放解码/IO/GPU与音频COM → completed(true) → QThread::finished。保留最后画面，可再次点击播放。

用户停止、切回直播、退出登录或关闭程序：Controller立即停GUI交付并清预览，Player请求中断、丢视频队列、唤醒背压，WASAPI丢剩余PCM并停止；AVIO中断读包。工作线程关闭资源后completed(false)/finished，迟到画面不重新弹出。构造后的QThread对象复用，每轮run重新创建解码上下文和输入资源。

打开/解码/音频/显示失败统一停止本轮，failed文字锁存，finished不抹掉错误。Controller处理finished后解锁输入与播放按钮。GUI日常停止不wait；析构才等待两个线程归还资源。

## 运行

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_clients.ps1 -Deploy
& ".\OBS client\build-agent\OBS_Player.exe"
```

Qt Creator重新配置CMake后选择OBS_Player运行。启动既有登录/调度服务 → root/root登录 → 点播 → 本地文件/在线地址 → 选择输入 → 设置声音/音量 → 播放。首帧打开独立窗口。选择已录制的MP4即可验证编码与解码闭环。

HTTP(S)入口面向普通媒体文件URL，例如MP4；不是网页播放页。当前使用FFmpeg内置网络/TLS并启用tls_verify证书验证，与Qt信令连接分别管理，不支持URL内账号密码、DRM或自定义认证头。公共HTTPS站点、播放列表/自适应分段流和长时间网络抖动尚未专门验收。

构建新增swscale.lib / swscale-8.dll，沿用FFmpeg7.1 shared SDK。默认点播测试生成合成媒体时使用SDK自带ffmpeg.exe，运行库/测试媒体均不提交Git。

## 验证

```powershell
$env:PATH = "C:/software/Qt/Tools/CMake_64/bin;" + $env:PATH
python scripts/run_checks.py
python scripts/run_playback_checks.py --audio-device
python scripts/run_playback_checks.py --media-file "out/编码验证.mp4" --audio-device
$env:QT_SCALE_FACTOR = "2"
$env:OBS_UI_SCREENSHOTS = "C:/projects/CourseStudioNext/out/二倍缩放点播"
python scripts/run_playback_checks.py --mvc-only
```

默认client.playback关闭音频输出，不打开扬声器；--audio-device显式使用默认输出设备，音量0，素材全部合成。本机验证覆盖中文路径、B帧EOF、26帧/1.04秒蓝色视频、双频独立声道、44.1kHz→48kHz的49920有效样本、纯音频/纯视频、有界背压、重复播放、本机HTTP与取消、独立窗口、模式切换、退出登录和错误恢复。

实际WASAPI覆盖普通媒体、音轨延后0.8秒、音频短于视频三种情况；本次结束位置分别1.040、1.839和2.040秒。HTTP无响应读包取消本次86ms；这些是本机短合成媒体结果，不是网络或长时间性能保证。

本机2026-10-12构建及8组CTest全部通过。此前NVENC生成的H.264/AAC合成录制文件，Player显示90帧、结束位置3.000秒，未跳帧或补静音。2倍缩放/小窗口验证路径、长错误文本及时间进度条完整显示，截图保存out/。

时序图已同步当前实现：[打开与解码](uml/vod-open.mmd)、[音画调度](uml/vod-play.mmd)、[停止与结束](uml/vod-stop.mmd)。后续RTMP闭环和硬件解码已分别完成，见[第012步](012-srs-live.md)、[第013步](013-gpu-decode.md)。WebRTC/ICE及实测低延迟优化仍为后续阶段。

接口依据：[FFmpeg7.1 send/receive](https://ffmpeg.org/doxygen/7.1/group__lavc__encdec.html)、[libswresample](https://ffmpeg.org/doxygen/7.1/group__lswr.html)、[WASAPI输出缓冲](https://learn.microsoft.com/en-us/windows/win32/coreaudio/rendering-a-stream)、[音频实际位置](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)。
