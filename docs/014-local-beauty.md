# 第014步：本地AI定位与GPU美颜

## 范围

按用户确认，复用SCRFD-10G-KPS TensorRT `.engine`做本地人脸定位，再用D3D11 Shader做磨皮和提亮；不调用远程API。效果进入Publisher实际视频链路，预览、MP4录制和RTMP推流消费同一处理纹理。Player显示接收到的处理结果；播放器不另做一遍美颜。

这是AI驱动定位加传统GPU效果：框和五点来自模型，效果不是生成模型，也没有训练皮肤分割。当前不实现瘦脸、大眼、密集关键点变形或HDR美颜。

## 对象持有与线程

`SessionController`仍持有`VideoCapture`采集线程与`GpuFaceDetector`检测线程。新增普通`GpuBeauty`由检测线程的私有Resources持有，没有新线程、窗口或MVC层。它拥有D3D11私有延迟上下文、Shader、参数缓冲与完成查询；共用输入的同一设备。

`FaceFrame`保留原采集序号/QPC、人脸框点和处理统计，`video`可指向原图或新的独立美颜纹理。`drawFaces`只控制诊断框点显示，`beautified`记录是否走效果处理，`beautyMs`为提交及等待的墙钟耗时，不能当作纯GPU时间。模型推理GPU耗时仍用CUDA event单独测量。

## Controller → begin → start → run

1. Publisher选择视频源和本地engine；“显示人脸框与五点”与“启用本地GPU美颜”独立。任一开启，Controller启动现有检测线程。
2. `GpuFaceDetector.begin(engine, settings, drawFaces)`清邮箱、保存本轮开关，再`QThread.start`；GUI不加载模型/编译Shader。开关及engine本轮锁定，磨皮/提亮强度可实时调节。
3. `run`读取引擎，等采集首帧，沿D3D11适配器选择CUDA设备，创建执行上下文与复用缓冲。
4. GUI每33ms取最新采集快照并`submit`；单帧邮箱保留最新输入，慢处理不堆积。工作线程用原图做GPU预处理、TensorRT推理、CPU输出解码/NMS。
5. 得到同帧框点后，按需创建普通`GpuBeauty`并`apply`；原图不回写，输出为新BGRA纹理。参数变动通过互斥锁存储，下一帧一次取完整快照，不在绘制过程中变化。
6. 原子发布完成结果。Controller交给`MediaTimeline`按原QPC调度，GUI预览与`MediaPublisher`使用同一`TimedVideoFrame`。美颜在编码前已完成，不只修改显示窗口。

## apply中的GPU处理

1. 关闭、无脸、零强度直接透传原纹理；最多使用16张脸，检查置信度和有限坐标，裁剪到画面边界。
2. 基于人脸框构造椭圆软边区域，用五点保护双眼、鼻尖和嘴部细节，再用宽松YCbCr肤色门控抑制背景渗入。它是启发式区域，不保证精确皮肤边缘或所有肤色/光照条件。
3. 对局部区域执行5×5保边平滑：空间距离与RGB差异共同决定采样权重；强度0～100%。提亮按剩余亮度空间做柔和提升，限制到有效RGB范围，不提升整个画面曝光。
4. 专用延迟上下文绘制到独立BGRA RenderTarget/ShaderResource；`ExecuteCommandList(TRUE)`恢复共享绘制状态。输入和输出图像不Map、不下载CPU。
5. EVENT确认完成，可取消且2秒超时，再交付同帧序号/QPC。输出纹理由COM引用持有，后续处理/停止/切尺寸不会改写已经交给预览或编码的纹理。

## stop与回退

Controller停止采集时同时`stop`检测：清输入/结果邮箱、请求中断并唤醒；当前推理完成后也不再发布迟到结果。美颜完成查询会检查取消。工作线程释放Shader/纹理及CUDA/TensorRT资源后发`finished`，Controller才允许重启。

模型缺失、设备不支持或处理失败锁存本轮错误，恢复原帧的同步/预览/编码，音频继续；停止后可重新选择引擎重试。零强度和无脸是正常透传，不是错误。美颜参数不是网络服务配置，不新增服务器或Docker端口。

## 运行与验证

```powershell
./scripts/build_clients.ps1 -Deploy
python scripts/run_beauty_checks.py
python scripts/run_beauty_checks.py --gpu
```

Qt Creator重新配置后运行`OBS_Publisher`，登录 → 选择视频源 → 确认`.engine`路径 → 勾选本地GPU美颜 → 开始采集/直播。默认磨皮50%、提亮15%，运行中可调整；需要观察定位才勾选框点。录制勾选保存MP4，直播仍通过现有房间地址。

默认`client.beauty`用WARP和人工纹理验证效果、背景保护、透传、独立输出、只读输入、尺寸变化、共享上下文恢复、取消与小窗口控件，不打开有效采集源。`--gpu`显式使用既有示例图/引擎，验证真实TensorRT同帧处理、参数变化、停止/重启，以及仅采集测试自建示例窗口的实际Publisher预览/MP4闭环；截图保存`out/美颜验证/`，测试临时录制自动清理。

本机2026-10-14、RTX 5060：构建及9组CTest通过。人工皮肤纹理的相邻像素差异总量1320→660，双眼中心/区域外背景保持原值，原输入与保留输出不变。真实SCRFD示例检测到1张脸，同帧效果、零强度、关闭框点、参数变化、停止/重启均通过。实际Publisher测试覆盖缺失引擎回退原帧与重试；本次26张美颜同步帧进入MP4，重新GPU解码得到26帧。现有Windows RTMP验收41张美颜帧进入推流，Player持续GPU解码显示、房间关闭清理通过；测试后恢复服务原停止状态。

640×480逻辑尺寸的小窗口与二倍缩放验证通过，磨皮/提亮输入完整位于滚动视口内，截图为`美颜控件页面.png`、`美颜控件二倍缩放.png`。

5次同图重复处理的本次平均总墙钟20.00ms，美颜提交/等待7.40ms；另一轮分别22.20ms、12.60ms。首帧包括资源/Shader初始化，本次238ms，其中美颜204ms；这些不是长期帧率或纯GPU耗时保证。单张示例图只能证明链路与可见效果；多肤色、侧脸、遮挡、强光、运动稳定性以及30fps长期负载仍需后续单独验证。

时序图：[本地GPU美颜](uml/gpu-beauty.mmd)、[GPU检测](uml/gpu-face.mmd)。模型优化独立工程继续维护在`cpp/`，美颜运行不重新构建engine。
