#include "mainwindow.h"
#include <QProgressBar>
#include <QGridLayout>
#include "PreviewWindow.h"
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>
#include "ui_mainwindow.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
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
    ui->formLayout->addRow(tr("信令主机"), m_signalHost);
    ui->formLayout->addRow(tr("信令端口"), m_signalPort);
    auto *loginForm = takeCentralWidget();
    loginForm->setMinimumWidth(480);
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
        role == csn::ClientRole::Publisher ? tr("尚无视频画面\n选择视频源后开始采集") : tr("尚无播放画面\n解码模块待接入"), this);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("pages"));
    m_pages->addWidget(loginPage);
    m_pages->addWidget(buildWorkspace());
    connect(m_preview, &PreviewWindow::failed, this, [this](const QString &message) {
        if (m_captureStatus) m_captureStatus->setText(message);
    });
    setCentralWidget(m_pages);
    setStyleSheet(QStringLiteral(
        "QMainWindow { background:#f3f5f9; } QLabel { color:#26354a; }"
        "QLineEdit,QSpinBox,QComboBox { padding:6px; min-height:20px; background:white; border:1px solid #ccd5e1; border-radius:5px; }"
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
    m_signalStatus->setWordWrap(true);
    m_signalStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    }
    auto *membership = new QHBoxLayout;
    m_roomInfo = new QLabel(room);
    m_roomInfo->setObjectName(QStringLiteral("roomInfoLabel"));
    m_roomInfo->setWordWrap(true);
    m_roomInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    auto *capture = new QGroupBox(tr("音视频采集"), parent);
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
    for (auto *level : {m_micLevel, m_systemLevel}) {
        level->setRange(0, 100); level->setValue(0); level->setTextVisible(false);
        level->setFixedWidth(72);
    }
    grid->addWidget(new QLabel(tr("视频源"), capture), 0, 0);
    grid->addWidget(m_videoSource, 0, 1, 1, 2);
    grid->addWidget(new QLabel(tr("麦克风"), capture), 1, 0);
    grid->addWidget(m_micSource, 1, 1);
    grid->addWidget(m_micLevel, 1, 2);
    grid->addWidget(new QLabel(tr("系统声音"), capture), 2, 0);
    grid->addWidget(m_systemSource, 2, 1);
    grid->addWidget(m_systemLevel, 2, 2);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);
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
    m_captureStatus->setWordWrap(true);
    m_captureStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_captureStatus->setStyleSheet(QStringLiteral("color:#596b82; font-weight:normal;"));
    layout->addWidget(m_captureStatus);
    auto *hint = new QLabel(tr("首帧自动打开独立画面窗口，关闭画面不会停止采集。"), capture);
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(hint);
    connect(m_refreshDevices, &QPushButton::clicked, this, &MainWindow::refreshDevicesRequested);
    connect(m_stopCapture, &QPushButton::clicked, this, &MainWindow::stopCaptureRequested);
    connect(m_startCapture, &QPushButton::clicked, this, [this] {
        QList<csn::CaptureSource> selected;
        for (auto *combo : {m_videoSource, m_micSource, m_systemSource})
            if (combo->currentData().isValid()) selected.append(combo->currentData().value<csn::CaptureSource>());
        emit captureRequested(selected);
    });
    setCaptureSources({});
    applyCaptureState(false, false, false, tr("正在准备采集模块"), {});
    return capture;
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
    modes->addWidget(live);
    auto *vod = new QWidget(modes);
    auto *form = new QFormLayout(vod);
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(12);
    auto *local = new QLineEdit(vod);
    local->setObjectName(QStringLiteral("localMediaEdit"));
    local->setMinimumWidth(0);
    local->setReadOnly(true);
    local->setPlaceholderText(tr("选择本地音视频文件"));
    auto *browse = new QPushButton(tr("选择文件"), vod);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(local, 1);
    fileRow->addWidget(browse);
    form->addRow(tr("本地点播"), fileRow);
    auto *url = new QLineEdit(vod);
    url->setObjectName(QStringLiteral("mediaUrlEdit"));
    url->setMinimumWidth(0);
    url->setPlaceholderText(tr("输入在线媒体地址"));
    form->addRow(tr("在线点播"), url);
    auto *play = new QPushButton(tr("播放（解码模块待接入）"), vod);
    play->setEnabled(false);
    form->addRow(play);
    connect(browse, &QPushButton::clicked, this, [this, local] {
        const QString path = QFileDialog::getOpenFileName(this, tr("选择音视频文件"));
        if (!path.isEmpty()) local->setText(path);
    });
    modes->addWidget(vod);
    layout->addWidget(modes);
    connect(m_mode, &QComboBox::currentIndexChanged, modes, &QStackedWidget::setCurrentIndex);
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index == 1 && m_inRoom) emit leaveRoomRequested();
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
void MainWindow::showAudioLevels(float microphone, float system)
{
    if (!m_micLevel) return;
    // dBFS 映射到 -60..0dB，低声说话也能看到变化；不播放回采声音，避免声反馈。
    const auto level = [](float value) { return value > 0 ? int(std::clamp((20.0f * std::log10(value) + 60.0f) / 60.0f, 0.0f, 1.0f) * 100) : 0; };
    m_micLevel->setValue(level(microphone)); m_systemLevel->setValue(level(system));
}
