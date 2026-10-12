#include "mainwindow.h"
#include <QProgressBar>
#include <QGridLayout>
#include "PreviewWindow.h"
#include <QCheckBox>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>
#include "ui_mainwindow.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QListWidget>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent) : MainWindow(csn::ClientRole::Publisher, parent) {}

MainWindow::MainWindow(csn::ClientRole role, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_role(role)
{
    ui->setupUi(this);
    const QSize available = screen()->availableGeometry().size() - QSize(40, 80);
    setMinimumSize(QSize(640, 480).boundedTo(available));
    resize(QSize(880, 720).boundedTo(available));
    ui->titleLabel->setText(tr("OBS · %1登录").arg(role == csn::ClientRole::Publisher ? tr("推流端") : tr("播放端")));
    ui->titleLabel->setStyleSheet(QStringLiteral("font-size:24px; font-weight:600;"));
    ui->endpointLabel->setText(tr("调度 / 登录地址"));
    ui->endpointEdit->setText(QStringLiteral("http://127.0.0.1:8080/login/server"));
    ui->endpointEdit->setToolTip(tr("默认先调度再登录；也可直接填写登录节点的 /auth/login 地址。"));
    ui->accountEdit->setText(QStringLiteral("root"));
    m_signalHost = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_signalHost->setObjectName(QStringLiteral("signalHostEdit"));
    m_signalPort = new QSpinBox(this);
    m_signalPort->setObjectName(QStringLiteral("signalPortSpin"));
    m_signalPort->setRange(1, 65535);
    m_signalPort->setValue(9000);
    ui->formLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    ui->formLayout->addRow(tr("信令主机"), m_signalHost);
    ui->formLayout->addRow(tr("信令端口"), m_signalPort);
    auto *loginForm = takeCentralWidget();
    loginForm->setMinimumWidth(qMin(480, minimumWidth() - 48));
    loginForm->setMaximumWidth(620);
    loginForm->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *loginPage = new QWidget(this);
    auto *loginLayout = new QVBoxLayout(loginPage);
    loginLayout->setContentsMargins(24, 24, 24, 24);
    loginLayout->addStretch();
    loginLayout->addWidget(loginForm, 0, Qt::AlignHCenter);
    loginLayout->addStretch();
    m_preview = new PreviewWindow(
        role == csn::ClientRole::Publisher ? tr("OBS · 采集预览") : tr("OBS · 播放画面"),
        role == csn::ClientRole::Publisher ? tr("尚无视频画面\n选择视频源后开始采集") : tr("尚无播放画面\n在点播页选择媒体后播放"), this);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("pages"));
    m_pages->addWidget(loginPage);
    m_pages->addWidget(buildWorkspace());
    connect(m_preview, &PreviewWindow::failed, this, [this](const QString &message) {
        if (m_captureStatus) m_captureStatus->setText(message);
        if (m_role == csn::ClientRole::Player) emit playbackRenderFailed(message);
    });
    setCentralWidget(m_pages);
    setStyleSheet(QStringLiteral(
        "QMainWindow { background:#f3f5f9; } QLabel { color:#26354a; }"
        "QLineEdit,QSpinBox,QComboBox { padding:6px; min-height:20px; background:white; border:1px solid #ccd5e1; border-radius:5px; }"
        "QComboBox { padding-right:24px; } QSpinBox { padding-right:40px; }"
        "QPushButton { padding:7px 14px; min-height:20px; border:1px solid #ccd5e1; border-radius:5px; background:white; }"
        "QPushButton:hover { background:#edf3fc; } QPushButton:disabled { color:#9aa5b4; background:#f5f7fa; }"
        "QPushButton#startCaptureButton,QPushButton#loginButton { background:#2864dc; color:white; border-color:#2864dc; }"
        "QPushButton#startCaptureButton:disabled,QPushButton#loginButton:disabled { background:#afc2e6; border-color:#afc2e6; }"
        "QGroupBox { background:white; border:1px solid #dce3ed; border-radius:8px; margin-top:12px; padding-top:12px; font-weight:600; }"
        "QGroupBox::title { subcontrol-origin:margin; left:16px; padding:0 5px; }"
        "QScrollArea { border:0; background:transparent; } QWidget#workspaceContent { background:transparent; }"
        "QListWidget { background:white; border:1px solid #dce3ed; border-radius:5px; }"
        "QProgressBar { border:0; background:#e7edf5; border-radius:4px; max-height:8px; }"
        "QProgressBar::chunk { background:#32a875; border-radius:4px; }"));
    connect(ui->loginButton, &QPushButton::clicked, this, [this] {
        const QString password = ui->passwordEdit->text();
        ui->passwordEdit->clear();
        emit loginRequested(QUrl(ui->endpointEdit->text().trimmed()), ui->accountEdit->text(), password);
    });
    connect(ui->cancelButton, &QPushButton::clicked, this, &MainWindow::cancelRequested);
    connect(ui->logoutButton, &QPushButton::clicked, this, &MainWindow::logoutRequested);
    applySessionState(false, false, false, tr("登录后连接信令服务器。"), {}, {});
    applyLoginState(tr("开发账号 root，密码 root。请先启动 Docker 中的服务器。"), false, false);
}

