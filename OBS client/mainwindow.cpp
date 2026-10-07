#include "mainwindow.h"
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
        ? tr("采集预览\n下一步接入摄像头与麦克风")
        : tr("播放画面\n下一步接入 FFmpeg 解码与音视频输出"), home);
    preview->setObjectName(QStringLiteral("previewPlaceholder"));
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumHeight(175);
    preview->setStyleSheet(QStringLiteral("background:#152235; color:#d7e4f7; border-radius:8px; font-size:18px;"));
    layout->addWidget(preview, 1);
    if (m_role == csn::ClientRole::Publisher) {
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
        auto *media = new QLabel(tr("媒体功能待接入：设备选择 → 音视频采集 → GPU 特效 → 编码 → 推流 / 录制"), home);
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
