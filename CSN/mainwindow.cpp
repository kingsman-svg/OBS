#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QStackedWidget>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    auto *loginPage = takeCentralWidget();
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("pages"));
    m_pages->addWidget(loginPage);
    auto *home = new QWidget(m_pages);
    home->setObjectName(QStringLiteral("homePage"));
    auto *layout = new QVBoxLayout(home);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addWidget(new QLabel(tr("CSN · 媒体工作台"), home));
    m_welcome = new QLabel(home);
    m_welcome->setWordWrap(true);
    layout->addWidget(m_welcome);
    auto *next = new QLabel(tr("下一步：摄像头与麦克风采集、预览和设备选择。\n"
                             "后续：屏幕采集、FFmpeg 编解码、GPU 美颜。"), home);
    next->setWordWrap(true);
    layout->addWidget(next);
    layout->addStretch();
    // Move the existing logout control to the home page; keep one logout intent.
    ui->buttonLayout->removeWidget(ui->logoutButton);
    layout->addWidget(ui->logoutButton);
    m_pages->addWidget(home);
    setCentralWidget(m_pages);
    ui->accountEdit->setText(QStringLiteral("root"));
    ui->titleLabel->setText(tr("CSN 登录 · 本地开发账号 root / root"));
    connect(ui->loginButton, &QPushButton::clicked, this, [this] {
        const QUrl endpoint(ui->endpointEdit->text().trimmed());
        const QString account = ui->accountEdit->text();
        const QString password = ui->passwordEdit->text();
        ui->passwordEdit->clear();
        emit loginRequested(endpoint, account, password);
    });
    connect(ui->cancelButton, &QPushButton::clicked, this, &MainWindow::cancelRequested);
    connect(ui->logoutButton, &QPushButton::clicked, this, &MainWindow::logoutRequested);
}

void MainWindow::applyLoginState(const QString &message, bool busy, bool loggedIn)
{
    ui->statusLabel->setText(message);
    ui->endpointEdit->setEnabled(!busy && !loggedIn);
    ui->accountEdit->setEnabled(!busy && !loggedIn);
    ui->passwordEdit->setEnabled(!busy && !loggedIn);
    ui->loginButton->setEnabled(!busy && !loggedIn);
    ui->cancelButton->setEnabled(busy);
    ui->logoutButton->setEnabled(loggedIn);
    m_welcome->setText(message);
    m_pages->setCurrentIndex(loggedIn ? 1 : 0);
    setWindowTitle(loggedIn ? tr("CSN - 媒体工作台") : tr("CSN - 登录"));
}

void MainWindow::setLoginEndpoint(const QUrl &endpoint)
{
    ui->endpointEdit->setText(endpoint.toString());
}

MainWindow::~MainWindow()
{
    delete ui;
}