QWidget *MainWindow::buildWorkspace()
{
    auto *home = new QWidget(this);
    home->setObjectName(QStringLiteral("homePage"));
    auto *layout = new QVBoxLayout(home);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);
    auto *header = new QHBoxLayout;
    auto *identity = new QVBoxLayout;
    auto *heading = new QLabel(m_role == csn::ClientRole::Publisher ? tr("OBS · 推流工作台") : tr("OBS · 播放工作台"), home);
    heading->setStyleSheet(QStringLiteral("font-size:22px; font-weight:600;"));
    identity->addWidget(heading);
    m_welcome = new QLabel(home);
    m_welcome->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_welcome->setWordWrap(true);
    identity->addWidget(m_welcome);
    header->addLayout(identity, 1);
    m_showPreview = new QPushButton(tr("打开画面"), home);
    m_showPreview->setObjectName(QStringLiteral("showPreviewButton"));
    header->addWidget(m_showPreview);
    ui->buttonLayout->removeWidget(ui->logoutButton);
    header->addWidget(ui->logoutButton);
    layout->addLayout(header);
    connect(m_showPreview, &QPushButton::clicked, m_preview, &PreviewWindow::present);

    auto *connection = new QHBoxLayout;
    m_signalStatus = new QLabel(home);
    m_signalStatus->setObjectName(QStringLiteral("signalStatusLabel"));
    // 先设置尺寸策略，再启用换行，避免重设策略时丢失 heightForWidth。
    m_signalStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_signalStatus->setWordWrap(true);
    connection->addWidget(m_signalStatus, 1);
    m_reconnect = new QPushButton(tr("重新连接"), home);
    m_reconnect->setObjectName(QStringLiteral("reconnectButton"));
    connection->addWidget(m_reconnect);
    layout->addLayout(connection);
    connect(m_reconnect, &QPushButton::clicked, this, &MainWindow::reconnectRequested);

    // 操作面板按内容自然排布；小窗口和高 DPI 下由滚动区兜底，不压缩控件。
    auto *scroll = new QScrollArea(home);
    scroll->setObjectName(QStringLiteral("workspaceScroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("workspaceContent"));
    auto *panels = new QVBoxLayout(content);
    panels->setContentsMargins(0, 0, 8, 0);
    panels->setSpacing(18);
    panels->setSizeConstraint(QLayout::SetMinimumSize);
    panels->addWidget(m_role == csn::ClientRole::Publisher ? buildCapturePanel(content) : buildPlayerPanel(content));

    auto *room = new QGroupBox(tr("直播房间"), content);
    auto *roomLayout = new QVBoxLayout(room);
    roomLayout->setContentsMargins(16, 24, 16, 16);
    roomLayout->setSpacing(12);
    if (m_role == csn::ClientRole::Publisher) {
        auto *create = new QHBoxLayout;
        m_roomTitle = new QLineEdit(tr("我的直播间"), room);
        m_roomTitle->setObjectName(QStringLiteral("roomTitleEdit"));
        m_roomTitle->setMaxLength(128);
        m_roomTitle->setMinimumWidth(0);
        m_create = new QPushButton(tr("创建房间"), room);
        m_create->setObjectName(QStringLiteral("createRoomButton"));
        create->addWidget(m_roomTitle, 1);
        create->addWidget(m_create);
        roomLayout->addLayout(create);
        connect(m_create, &QPushButton::clicked, this, [this] { emit createRoomRequested(m_roomTitle->text().trimmed()); });
        auto *liveActions = new QHBoxLayout;
        m_startLive = new QPushButton(tr("开始直播"), room); m_startLive->setObjectName(QStringLiteral("startLiveButton"));
        m_stopLive = new QPushButton(tr("停止直播"), room); m_stopLive->setObjectName(QStringLiteral("stopLiveButton"));
        m_startLive->setEnabled(false); m_stopLive->setEnabled(false);
        liveActions->addWidget(m_startLive); liveActions->addWidget(m_stopLive); liveActions->addStretch(); roomLayout->addLayout(liveActions);
        m_liveOutputStatus = new QLabel(tr("创建房间、选择采集源后开始直播；可同时录制MP4。"), room);
        m_liveOutputStatus->setObjectName(QStringLiteral("liveOutputStatusLabel"));
        m_liveOutputStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum); m_liveOutputStatus->setWordWrap(true);
        roomLayout->addWidget(m_liveOutputStatus);
        connect(m_startLive, &QPushButton::clicked, this, &MainWindow::startLiveRequested);
        connect(m_stopLive, &QPushButton::clicked, this, &MainWindow::stopLiveRequested);
    }
    auto *membership = new QHBoxLayout;
    m_roomInfo = new QLabel(room);
    m_roomInfo->setObjectName(QStringLiteral("roomInfoLabel"));
    m_roomInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_roomInfo->setWordWrap(true);
    membership->addWidget(m_roomInfo, 1);
    m_leave = new QPushButton(m_role == csn::ClientRole::Publisher ? tr("关闭房间") : tr("退出房间"), room);
    m_leave->setObjectName(QStringLiteral("leaveRoomButton"));
    membership->addWidget(m_leave);
    roomLayout->addLayout(membership);
    connect(m_leave, &QPushButton::clicked, this, &MainWindow::leaveRoomRequested);
    panels->addWidget(room);
    panels->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    return home;
}

