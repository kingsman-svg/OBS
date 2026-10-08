#include "mainwindow.h"
#include <QProgressBar>
#include <QGridLayout>
#include <QPixmap>
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
    resize(900, 650);
    setMinimumSize(720, 560);
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
    auto *loginPage = takeCentralWidget();
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("pages"));
    m_pages->addWidget(loginPage);
    m_pages->addWidget(buildWorkspace());
    setCentralWidget(m_pages);
    setStyleSheet(QStringLiteral(
        "QMainWindow { background:#f5f7fb; } QLineEdit,QSpinBox,QComboBox { padding:7px; }"
        "QPushButton { padding:8px 16px; } QGroupBox { font-weight:600; margin-top:12px; padding-top:14px; }"
        "QLabel { color:#26354a; } QListWidget { background:white; border:1px solid #cbd5e1; }"));
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
    layout->setSpacing(12);
    auto *heading = new QLabel(m_role == csn::ClientRole::Publisher ? tr("OBS · 推流工作台") : tr("OBS · 播放工作台"), home);
    heading->setStyleSheet(QStringLiteral("font-size:24px; font-weight:600;"));
    layout->addWidget(heading);
    m_welcome = new QLabel(home);
    layout->addWidget(m_welcome);
    auto *top = new QHBoxLayout;
    m_signalStatus = new QLabel(home);
    m_signalStatus->setObjectName(QStringLiteral("signalStatusLabel"));
    m_signalStatus->setWordWrap(true);
    top->addWidget(m_signalStatus, 1);
    m_reconnect = new QPushButton(tr("重新连接"), home);
    m_reconnect->setObjectName(QStringLiteral("reconnectButton"));
    top->addWidget(m_reconnect);
    ui->buttonLayout->removeWidget(ui->logoutButton);
    top->addWidget(ui->logoutButton);
    layout->addLayout(top);
    connect(m_reconnect, &QPushButton::clicked, this, &MainWindow::reconnectRequested);
    auto *preview = new QLabel(m_role == csn::ClientRole::Publisher
        ? tr("采集预览\n选择视频源后开始采集")
        : tr("播放画面\n下一步接入 FFmpeg 解码与音视频输出"), home);
    preview->setObjectName(QStringLiteral("previewPlaceholder"));
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(175);
    preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    preview->setStyleSheet(QStringLiteral("background:#152235; color:#d7e4f7; border-radius:8px; font-size:18px;"));
    layout->addWidget(preview, 1);
    m_preview = preview;
    if (m_role == csn::ClientRole::Publisher) {
        auto *capture = new QGroupBox(tr("音视频采集"), home);
        auto *grid = new QGridLayout(capture);
        m_videoSource = new QComboBox(capture);
        m_videoSource->setObjectName(QStringLiteral("videoSourceCombo"));
        m_micSource = new QComboBox(capture);
        m_micSource->setObjectName(QStringLiteral("microphoneCombo"));
        m_systemSource = new QComboBox(capture);
        m_systemSource->setObjectName(QStringLiteral("systemAudioCombo"));
        m_micLevel = new QProgressBar(capture);
        m_micLevel->setObjectName(QStringLiteral("microphoneLevel"));
        m_systemLevel = new QProgressBar(capture);
        m_systemLevel->setObjectName(QStringLiteral("systemAudioLevel"));
        for (auto *level : {m_micLevel, m_systemLevel}) { level->setRange(0, 100); level->setValue(0); level->setTextVisible(false); level->setMaximumWidth(140); }
        grid->addWidget(new QLabel(tr("视频源"), capture), 0, 0);
        grid->addWidget(m_videoSource, 0, 1, 1, 2);
        grid->addWidget(new QLabel(tr("麦克风"), capture), 1, 0);
        grid->addWidget(m_micSource, 1, 1);
        grid->addWidget(m_micLevel, 1, 2);
        grid->addWidget(new QLabel(tr("系统声音"), capture), 2, 0);
        grid->addWidget(m_systemSource, 2, 1);
        grid->addWidget(m_systemLevel, 2, 2);
        grid->setColumnStretch(1, 1);
        auto *actions = new QHBoxLayout;
        m_refreshDevices = new QPushButton(tr("刷新设备"), capture);
        m_refreshDevices->setObjectName(QStringLiteral("refreshDevicesButton"));
        m_startCapture = new QPushButton(tr("开始采集"), capture);
        m_startCapture->setObjectName(QStringLiteral("startCaptureButton"));
        m_stopCapture = new QPushButton(tr("停止采集"), capture);
        m_stopCapture->setObjectName(QStringLiteral("stopCaptureButton"));
        actions->addWidget(m_refreshDevices); actions->addStretch();
        actions->addWidget(m_startCapture); actions->addWidget(m_stopCapture);
        grid->addLayout(actions, 3, 0, 1, 3);
        m_captureStatus = new QLabel(tr("正在枚举设备…"), capture);
        m_captureStatus->setObjectName(QStringLiteral("captureStatusLabel"));
        m_captureStatus->setWordWrap(true);
        grid->addWidget(m_captureStatus, 4, 0, 1, 3);
        layout->addWidget(capture);
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
        auto *group = new QGroupBox(tr("直播房间"), home);
        auto *row = new QHBoxLayout(group);
        m_roomTitle = new QLineEdit(tr("我的直播间"), group);
        m_roomTitle->setObjectName(QStringLiteral("roomTitleEdit"));
        m_roomTitle->setMaxLength(128);
        m_create = new QPushButton(tr("创建房间"), group);
        m_create->setObjectName(QStringLiteral("createRoomButton"));
        row->addWidget(m_roomTitle, 1);
        row->addWidget(m_create);
        connect(m_create, &QPushButton::clicked, this, [this] { emit createRoomRequested(m_roomTitle->text().trimmed()); });
        layout->addWidget(group);
        auto *media = new QLabel(tr("采集可独立预览；GPU 特效、编码及推流将在后续接入。"), home);
        media->setWordWrap(true);
        layout->addWidget(media);
    } else {
        m_mode = new QComboBox(home);
        m_mode->setObjectName(QStringLiteral("modeCombo"));
        m_mode->addItems({tr("直播"), tr("点播")});
        layout->addWidget(m_mode);
        auto *modes = new QStackedWidget(home);
        auto *live = new QWidget(modes);
        auto *liveLayout = new QHBoxLayout(live);
        liveLayout->setContentsMargins(0, 0, 0, 0);
        m_rooms = new QListWidget(live);
        m_rooms->setObjectName(QStringLiteral("roomList"));
        m_rooms->setMinimumHeight(110);
        liveLayout->addWidget(m_rooms, 1);
        auto *actions = new QVBoxLayout;
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
        auto *local = new QLineEdit(vod);
        local->setObjectName(QStringLiteral("localMediaEdit"));
        local->setReadOnly(true);
        local->setPlaceholderText(tr("选择本地音视频文件"));
        auto *browse = new QPushButton(tr("选择文件"), vod);
        auto *fileRow = new QHBoxLayout;
        fileRow->addWidget(local, 1);
        fileRow->addWidget(browse);
        form->addRow(tr("本地点播"), fileRow);
        auto *url = new QLineEdit(vod);
        url->setObjectName(QStringLiteral("mediaUrlEdit"));
        url->setPlaceholderText(tr("在线媒体地址，待接入解码后启用"));
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
    }
    auto *bottom = new QHBoxLayout;
    m_roomInfo = new QLabel(home);
    m_roomInfo->setObjectName(QStringLiteral("roomInfoLabel"));
    m_roomInfo->setWordWrap(true);
    bottom->addWidget(m_roomInfo, 1);
    m_leave = new QPushButton(m_role == csn::ClientRole::Publisher ? tr("关闭房间") : tr("退出房间"), home);
    m_leave->setObjectName(QStringLiteral("leaveRoomButton"));
    bottom->addWidget(m_leave);
    connect(m_leave, &QPushButton::clicked, this, &MainWindow::leaveRoomRequested);
    layout->addLayout(bottom);
    return home;
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
void MainWindow::closeEvent(QCloseEvent *event) { emit logoutRequested(); QMainWindow::closeEvent(event); }
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
void MainWindow::showCapturePreview(const QImage &image)
{
    if (m_role != csn::ClientRole::Publisher) return;
    if (image.isNull()) { m_preview->setPixmap({}); m_preview->setText(tr("采集预览\n选择视频源后开始采集")); }
    else m_preview->setPixmap(QPixmap::fromImage(image).scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
}
void MainWindow::showAudioLevels(float microphone, float system)
{
    if (!m_micLevel) return;
    // dBFS 映射到 -60..0dB，低声说话也能看到变化；不播放回采声音，避免声反馈。
    const auto level = [](float value) { return value > 0 ? int(std::clamp((20.0f * std::log10(value) + 60.0f) / 60.0f, 0.0f, 1.0f) * 100) : 0; };
    m_micLevel->setValue(level(microphone)); m_systemLevel->setValue(level(system));
}
