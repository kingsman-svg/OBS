#pragma once
#include "CaptureSource.h"
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
struct VideoFrame;
struct AudioPacket;

// 工作台控制器协调信令、房间及采集；底层采集类只负责设备和数据。
class SessionController final : public QObject {
    Q_OBJECT
public:
    SessionController(MainWindow *view, LoginModel *login, SessionModel *model,
                      SignalClient *signal, QObject *parent = nullptr);
    ~SessionController() override;
signals:
    // 当前在 GUI 线程交付；后续编码消费者应快速转交有界队列。
    void videoFrameReady(const csn::VideoFrame &frame);
    void audioPacketReady(csn::CaptureSource::Kind kind, const csn::AudioPacket &packet);
private:
    void onLoginChanged();
    void reconnect();
    void refreshView();
    void execute(const QString &type, const QJsonObject &fields = {});
    void refreshRooms();
    void onResponse(const QString &id, const QString &type, bool ok,
                    const QJsonObject &data, const QString &error);
    void onEvent(const QString &event, const QJsonObject &data);
    void scheduleExpiry();
    void setupCapture(); // 创建三个采集线程并绑定 opened/failed/finished，启动交付定时器。
    void refreshDevices(); // MTA 后台枚举设备，结果排队回 GUI；枚举期间禁止启动。
    void startCapture(const QList<CaptureSource> &selection); // 根据选择启动视频/麦克风/回环，不重复启动同一线程。
    void stopCapture(); // 异步请求全部退出，立即清除预览，等待 finished 解锁。
    void refreshCaptureView(); // 把线程状态和设备提示刷新到主窗口。
    void deliverCapture(); // GUI 每 33ms 取最新 GPU 帧及音频包，不让旧帧堆积。
    void bindCapture(QThread *worker, int index); // 绑定退出回调，index 为视频0/麦克风1/回环2。
    MainWindow *m_view;
    LoginModel *m_login;
    SessionModel *m_model;
    SignalClient *m_signal;
    QTimer m_expiry;
    bool m_loggedIn = false;
    QString m_pending;
    QString m_closedRoom;
    QJsonArray m_listing;
    int m_offset = 0;
    VideoCapture *m_video = nullptr; // 唯一视频采集线程，由控制器拥有。
    WasapiCapture *m_microphone = nullptr; // 麦克风线程，音频原始格式交付。
    WasapiCapture *m_system = nullptr; // 系统输出回环线程，与麦克风独立启停。
    QTimer m_captureDelivery; // GUI 线程取帧/包定时器，避免逐帧跨线程信号积压。
    QThread *m_enumerator = nullptr; // 临时 MTA 枚举线程，完成后 deleteLater。
    QSet<QThread *> m_pendingCapture; // 已启动且 GUI 尚未收到 finished 的线程。
    QSet<int> m_captureErrors; // 失败源索引，防止 finished 抹掉错误提示。
    QStringList m_captureMessages{QString(), QString(), QString()}; // 视频、麦克风、回环各自的状态文字。
    QString m_deviceMessage; // 设备枚举结果或权限提示。
    bool m_stoppingCapture = false; // 停止阶段拒绝交付迟到的帧/包。
    quint64 m_lastSequence = 0; // 上次交付的视频序号，用于邮箱去重。
};
}