QWidget *MainWindow::buildCapturePanel(QWidget *parent)
{
    // 1. 采集与人脸检测分成两张卡片，所有内容按自然高度进入工作台滚动区。
    auto *panel = new QWidget(parent);
    auto *panels = new QVBoxLayout(panel);
    panels->setContentsMargins(0, 0, 0, 0);
    panels->setSpacing(18);
    auto *capture = new QGroupBox(tr("音视频采集"), panel);
    capture->setObjectName(QStringLiteral("captureGroup"));
    panels->addWidget(capture);
    auto *layout = new QVBoxLayout(capture);
    layout->setContentsMargins(16, 24, 16, 16);
    layout->setSpacing(14);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    m_videoSource = new QComboBox(capture);
    m_videoSource->setObjectName(QStringLiteral("videoSourceCombo"));
    m_micSource = new QComboBox(capture);
    m_micSource->setObjectName(QStringLiteral("microphoneCombo"));
    m_systemSource = new QComboBox(capture);
    m_systemSource->setObjectName(QStringLiteral("systemAudioCombo"));
    for (auto *combo : {m_videoSource, m_micSource, m_systemSource}) {
        // 长设备名只在下拉列表和提示中完整显示，不撑大主窗口。
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(14);
        combo->setMinimumWidth(0);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(combo, &QComboBox::currentTextChanged, combo, &QWidget::setToolTip);
    }
    m_micLevel = new QProgressBar(capture);
    m_micLevel->setObjectName(QStringLiteral("microphoneLevel"));
    m_systemLevel = new QProgressBar(capture);
    m_systemLevel->setObjectName(QStringLiteral("systemAudioLevel"));
    m_mixedLevel = new QProgressBar(capture);
    m_mixedLevel->setObjectName(QStringLiteral("mixedAudioLevel"));
    for (auto *level : {m_micLevel, m_systemLevel, m_mixedLevel}) {
        level->setRange(0, 100); level->setValue(0); level->setTextVisible(false);
        level->setMinimumWidth(80);
        level->setMaximumWidth(160);
        level->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    grid->addWidget(new QLabel(tr("视频源"), capture), 0, 0);
    grid->addWidget(m_videoSource, 0, 1);
    grid->addWidget(new QLabel(tr("麦克风"), capture), 1, 0);
    grid->addWidget(m_micSource, 1, 1);
    grid->addWidget(new QLabel(tr("系统声音"), capture), 3, 0);
    grid->addWidget(m_systemSource, 3, 1);
    // 2. 设备下拉框独占输入列；音量、静音和电平另起一行，避免挤压设备名。
    for (int index = 0; index < 2; ++index) {
        auto *controls = new QHBoxLayout;
        controls->addWidget(new QLabel(tr("音量"), capture));
        m_audioGain[index] = new QSpinBox(capture);
        m_audioGain[index]->setObjectName(index == 0 ? QStringLiteral("microphoneGain") : QStringLiteral("systemAudioGain"));
        m_audioGain[index]->setRange(0, 200); m_audioGain[index]->setSuffix(QStringLiteral("%")); m_audioGain[index]->setValue(100);
        m_audioGain[index]->setMinimumWidth(m_audioGain[index]->fontMetrics().horizontalAdvance(QStringLiteral("200%")) + 52);
        m_audioGain[index]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_audioMute[index] = new QCheckBox(tr("静音"), capture);
        m_audioMute[index]->setObjectName(index == 0 ? QStringLiteral("microphoneMute") : QStringLiteral("systemAudioMute"));
        controls->addWidget(m_audioGain[index]); controls->addWidget(m_audioMute[index]); controls->addStretch(1);
        controls->addWidget(new QLabel(tr("电平"), capture));
        controls->addWidget(index == 0 ? m_micLevel : m_systemLevel, 1);
        grid->addLayout(controls, index == 0 ? 2 : 4, 0, 1, 2);
        // 3. 输入改变只发意图；混音器由现有 SessionController 串行操控。
        const auto changed = [this, index] {
            emit audioMixChanged(index == 0 ? csn::CaptureSource::Kind::Microphone : csn::CaptureSource::Kind::Loopback,
                m_audioGain[index]->value() / 100.0f, m_audioMute[index]->isChecked());
        };
        connect(m_audioGain[index], &QSpinBox::valueChanged, this, changed);
        connect(m_audioMute[index], &QCheckBox::toggled, this, changed);
    }
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
    auto *mixed = new QHBoxLayout;
    mixed->addWidget(new QLabel(tr("混音输出"), capture));
    mixed->addStretch(1);
    mixed->addWidget(m_mixedLevel, 1);
    layout->addLayout(mixed);
    m_mixStatus = new QLabel(tr("未启用音频混音"), capture);
    m_mixStatus->setObjectName(QStringLiteral("audioMixStatusLabel"));
    m_mixStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_mixStatus->setWordWrap(true); // 在 sizePolicy 后设置，保留 heightForWidth 换行测量。
    m_mixStatus->setMinimumHeight(m_mixStatus->fontMetrics().lineSpacing() * 2);
    layout->addWidget(m_mixStatus); // 格式、统计及错误详情独占整行，可随文本增高。
    m_syncStatus = new QLabel(tr("音视频同步未开始"), capture);
    m_syncStatus->setObjectName(QStringLiteral("mediaSyncStatusLabel"));
    m_syncStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_syncStatus->setWordWrap(true);
    layout->addWidget(m_syncStatus);
    auto *actions = new QHBoxLayout;
    m_startCapture = new QPushButton(tr("开始采集"), capture);
    m_startCapture->setObjectName(QStringLiteral("startCaptureButton"));
    m_stopCapture = new QPushButton(tr("停止采集"), capture);
    m_stopCapture->setObjectName(QStringLiteral("stopCaptureButton"));
    m_refreshDevices = new QPushButton(tr("刷新设备"), capture);
    m_refreshDevices->setObjectName(QStringLiteral("refreshDevicesButton"));
    actions->addWidget(m_startCapture);
    actions->addWidget(m_stopCapture);
    actions->addStretch();
    actions->addWidget(m_refreshDevices);
    layout->addLayout(actions);
    m_captureStatus = new QLabel(tr("正在枚举设备…"), capture);
    m_captureStatus->setObjectName(QStringLiteral("captureStatusLabel"));
    m_captureStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_captureStatus->setWordWrap(true);
    m_captureStatus->setStyleSheet(QStringLiteral("color:#596b82; font-weight:normal;"));
    layout->addWidget(m_captureStatus);
    auto *hint = new QLabel(tr("首帧自动打开独立画面窗口，关闭画面不会停止采集。"), capture);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    // 4. 人脸开关、引擎路径和状态独立排布；完整路径可在提示中查看。
    auto *face = new QGroupBox(tr("人脸检测"), panel);
    face->setObjectName(QStringLiteral("faceDetectionGroup"));
    auto *faceLayout = new QVBoxLayout(face);
    faceLayout->setContentsMargins(16, 24, 16, 16);
    faceLayout->setSpacing(12);
    panels->addWidget(face);
    m_faceDetection = new QCheckBox(tr("显示人脸框与五点"), face);
    m_faceDetection->setObjectName(QStringLiteral("faceDetectionCheck"));
    faceLayout->addWidget(m_faceDetection);
    faceLayout->addWidget(new QLabel(tr("引擎文件（.engine）"), face));
    auto *engineRow = new QHBoxLayout;
    m_faceEngine = new QLineEdit(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models/scrfd_10g.engine")), face);
    m_faceEngine->setObjectName(QStringLiteral("faceEngineEdit"));
    m_faceEngine->setMinimumWidth(0);
    m_faceEngine->setToolTip(QDir::toNativeSeparators(m_faceEngine->text()));
    connect(m_faceEngine, &QLineEdit::textChanged, m_faceEngine, &QWidget::setToolTip);
    m_chooseEngine = new QPushButton(tr("选择引擎"), face);
    m_chooseEngine->setObjectName(QStringLiteral("chooseFaceEngineButton"));
    engineRow->addWidget(m_faceEngine, 1); engineRow->addWidget(m_chooseEngine);
    faceLayout->addLayout(engineRow);
    m_faceStatus = new QLabel(tr("人脸检测未启用"), face);
    m_faceStatus->setObjectName(QStringLiteral("faceStatusLabel"));
    m_faceStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_faceStatus->setWordWrap(true);
    m_faceStatus->setMinimumHeight(m_faceStatus->fontMetrics().lineSpacing() * 2);
    faceLayout->addWidget(m_faceStatus);
    connect(m_chooseEngine, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("选择本机优化工程生成的引擎"), m_faceEngine->text(), tr("TensorRT 引擎 (*.engine)"));
        if (!path.isEmpty()) m_faceEngine->setText(QDir::toNativeSeparators(path));
    });
    connect(m_refreshDevices, &QPushButton::clicked, this, &MainWindow::refreshDevicesRequested);
    auto *record = new QGroupBox(tr("本地编码录制"), panel);
    record->setObjectName(QStringLiteral("recordingGroup"));
    auto *recordLayout = new QVBoxLayout(record);
    recordLayout->setContentsMargins(16, 24, 16, 16); recordLayout->setSpacing(12);
    panels->addWidget(record);
    m_recording = new QCheckBox(tr("采集时保存 MP4（H.264 NVENC + AAC）"), record);
    m_recording->setObjectName(QStringLiteral("recordingCheck"));
    recordLayout->addWidget(m_recording);
    recordLayout->addWidget(new QLabel(tr("保存目录（每轮自动生成新文件名）"), record));
    auto *recordRow = new QHBoxLayout;
    const auto movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    m_recordDirectory = new QLineEdit(QDir(movies.isEmpty() ? QDir::homePath() : movies).filePath(QStringLiteral("OBS")), record);
    m_recordDirectory->setObjectName(QStringLiteral("recordingDirectoryEdit"));
    m_recordDirectory->setMinimumWidth(0);
    m_recordDirectory->setToolTip(QDir::toNativeSeparators(m_recordDirectory->text()));
    connect(m_recordDirectory, &QLineEdit::textChanged, m_recordDirectory, &QWidget::setToolTip);
    m_chooseRecordDirectory = new QPushButton(tr("选择目录"), record);
    m_chooseRecordDirectory->setObjectName(QStringLiteral("chooseRecordingDirectoryButton"));
    recordRow->addWidget(m_recordDirectory, 1); recordRow->addWidget(m_chooseRecordDirectory);
    recordLayout->addLayout(recordRow);
    m_recordStatus = new QLabel(tr("录制未启用；勾选后与采集一起启停，停止后写完 MP4 索引。"), record);
    m_recordStatus->setObjectName(QStringLiteral("recordingStatusLabel"));
    m_recordStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_recordStatus->setWordWrap(true);
    m_recordStatus->setMinimumHeight(m_recordStatus->fontMetrics().lineSpacing() * 2);
    recordLayout->addWidget(m_recordStatus);
    connect(m_chooseRecordDirectory, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("选择录制保存目录"), m_recordDirectory->text());
        if (!path.isEmpty()) m_recordDirectory->setText(QDir::toNativeSeparators(path));
    });
    connect(m_stopCapture, &QPushButton::clicked, this, &MainWindow::stopCaptureRequested);
    connect(m_startCapture, &QPushButton::clicked, this, [this] {
        emit captureRequested(selectedCaptureSources());
    });
    setCaptureSources({});
    applyCaptureState(false, false, false, tr("正在准备采集模块"), {});
    return panel;
}

