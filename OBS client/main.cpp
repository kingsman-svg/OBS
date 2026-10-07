#include "HttpClient.h"
#include "LoginController.h"
#include "LoginModel.h"
#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "mainwindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("CourseStudioNext"));
#ifdef OBS_PLAYER
    constexpr auto role = csn::ClientRole::Player;
    app.setApplicationName(QStringLiteral("OBS_Player"));
#else
    constexpr auto role = csn::ClientRole::Publisher;
    app.setApplicationName(QStringLiteral("OBS_Publisher"));
#endif
    MainWindow view(role);
    csn::HttpClient http;
    csn::LoginModel login;
    csn::LoginController loginController(&view, &login, &http);
    csn::SignalClient signal;
    csn::SessionModel session;
    csn::SessionController sessionController(&view, &login, &session, &signal);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &signal, [&signal] { signal.disconnectFromServer(); });
    view.show();
    return app.exec();
}
