#include "mainwindow.h"
#include "./ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
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
}

MainWindow::~MainWindow()
{
    delete ui;
}