QWidget *MainWindow::buildPlayerPanel(QWidget *parent)
{
    auto *group = new QGroupBox(tr("媒体选择"), parent);
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(16, 24, 16, 16);
    layout->setSpacing(14);
    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel(tr("播放模式"), group));
    m_mode = new QComboBox(group);
    m_mode->setObjectName(QStringLiteral("modeCombo"));
    m_mode->addItems({tr("直播"), tr("点播")});
    modeRow->addWidget(m_mode);
    modeRow->addStretch();
    layout->addLayout(modeRow);
    auto *modes = new QStackedWidget(group);
    auto *live = new QWidget(modes);
    auto *liveLayout = new QVBoxLayout(live);
    liveLayout->setContentsMargins(0, 0, 0, 0);
    m_rooms = new QListWidget(live);
    m_rooms->setObjectName(QStringLiteral("roomList"));
    m_rooms->setMinimumHeight(160);
    liveLayout->addWidget(m_rooms, 1);
    auto *actions = new QHBoxLayout;
    m_refresh = new QPushButton(tr("刷新房间"), live);
    m_refresh->setObjectName(QStringLiteral("refreshRoomsButton"));
    m_join = new QPushButton(tr("加入房间"), live);
    m_join->setObjectName(QStringLiteral("joinRoomButton"));
    actions->addWidget(m_refresh);
    actions->addWidget(m_join);
    actions->addStretch();
    liveLayout->addLayout(actions);
    auto *livePlaybackActions = new QHBoxLayout;
    m_playLive = new QPushButton(tr("播放直播 / 重试"), live); m_playLive->setObjectName(QStringLiteral("playLiveButton"));
    m_stopLivePlayback = new QPushButton(tr("停止播放"), live); m_stopLivePlayback->setObjectName(QStringLiteral("stopLivePlaybackButton"));
    m_playLive->setEnabled(false); m_stopLivePlayback->setEnabled(false);
    livePlaybackActions->addWidget(m_playLive); livePlaybackActions->addWidget(m_stopLivePlayback); livePlaybackActions->addStretch();
    liveLayout->addLayout(livePlaybackActions);
    m_livePlaybackStatus = new QLabel(tr("加入已开播房间后自动播放，未开播时等待主播。"), live);
    m_livePlaybackStatus->setObjectName(QStringLiteral("livePlaybackStatusLabel"));
    m_livePlaybackStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum); m_livePlaybackStatus->setWordWrap(true);
    liveLayout->addWidget(m_livePlaybackStatus);
    connect(m_playLive, &QPushButton::clicked, this, &MainWindow::playLiveRequested);
    connect(m_stopLivePlayback, &QPushButton::clicked, this, &MainWindow::stopPlaybackRequested);
    modes->addWidget(live);
    auto *vod = new QWidget(modes);
    auto *form = new QVBoxLayout(vod);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(12);
    m_vodSource = new QComboBox(vod);
    m_vodSource->setObjectName(QStringLiteral("vodSourceCombo"));
    m_vodSource->addItems({tr("本地文件"), tr("在线 HTTP / HTTPS 地址")});
    form->addWidget(m_vodSource);
    auto *inputs = new QStackedWidget(vod);
    auto *localPage = new QWidget(inputs);
    auto *localLayout = new QVBoxLayout(localPage); localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->addWidget(new QLabel(tr("媒体文件"), localPage));
    m_localMedia = new QLineEdit(localPage);
    m_localMedia->setObjectName(QStringLiteral("localMediaEdit"));
    m_localMedia->setMinimumWidth(0);
    m_localMedia->setPlaceholderText(tr("选择或粘贴本地音视频文件路径"));
    m_browseMedia = new QPushButton(tr("选择文件"), localPage);
    m_browseMedia->setObjectName(QStringLiteral("browseMediaButton"));
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_localMedia, 1); fileRow->addWidget(m_browseMedia);
    localLayout->addLayout(fileRow); inputs->addWidget(localPage);
    auto *urlPage = new QWidget(inputs);
    auto *urlLayout = new QVBoxLayout(urlPage); urlLayout->setContentsMargins(0, 0, 0, 0);
    urlLayout->addWidget(new QLabel(tr("在线媒体地址"), urlPage));
    m_mediaUrl = new QLineEdit(urlPage);
    m_mediaUrl->setObjectName(QStringLiteral("mediaUrlEdit"));
    m_mediaUrl->setMinimumWidth(0); m_mediaUrl->setPlaceholderText(tr("https://…/video.mp4"));
    urlLayout->addWidget(m_mediaUrl); inputs->addWidget(urlPage); form->addWidget(inputs);
    connect(m_vodSource, &QComboBox::currentIndexChanged, inputs, &QStackedWidget::setCurrentIndex);
    auto *soundRow = new QHBoxLayout;
    m_playAudio = new QCheckBox(tr("播放声音"), group); m_playAudio->setChecked(true);
    m_playAudio->setObjectName(QStringLiteral("playAudioCheck"));
    m_playVolume = new QSpinBox(group); m_playVolume->setRange(0, 100); m_playVolume->setValue(50); m_playVolume->setSuffix(QStringLiteral("%"));
    m_playVolume->setObjectName(QStringLiteral("playbackVolumeSpin"));
    soundRow->addWidget(m_playAudio); soundRow->addStretch();
    soundRow->addWidget(new QLabel(tr("音量"), group)); soundRow->addWidget(m_playVolume);
    auto *playRow = new QHBoxLayout;
    m_playMedia = new QPushButton(tr("播放"), vod); m_playMedia->setObjectName(QStringLiteral("playMediaButton"));
    m_stopMedia = new QPushButton(tr("停止"), vod); m_stopMedia->setObjectName(QStringLiteral("stopMediaButton"));
    m_playMedia->setEnabled(false); m_stopMedia->setEnabled(false);
    playRow->addWidget(m_playMedia); playRow->addWidget(m_stopMedia); playRow->addStretch(); form->addLayout(playRow);
    m_playProgress = new QProgressBar(vod); m_playProgress->setRange(0, 1000); m_playProgress->setValue(0);
    // 音频电平条的全局样式只有8px；点播进度显示时间，按字体单独给足高度。
    m_playProgress->setStyleSheet(QStringLiteral("QProgressBar { min-height:%1px; max-height:%1px; text-align:center; }")
        .arg(m_playProgress->fontMetrics().lineSpacing() + 12));
    m_playProgress->setFormat(tr("尚未播放")); m_playProgress->setObjectName(QStringLiteral("playbackProgress")); form->addWidget(m_playProgress);
    m_playbackStatus = new QLabel(tr("选择媒体后播放；画面在独立窗口，关闭画面不停止声音。"), vod);
    m_playbackStatus->setObjectName(QStringLiteral("playbackStatusLabel"));
    m_playbackStatus->setWordWrap(true); m_playbackStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_playbackStatus->setMinimumHeight(m_playbackStatus->fontMetrics().lineSpacing() * 2); form->addWidget(m_playbackStatus);
    connect(m_browseMedia, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("选择音视频文件"));
        if (!path.isEmpty()) m_localMedia->setText(QDir::toNativeSeparators(path));
    });
    connect(m_playMedia, &QPushButton::clicked, this, [this] {
        emit playbackRequested(m_vodSource->currentIndex() == 0 ? m_localMedia->text().trimmed() : m_mediaUrl->text().trimmed(), m_playAudio->isChecked());
    });
    connect(m_stopMedia, &QPushButton::clicked, this, &MainWindow::stopPlaybackRequested);
    connect(m_playVolume, &QSpinBox::valueChanged, this, [this](int value) { emit playbackVolumeChanged(value / 100.0f); });
    modes->addWidget(vod);
    layout->addWidget(modes);
    layout->addLayout(soundRow); // 两种模式共用输出声音/音量，避免直播控件隐藏在点播页面。
    connect(m_mode, &QComboBox::currentIndexChanged, modes, &QStackedWidget::setCurrentIndex);
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index == 1 && m_inRoom) emit leaveRoomRequested();
        emit stopPlaybackRequested();
        m_playMedia->setEnabled(index == 1 && m_loggedIn && !m_playbackActive && !m_busy && !m_inRoom);
        m_join->setEnabled(index == 0 && m_ready && !m_busy && !m_inRoom && m_rooms->currentItem());
    });
    connect(m_refresh, &QPushButton::clicked, this, &MainWindow::refreshRoomsRequested);
    connect(m_join, &QPushButton::clicked, this, [this] {
        if (auto *item = m_rooms->currentItem()) emit joinRoomRequested(item->data(Qt::UserRole).toString());
    });
    connect(m_rooms, &QListWidget::itemSelectionChanged, this, [this] {
        m_join->setEnabled(m_ready && !m_busy && !m_inRoom && m_mode->currentIndex() == 0 && m_rooms->currentItem());
    });
    return group;
}

