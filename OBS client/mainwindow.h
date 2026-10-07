#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QUrl>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE
class QStackedWidget;
class QLabel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void applyLoginState(const QString &message, bool busy, bool loggedIn);
    void setLoginEndpoint(const QUrl &endpoint);

signals:
    void loginRequested(const QUrl &endpoint, const QString &account, const QString &password);
    void cancelRequested();
    void logoutRequested();

private:
    Ui::MainWindow *ui;
    QStackedWidget *m_pages;
    QLabel *m_welcome;
};
#endif // MAINWINDOW_H
