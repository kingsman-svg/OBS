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
    void setupCapture();
    void refreshDevices();
    void startCapture(const QList<CaptureSource> &selection);
    void stopCapture();
    void refreshCaptureView();
    void deliverCapture();
    void bindCapture(QThread *worker, int index);
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
    VideoCapture *m_video = nullptr;
    WasapiCapture *m_microphone = nullptr;
    WasapiCapture *m_system = nullptr;
    QTimer m_captureDelivery;
    QThread *m_enumerator = nullptr;
    QSet<QThread *> m_pendingCapture;
    QSet<int> m_captureErrors;
    QStringList m_captureMessages{QString(), QString(), QString()};
    QString m_deviceMessage;
    bool m_stoppingCapture = false;
    quint64 m_lastSequence = 0;
    qint64 m_lastPreviewTimestamp = 0;
};
}
