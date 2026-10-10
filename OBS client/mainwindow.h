#pragma once
#include "ClientRole.h"
#include "CaptureSource.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QUrl>
#include <array>
namespace Ui { class MainWindow; }
namespace csn { struct VideoFrame; struct FaceFrame; }
class QStackedWidget;
class QLabel;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QListWidget;
class QComboBox;
class QCloseEvent;
class QProgressBar;
class QCheckBox;
class PreviewWindow;

// 两端共用 View：控件只发出意图，网络与业务交给 Controller。
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    explicit MainWindow(csn::ClientRole role, QWidget *parent = nullptr);
    ~MainWindow() override;
    csn::ClientRole role() const { return m_role; }
    QString signalHost() const;
    quint16 signalPort() const;
    void applyLoginState(const QString &message, bool busy, bool loggedIn);
    void applySessionState(bool ready, bool busy, bool connecting, const QString &message,
                           const QJsonObject &room, const QJsonArray &rooms);
    void setLoginEndpoint(const QUrl &endpoint);
    void setSignalEndpoint(const QString &host, quint16 port);
    void setCaptureSources(const QList<csn::CaptureSource> &sources); // 刷新三个源列表，保留匹配的选择。
    void applyCaptureState(bool canStart, bool active, bool enumerating,
                           const QString &devices, const QStringList &messages); // 同步按钮及各路状态。
    void showCapturePreview(const csn::VideoFrame &frame); // 交给独立 GPU 预览窗口；空帧清理。
    bool faceDetectionEnabled() const;                 // 本轮开始前读取勾选，运行期间锁定。
    QString faceEnginePath() const;                    // 已选本地引擎路径，不在 View 读取文件。
    void showFacePreview(const csn::FaceFrame &frame);   // 原画面与检测框点一起交给预览。
    void showFaceStatus(const QString &message);        // 独立状态文字，失败不覆盖采集/音频状态。
    void showAudioLevels(float microphone, float system); // 展示归一化 RMS，不播放音频。
    void showMixedAudio(float level, const QString &message); // 混音后的电平、格式及削波统计。
    void showMediaSyncStatus(const QString &message);    // 编码前时间线的输出/重复/丢弃计数。
    bool recordingEnabled() const;                     // 开始采集时读取；默认仅预览。
    QString recordingDirectory() const;                // 本地MP4保存目录，文件名由Controller生成。
    void showRecordingStatus(const QString &message);   // 编码计数、封存结果或独立错误提示。
signals:
    void loginRequested(const QUrl &endpoint, const QString &account, const QString &password);
    void cancelRequested();
    void logoutRequested();
    void reconnectRequested();
    void createRoomRequested(const QString &title);
    void refreshRoomsRequested();
    void joinRoomRequested(const QString &roomId);
    void leaveRoomRequested();
    void captureRequested(const QList<csn::CaptureSource> &sources);
    void stopCaptureRequested();
    void refreshDevicesRequested();
    void audioMixChanged(csn::CaptureSource::Kind kind, float gain, bool muted); // 控件只发意图，不处理 PCM。
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    QWidget *buildWorkspace();
    QWidget *buildCapturePanel(QWidget *parent);
    QWidget *buildPlayerPanel(QWidget *parent);
    Ui::MainWindow *ui;
    csn::ClientRole m_role;
    QStackedWidget *m_pages;
    QLabel *m_welcome;
    QLabel *m_signalStatus;
    QLabel *m_roomInfo;
    QLineEdit *m_signalHost;
    QSpinBox *m_signalPort;
    QPushButton *m_reconnect;
    QPushButton *m_leave;
    QLineEdit *m_roomTitle = nullptr;
    QPushButton *m_create = nullptr;
    QListWidget *m_rooms = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_join = nullptr;
    QComboBox *m_mode = nullptr;
    PreviewWindow *m_preview = nullptr;       // 拥有的非模态 GPU 画面窗口，关闭仅隐藏。
    QPushButton *m_showPreview = nullptr;     // 主动恢复被隐藏的画面窗口。
    QComboBox *m_videoSource = nullptr;       // 摄像头/窗口/屏幕共用一项选择。
    QComboBox *m_micSource = nullptr;         // 麦克风端点选择，播放端为空。
    QComboBox *m_systemSource = nullptr;      // 系统声音回环端点选择。
    QPushButton *m_startCapture = nullptr;    // 发出开始意图，实际启停由控制器处理。
    QPushButton *m_stopCapture = nullptr;     // 请求异步停止所有采集路。
    QPushButton *m_refreshDevices = nullptr;  // 请求后台重新枚举。
    QLabel *m_captureStatus = nullptr;        // 设备枚举及三路采集状态文字。
    QProgressBar *m_micLevel = nullptr;       // 麦克风 RMS 电平显示。
    QProgressBar *m_systemLevel = nullptr;    // 回环 RMS 电平显示。
    std::array<QSpinBox *, 2> m_audioGain{};   // 麦克风/回环线性音量，0～200%。
    std::array<QCheckBox *, 2> m_audioMute{};  // 两路静音选择，恢复时不重放旧数据。
    QProgressBar *m_mixedLevel = nullptr;     // 削波后混音 RMS，独立于原始输入电平。
    QLabel *m_mixStatus = nullptr;            // 输出格式、包数、削波和处理错误。
    QLabel *m_syncStatus = nullptr;           // 同步状态独占整行，文字增高后由滚动区承载。
    QCheckBox *m_faceDetection = nullptr;     // 勾选后启用实时检测；默认关闭。
    QLineEdit *m_faceEngine = nullptr;        // 本地 .engine 路径，可浏览选择。
    QPushButton *m_chooseEngine = nullptr;    // 仅停止时允许更换引擎。
    QLabel *m_faceStatus = nullptr;           // 人脸数、耗时、邮箱替换数或失败信息。
    QCheckBox *m_recording = nullptr;          // 勾选后本轮采集同时编码到MP4。
    QLineEdit *m_recordDirectory = nullptr;    // 保存目录，运行期间锁定。
    QPushButton *m_chooseRecordDirectory = nullptr; // 选择目录，不打开文件或启动编码。
    QLabel *m_recordStatus = nullptr;         // 独占整行，长路径换行，由滚动区承载。
    bool m_loggedIn = false;
    bool m_ready = false;
    bool m_busy = false;
    bool m_inRoom = false;
};
