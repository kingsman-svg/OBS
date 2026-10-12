#pragma once
#include "CaptureSource.h"
#include "AudioMixer.h"
#include "MediaTimeline.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QSet>
class QThread;
class MainWindow;
namespace csn {
class LoginModel;
class SessionModel;
class SignalClient;
class VideoCapture;
class WasapiCapture;
class GpuFaceDetector;
class MediaPublisher;
class MediaPlayer;
struct VideoFrame;
struct AudioPacket;

// 工作台控制器协调信令、房间、采集录制及点播；底层媒体类只负责设备和数据。
class SessionController final : public QObject {
    Q_OBJECT
public:
    SessionController(MainWindow *view, LoginModel *login, SessionModel *model,
                      SignalClient *signal, QObject *parent = nullptr); // 绑定界面/信令回调，推流端再建立采集与检测。
    ~SessionController() override; // 正常启停异步；销毁时等待所有工作线程归还资源。
signals:
    // 当前在 GUI 线程交付；编码消费者只快速转交有界队列。
    void videoFrameReady(const csn::VideoFrame &frame);
    void audioPacketReady(csn::CaptureSource::Kind kind, const csn::AudioPacket &packet);
    void mixedAudioReady(const csn::AudioPacket &packet); // 混音诊断出口；编码使用下方同步信号。
    void synchronizedVideoReady(const csn::TimedVideoFrame &frame); // 30fps、PTS时间基1/30，编码的新入口。
    void synchronizedAudioReady(const csn::TimedAudioPacket &packet); // PTS时间基1/48000，与视频共用原点。
    void playbackFrameReady(const csn::VideoFrame &frame); // GUI调度后的播放帧，诊断/测试出口。
private:
    void onLoginChanged(); // 登录后连接信令；退出时停止采集、检测和会话计时。
    void reconnect();      // 使用有效登录会话重新连接，避免重复连接请求。
    void refreshView();    // 只把 SessionModel 状态映射到工作台控件。
    void execute(const QString &type, const QJsonObject &fields = {}); // 单个在途房间请求，保存响应编号。
    void refreshRooms();   // 从第一页开始拉取房间列表，后续响应继续分页。
    void onResponse(const QString &id, const QString &type, bool ok,
                    const QJsonObject &data, const QString &error); // 按请求编号校验并更新对应房间业务。
    void onEvent(const QString &event, const QJsonObject &data); // 处理当前房间的关闭/成员/开播变化。
    void scheduleExpiry(); // 到期退出登录；长生命周期分段安排 Qt 定时器。
    void setupCapture(); // 创建三个采集线程并绑定 opened/failed/finished，启动交付定时器。
    void refreshDevices(); // MTA 后台枚举设备，结果排队回 GUI；枚举期间禁止启动。
    void startCapture(const QList<CaptureSource> &selection); // 根据选择启动视频/麦克风/回环，不重复启动同一线程。
    void stopCapture(); // 异步请求全部退出，立即清除预览，等待 finished 解锁。
    void refreshCaptureView(); // 把线程状态和设备提示刷新到主窗口。
    void deliverCapture(); // GUI 每 33ms 取最新 GPU 帧及音频包，不让旧帧堆积。
    void bindCapture(QThread *worker, int index); // 绑定退出回调，index 为视频0/麦克风1/回环2。
    void consumeAudio();                      // 取两路原始包、发诊断信号并交给独立重采样器。
    void finishAudio();                       // 正常停止后排空混音，退出登录直接丢弃。
    void refreshAudioView();                  // 更新混音输出状态，错误不覆盖设备采集状态。
    void setupFaceDetection();                // 绑定检测 opened/failed/finished；仍由现有控制器协调。
    void deliverMedia(const MediaBatch &batch, bool preview); // 快速交付同步数据；停止排空不重新打开画面。
    void finishMedia();                       // 所有线程退出后排空同步队列，退出登录则直接清空。
    void refreshMediaView();                  // 同步等待、输出帧/包与丢弃统计。
    void setupRecording();                    // 创建编码工作线程，绑定快速入队和结果回调。
    void refreshRecordingView();              // GUI读取统计快照，录制错误不影响采集/预览。
    void setupPlayback();                     // Player创建播放线程，绑定begin意图和结果回调。
    void startPlayback(const QString &source, bool audioOutput); // GUI检查登录/模式/finished后begin。
    void stopPlayback();                      // GUI请求退出并立即暂停交付，线程异步回收。
    void deliverPlayback();                   // 每10ms按主时钟取到期帧，避免逐帧Qt事件积压。
    void refreshPlaybackView();               // 独立播放状态/时长/统计，不依赖信令连接成功。
    void setupLiveOutput();                  // 独立RTMP输出线程，与本地录制共用同步帧入口。
    void startLive();                        // GUI验证房间/源，先开启媒体入口再启动采集。
    void refreshLiveView();                  // GUI显示输出状态与统计，不显示包含流标识的地址。
    void updateLiveNotice();                 // 媒体首次写出/停止后发送live.start/stop，单请求串行。
    void startLivePlayback();                // 当前房间开播后自动begin，失败可手动重试。
    MainWindow *m_view;        // 非拥有 View，控制器仅调用其展示接口。
    LoginModel *m_login;       // 非拥有登录状态与凭据，不能打印其中令牌。
    SessionModel *m_model;     // 非拥有工作台业务状态，网络与媒体仍由模块处理。
    SignalClient *m_signal;   // 非拥有异步信令客户端。
    QTimer m_expiry;           // GUI 线程单次会话到期计时器。
    bool m_loggedIn = false;  // 上次处理的登录状态，避免重复启动/停止。
    QString m_pending;        // 当前在途房间请求编号，不是访问令牌。
    QString m_closedRoom;     // 已关闭房间ID，防止迟到加入响应恢复旧房间。
    QJsonArray m_listing;     // 当前分页累积的房间列表，完成后交给 Model。
    int m_offset = 0;         // 已请求的房间分页偏移，用于拒绝倒退/循环页。
    VideoCapture *m_video = nullptr; // 唯一视频采集线程，由控制器拥有。
    WasapiCapture *m_microphone = nullptr; // 麦克风线程，音频原始格式交付。
    WasapiCapture *m_system = nullptr; // 系统输出回环线程，与麦克风独立启停。
    QTimer m_captureDelivery; // GUI 线程取帧/包定时器，避免逐帧跨线程信号积压。
    QThread *m_enumerator = nullptr; // 临时 MTA 枚举线程，完成后 deleteLater。
    QSet<QThread *> m_pendingCapture; // 已启动且 GUI 尚未收到 finished 的采集/检测线程。
    QSet<int> m_captureErrors; // 失败源索引，防止 finished 抹掉错误提示。
    QStringList m_captureMessages{QString(), QString(), QString()}; // 视频、麦克风、回环各自的状态文字。
    QString m_deviceMessage; // 设备枚举结果或权限提示。
    bool m_stoppingCapture = false; // 暂停普通媒体交付；仍登录时允许音频尾部排空。
    quint64 m_lastSequence = 0; // 上次交付的视频序号，用于邮箱去重。
    AudioMixer m_audioMixer;                  // GUI串行处理两路有限长度数据，不增加采集MVC。
    quint64 m_mixedPackets = 0;                // 本轮已交付的固定10ms混音包数。
    QString m_audioError;                     // 音频处理错误锁存到下一轮，其他成功路继续。
    GpuFaceDetector *m_faces = nullptr;        // 独立检测 worker，单帧输入/结果邮箱。
    bool m_faceFailed = false;                // 本轮失败后继续原预览，不逐帧重试引擎。
    quint64 m_lastFaceSequence = 0;           // 已显示检测帧的序号，防止重复提交。
    QString m_faceMessage;                    // 独立检测状态，停止保留本轮错误。
    MediaTimeline m_media;                    // GUI串行同步调度，信号消费者只转交有界编码队列。
    MediaPublisher *m_publisher = nullptr;    // 控制器拥有的独立编码/封装QThread，播放端不创建。
    bool m_recordingPending = false;          // begin到GUI处理finished之间锁定下一轮录制。
    QString m_recordingMessage;               // 本轮等待、录制、封存、失败或保存路径。
    MediaPlayer *m_player = nullptr;          // Player控制器拥有，内部再持有WASAPI输出线程。
    QTimer m_playbackDelivery;                // GUI播放交付定时器，正常停止立即停。
    bool m_playbackPending = false;           // begin到GUI处理finished之间锁定下一轮。
    bool m_playbackStopping = false;          // 拒绝迟到画面，退出/模式切换不会弹回窗口。
    bool m_playbackFailed = false;            // 锁存解码/声音/显示错误，finished不覆盖。
    qint64 m_playbackDurationUs = 0;          // 已打开媒体的时长，未知时只显示当前时间。
    QString m_playbackMessage;                // 开始、播放、EOF、停止或错误文本。
    MediaPublisher *m_livePublisher = nullptr; // 独立RTMP编码/输出线程；录制失败不影响直播。
    QString m_liveUrl;                        // 创建房间响应给主播的地址，仅本轮房间内使用。
    QString m_liveMessage;                    // 等待、推流、停止、失败文本。
    bool m_livePending = false;               // begin到GUI处理finished期间不重启采集/直播。
    bool m_liveMediaSent = false;             // 已写出媒体字节；信令业务状态另由Model保存。
    bool m_playbackLive = false;              // 当前Player线程是直播还是点播。
    bool m_livePlaybackWanted = false;        // 房间开播请求播放；手动停止/失败撤销自动播放意图。
    bool m_leaveRequested = false;            // 开播请求在途时关闭房间，等响应后串行发leave。
};
}
