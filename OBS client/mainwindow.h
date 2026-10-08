#pragma once
#include "ClientRole.h"
#include "CaptureSource.h"
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QUrl>
namespace Ui { class MainWindow; }
class QStackedWidget;
class QLabel;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QListWidget;
class QComboBox;
class QCloseEvent;
class QProgressBar;

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
    void setCaptureSources(const QList<csn::CaptureSource> &sources);
    void applyCaptureState(bool canStart, bool active, bool enumerating,
                           const QString &devices, const QStringList &messages);
    void showCapturePreview(const QImage &image);
    void showAudioLevels(float microphone, float system);
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
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    QWidget *buildWorkspace();
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
    QLabel *m_preview = nullptr;
    QComboBox *m_videoSource = nullptr;
    QComboBox *m_micSource = nullptr;
    QComboBox *m_systemSource = nullptr;
    QPushButton *m_startCapture = nullptr;
    QPushButton *m_stopCapture = nullptr;
    QPushButton *m_refreshDevices = nullptr;
    QLabel *m_captureStatus = nullptr;
    QProgressBar *m_micLevel = nullptr;
    QProgressBar *m_systemLevel = nullptr;
    bool m_loggedIn = false;
    bool m_ready = false;
    bool m_busy = false;
    bool m_inRoom = false;
};