void MainWindow::applySessionState(bool ready, bool busy, bool connecting, const QString &message,
                                  const QJsonObject &room, const QJsonArray &rooms)
{
    m_ready = ready;
    m_busy = busy;
    m_inRoom = !room.isEmpty();
    m_roomStreaming = room.value(QStringLiteral("streaming")).toBool();
    m_signalStatus->setText(message);
    m_reconnect->setEnabled(m_loggedIn && !ready && !connecting);
    m_leave->setEnabled(ready && !busy && m_inRoom);
    m_roomInfo->setText(m_inRoom ? tr("房间 %1 · %2\n%3 · 观众 %4")
        .arg(room.value(QStringLiteral("roomId")).toString(), room.value(QStringLiteral("title")).toString(),
             room.value(QStringLiteral("streaming")).toBool() ? tr("直播中") : tr("尚未开播"))
        .arg(room.value(QStringLiteral("viewers")).toInt()) : tr("尚未加入房间"));
    if (m_create) {
        m_create->setEnabled(ready && !busy && !m_inRoom);
        m_roomTitle->setEnabled(ready && !busy && !m_inRoom);
    }
    if (m_rooms) {
        QString selected;
        if (auto *item = m_rooms->currentItem()) selected = item->data(Qt::UserRole).toString();
        m_rooms->clear();
        for (const auto &value : rooms) {
            const auto entry = value.toObject();
            auto *item = new QListWidgetItem(tr("%1 · %2 · 观众 %3").arg(entry.value(QStringLiteral("title")).toString(),
                entry.value(QStringLiteral("streaming")).toBool() ? tr("直播中") : tr("等待开播"))
                .arg(entry.value(QStringLiteral("viewers")).toInt()), m_rooms);
            item->setData(Qt::UserRole, entry.value(QStringLiteral("roomId")).toString());
            if (item->data(Qt::UserRole).toString() == selected) m_rooms->setCurrentItem(item);
        }
        m_refresh->setEnabled(ready && !busy);
        m_join->setEnabled(ready && !busy && !m_inRoom && m_mode->currentIndex() == 0 && m_rooms->currentItem());
        m_mode->setEnabled(!busy);
        m_playMedia->setEnabled(m_loggedIn && vodMode() && !m_playbackActive && !busy && !m_inRoom);
        m_playLive->setEnabled(m_loggedIn && !vodMode() && m_inRoom && m_roomStreaming && !m_playbackActive && !busy);
    }
}

