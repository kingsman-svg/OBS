#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QUrl>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void applyLoginState(const QString &message, bool busy, bool loggedIn);

signals:
    void loginRequested(const QUrl &endpoint, const QString &account, const QString &password);
    void cancelRequested();
    void logoutRequested();

private:
    Ui::MainWindow *ui;
};
#endif // MAINWINDOW_H
