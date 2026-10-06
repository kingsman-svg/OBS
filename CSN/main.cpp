#include "mainwindow.h"
#include "login/LoginController.h"
#include "login/LoginModel.h"
#include "network/HttpClient.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
    csn::HttpClient http;
    csn::LoginModel model;
    csn::LoginController controller(&w, &model, &http);
    w.show();
    return QApplication::exec();
}