void MainWindow::applyLoginState(const QString &message, bool busy, bool loggedIn)
{
    m_loggedIn = loggedIn;
    m_showPreview->setEnabled(loggedIn);
    if (!loggedIn) m_preview->setFrame({});
    ui->statusLabel->setText(message);
    for (auto *edit : {ui->endpointEdit, ui->accountEdit, ui->passwordEdit, m_signalHost}) edit->setEnabled(!busy && !loggedIn);
    m_signalPort->setEnabled(!busy && !loggedIn);
    ui->loginButton->setEnabled(!busy && !loggedIn);
    ui->cancelButton->setEnabled(busy);
    ui->logoutButton->setEnabled(loggedIn);
    m_reconnect->setEnabled(loggedIn && !m_ready);
    m_welcome->setText(message);
    m_pages->setCurrentIndex(loggedIn ? 1 : 0);
    if (m_playMedia) m_playMedia->setEnabled(loggedIn && vodMode() && !m_playbackActive && !m_busy && !m_inRoom);
    setWindowTitle(tr("OBS %1 · %2").arg(m_role == csn::ClientRole::Publisher ? tr("推流端") : tr("播放端"), loggedIn ? tr("工作台") : tr("登录")));
}
QString MainWindow::signalHost() const { return m_signalHost->text().trimmed(); }
quint16 MainWindow::signalPort() const { return quint16(m_signalPort->value()); }
void MainWindow::setSignalEndpoint(const QString &host, quint16 port) { m_signalHost->setText(host); m_signalPort->setValue(port); }
void MainWindow::setLoginEndpoint(const QUrl &endpoint) { ui->endpointEdit->setText(endpoint.toString()); }
void MainWindow::closeEvent(QCloseEvent *event) { m_preview->setFrame({}); emit logoutRequested(); QMainWindow::closeEvent(event); }
MainWindow::~MainWindow() { delete ui; }

