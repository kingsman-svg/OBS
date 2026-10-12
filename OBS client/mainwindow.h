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
    bool vodMode() const;                              // 点播模式才接受播放意图，切回直播时停止。
    void applyPlaybackState(bool active, qint64 positionUs, qint64 durationUs, const QString &message); // 控件/进度，不操作解码器。
    void showPlaybackPreview(const csn::VideoFrame &frame); // Player独立窗口消费到期GPU帧，空帧清理。
    QList<csn::CaptureSource> selectedCaptureSources() const; // 采集/直播按钮共用本轮源选择。
    bool playbackAudioEnabled() const;                // 点播与直播共用声音选择。
    void applyLiveOutputState(bool canStart, bool active, const QString &message); // 只展示直播启停状态。
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
    void playbackRequested(const QString &source, bool audioOutput); // 选择的本地文件或HTTP(S)地址。
    void stopPlaybackRequested();                      // 停止、模式切换均交给同一个Controller。
    void playbackVolumeChanged(float volume);         // 实时音量0～1，静音也保留播放时钟。
    void playbackRenderFailed(const QString &message); // 显示失败时Controller停止音频和解码。
    void startLiveRequested();                         // 主播开始采集并推流；Controller取得房间地址后执行。
    void stopLiveRequested();                          // 主播停止采集/媒体后通知信令。
    void playLiveRequested();                          // 观众重试当前直播地址，不新增播放MVC。
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
    QComboBox *m_vodSource = nullptr;          // 本地文件/在线地址，运行期间锁定。
    QLineEdit *m_localMedia = nullptr;         // 选择或粘贴本地媒体路径。
    QLineEdit *m_mediaUrl = nullptr;           // HTTP(S)点播地址，不与直播房间地址混用。
    QPushButton *m_browseMedia = nullptr;      // 选择文件不自动播放。
    QPushButton *m_playMedia = nullptr;        // 发begin意图，finished前禁用。
    QPushButton *m_stopMedia = nullptr;        // 请求异步停止。
    QCheckBox *m_playAudio = nullptr;          // 本轮是否打开扬声器；关闭时用单调时钟。
    QSpinBox *m_playVolume = nullptr;          // 0～100%，运行中可调。
    QProgressBar *m_playProgress = nullptr;    // 只读进度，本步尚未接入跳转。
    QLabel *m_playbackStatus = nullptr;        // 时长、解码统计和错误详情独占整行。
    bool m_playbackActive = false;            // begin到GUI收到finished之间不更换输入。
    bool m_loggedIn = false;
    bool m_ready = false;
    bool m_busy = false;
    bool m_inRoom = false;
    bool m_roomStreaming = false;                      // 当前房间业务状态，媒体状态由Controller独立展示。
    QPushButton *m_startLive = nullptr;                // 主播开始直播，创建房间/选好源后启用。
    QPushButton *m_stopLive = nullptr;                 // 主播异步停播。
    QLabel *m_liveOutputStatus = nullptr;              // 网络输出统计/错误，长文本自动换行。
    QPushButton *m_playLive = nullptr;                 // 观众失败后手动重试。
    QPushButton *m_stopLivePlayback = nullptr;         // 观众停止媒体，可继续留在房间。
    QLabel *m_livePlaybackStatus = nullptr;            // 直播模式下显示解码/播放信息。
};
