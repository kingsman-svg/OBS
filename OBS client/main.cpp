#include "mainwindow.h"
#include "LoginController.h"
#include "LoginModel.h"
#include "HttpClient.h"
#include "LocalAuthServer.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    csn::LocalAuthServer localAuth;
    MainWindow w;
    csn::HttpClient http;
    csn::LoginModel model;
    csn::LoginController controller(&w, &model, &http);
    if (localAuth.start())
        w.setLoginEndpoint(localAuth.endpoint());
    else
        w.applyLoginState(QStringLiteral("本地登录服务启动失败：") + localAuth.errorString(), false, false);
    w.show();
    return QApplication::exec();
}