void MainWindow::setCaptureSources(const QList<csn::CaptureSource> &sources)
{
    if (!m_videoSource) return;
    for (auto *combo : {m_videoSource, m_micSource, m_systemSource}) {
        const auto old = combo->currentData().value<csn::CaptureSource>();
        QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(tr("不采集")); // 默认关闭采集，用户明确选择后才打开设备。
        for (const auto &source : sources) {
            const bool microphone = source.kind == csn::CaptureSource::Kind::Microphone;
            const bool system = source.kind == csn::CaptureSource::Kind::Loopback;
            if ((combo == m_micSource && microphone) || (combo == m_systemSource && system)
                || (combo == m_videoSource && !microphone && !system)) {
                combo->addItem(source.name, QVariant::fromValue(source));
                if (source.kind == old.kind && source.id == old.id && !old.id.isEmpty()) combo->setCurrentIndex(combo->count() - 1);
            }
        }
    }
}
void MainWindow::applyCaptureState(bool canStart, bool active, bool enumerating,
                                   const QString &devices, const QStringList &messages)
{
    if (!m_videoSource) return;
    for (auto *combo : {m_videoSource, m_micSource, m_systemSource}) combo->setEnabled(canStart);
    m_startCapture->setEnabled(canStart);
    m_stopCapture->setEnabled(active);
    m_refreshDevices->setEnabled(!active && !enumerating);
    m_faceDetection->setEnabled(canStart);
    m_faceEngine->setEnabled(canStart);
    m_recording->setEnabled(canStart);
    m_recordDirectory->setEnabled(canStart);
    m_chooseRecordDirectory->setEnabled(canStart);
    m_chooseEngine->setEnabled(canStart);
    for (int index = 0; index < 2; ++index) {
        m_audioGain[index]->setEnabled(canStart || active);
        m_audioMute[index]->setEnabled(canStart || active);
    }
    QStringList text;
    text << (enumerating ? tr("正在枚举设备…") : devices);
    const QStringList names{tr("视频"), tr("麦克风"), tr("系统声音")};
    for (int index = 0; index < std::min(3, int(messages.size())); ++index)
        if (!messages[index].isEmpty()) text << names[index] + QStringLiteral("：") + messages[index];
    m_captureStatus->setText(text.join(QLatin1Char('\n')));
}
void MainWindow::showCapturePreview(const csn::VideoFrame &frame)
{
    if (m_role == csn::ClientRole::Publisher && m_loggedIn) m_preview->setFrame(frame);
}
bool MainWindow::faceDetectionEnabled() const { return m_faceDetection && m_faceDetection->isChecked(); }
QString MainWindow::faceEnginePath() const { return m_faceEngine ? m_faceEngine->text().trimmed() : QString(); }
void MainWindow::showFacePreview(const csn::FaceFrame &frame)
{
    if (m_role == csn::ClientRole::Publisher && m_loggedIn) m_preview->setFrame(frame.video, frame.faces);
}
void MainWindow::showFaceStatus(const QString &message) { if (m_faceStatus) m_faceStatus->setText(message); }
void MainWindow::showAudioLevels(float microphone, float system)
{
    if (!m_micLevel) return;
    // dBFS 映射到 -60..0dB，低声说话也能看到变化；不播放回采声音，避免声反馈。
    const auto level = [](float value) { return value > 0 ? int(std::clamp((20.0f * std::log10(value) + 60.0f) / 60.0f, 0.0f, 1.0f) * 100) : 0; };
    m_micLevel->setValue(level(microphone)); m_systemLevel->setValue(level(system));
}

void MainWindow::showMixedAudio(float level, const QString &message)
{
    if (!m_mixedLevel) return;
    // 与输入电平使用同一 -60～0dBFS 标尺，低电平混音也能直观看到。
    const int value = level > 0 ? int(std::clamp((20.0f * std::log10(level) + 60.0f) / 60.0f, 0.0f, 1.0f) * 100) : 0;
    m_mixedLevel->setValue(value);
    m_mixStatus->setText(message);
}
void MainWindow::showMediaSyncStatus(const QString &message) { if (m_syncStatus) m_syncStatus->setText(message); }
bool MainWindow::recordingEnabled() const { return m_recording && m_recording->isChecked(); }
QString MainWindow::recordingDirectory() const { return m_recordDirectory ? m_recordDirectory->text().trimmed() : QString(); }
void MainWindow::showRecordingStatus(const QString &message) { if (m_recordStatus) m_recordStatus->setText(message); }
bool MainWindow::vodMode() const { return m_mode && m_mode->currentIndex() == 1; }
void MainWindow::showPlaybackPreview(const csn::VideoFrame &frame)
{
    if (!frame.texture || (m_role == csn::ClientRole::Player && m_loggedIn && vodMode())) m_preview->setFrame(frame);
}
void MainWindow::applyPlaybackState(bool active, qint64 positionUs, qint64 durationUs, const QString &message)
{
    if (!m_playMedia) return;
    m_playbackActive = active;
    m_playLive->setEnabled(m_loggedIn && !vodMode() && m_inRoom && m_roomStreaming && !active && !m_busy);
    m_stopLivePlayback->setEnabled(active && !vodMode());
    m_playMedia->setEnabled(m_loggedIn && vodMode() && !active && !m_busy && !m_inRoom);
    m_stopMedia->setEnabled(active); m_vodSource->setEnabled(!active); m_localMedia->setEnabled(!active);
    m_mediaUrl->setEnabled(!active); m_browseMedia->setEnabled(!active); m_playAudio->setEnabled(!active);
    const auto time = [](qint64 us) {
        const qint64 seconds = std::max<qint64>(0, us / 1000000);
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
            .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    };
    const auto position = durationUs > 0 ? std::min(positionUs, durationUs) : positionUs;
    m_playProgress->setValue(durationUs > 0 ? int(std::clamp<double>(double(position) / durationUs * 1000, 0, 1000)) : 0);
    m_playProgress->setFormat(durationUs > 0 ? time(position) + QStringLiteral(" / ") + time(durationUs) : time(position));
    m_playbackStatus->setText(message);
    m_livePlaybackStatus->setText(message);
}
QList<csn::CaptureSource> MainWindow::selectedCaptureSources() const
{
    QList<csn::CaptureSource> selected;
    if (m_videoSource) for (auto *combo : {m_videoSource, m_micSource, m_systemSource})
        if (combo->currentData().isValid()) selected.append(combo->currentData().value<csn::CaptureSource>());
    return selected;
}
bool MainWindow::playbackAudioEnabled() const { return m_playAudio && m_playAudio->isChecked(); }
void MainWindow::applyLiveOutputState(bool canStart, bool active, const QString &message)
{
    if (!m_startLive) return;
    m_startLive->setEnabled(canStart); m_stopLive->setEnabled(active); m_liveOutputStatus->setText(message);
}
