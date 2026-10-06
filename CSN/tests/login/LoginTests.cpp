#include "login/LoginController.h"
#include "login/LoginModel.h"
#include "mainwindow.h"
#include "network/HttpClient.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <functional>
#include <cstdio>

namespace {
const QByteArray session = R"({"accessToken":"fixture-token","expiresIn":3600,"user":{"id":"u1","displayName":"测试用户"}})";

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}
#define CHECK(condition) \
    do { if (!(condition)) { qCritical("FAIL line %d: %s", __LINE__, #condition); return false; } } while (false)

// Local HTTP fixture: consumes an entire POST, optionally fragments its response.
void serve(QTcpServer &server, QByteArray body, int status = 200, bool hang = false,
           QByteArray *captured = nullptr, bool fragment = false)
{
    QObject::connect(&server, &QTcpServer::newConnection, &server,
                     [&server, body, status, hang, captured, fragment] {
        while (server.hasPendingConnections()) {
            QTcpSocket *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                             [socket, body, status, hang, captured, fragment] {
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (socket->property("handled").toBool())
                    return;
                const qsizetype end = request.indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                int length = 0;
                for (const QByteArray &line : request.left(end).split('\n')) {
                    if (line.trimmed().toLower().startsWith("content-length:"))
                        length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                }
                if (request.size() < end + 4 + length)
                    return;
                socket->setProperty("handled", true);
                if (captured)
                    *captured = request;
                if (hang)
                    return;
                const QByteArray response = "HTTP/1.1 " + QByteArray::number(status)
                    + " Fixture\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                if (fragment) {
                    const qsizetype cut = response.size() / 2;
                    socket->write(response.left(cut));
                    QTimer::singleShot(20, socket, [socket, response, cut] {
                        socket->write(response.mid(cut));
                        socket->disconnectFromHost();
                    });
                } else {
                    socket->write(response);
                    socket->disconnectFromHost();
                }
            });
        }
    });
}
QUrl endpoint(const QTcpServer &server)
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1/auth/login").arg(server.serverPort()));
}

bool httpSuccessAndIdentity()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    QByteArray request;
    serve(server, session, 200, false, &request, true);
    csn::HttpClient http;
    QSignalSpy success(&http, &csn::HttpClient::succeeded);
    QSignalSpy failure(&http, &csn::HttpClient::failed);
    const QJsonObject credentials{{QStringLiteral("account"), QStringLiteral("alice")},
                                  {QStringLiteral("password"), QStringLiteral("fixture-password")}};
    const quint64 a = http.postJson(endpoint(server), credentials);
    const quint64 b = http.postJson(endpoint(server), credentials);
    CHECK(a != b);
    CHECK(waitUntil([&] { return success.count() == 2; }));
    CHECK(failure.isEmpty());
    CHECK(success[0][0].toULongLong() != success[1][0].toULongLong());
    CHECK(request.startsWith("POST /auth/login HTTP/1.1\r\n"));
    const QByteArray body = request.mid(request.indexOf("\r\n\r\n") + 4);
    CHECK(QJsonDocument::fromJson(body).object() == credentials);
    return true;
}
bool httpErrorsAndTimeout()
{
    csn::HttpClient http;
    QSignalSpy failure(&http, &csn::HttpClient::failed);
    const quint64 invalid = http.postJson(QUrl(QStringLiteral("file:///tmp/login")), {});
    CHECK(failure.isEmpty());
    CHECK(waitUntil([&] { return failure.count() == 1; }));
    CHECK(failure[0][0].toULongLong() == invalid);

    QTcpServer badJson;
    CHECK(badJson.listen(QHostAddress::LocalHost, 0));
    serve(badJson, QByteArrayLiteral("not-json"));
    http.postJson(endpoint(badJson), {});
    CHECK(waitUntil([&] { return failure.count() == 2; }));

    QTcpServer denied;
    CHECK(denied.listen(QHostAddress::LocalHost, 0));
    serve(denied, QByteArrayLiteral("{\"error\":{\"code\":\"INVALID_CREDENTIALS\",\"message\":\"Wrong credentials\"}}"), 401);
    http.postJson(endpoint(denied), {});
    CHECK(waitUntil([&] { return failure.count() == 3; }));
    CHECK(failure[2][2].toInt() == 401);
    CHECK(failure[2][1].toString().contains(QStringLiteral("Wrong credentials")));

    QTcpServer stalled;
    CHECK(stalled.listen(QHostAddress::LocalHost, 0));
    serve(stalled, {}, 200, true);
    http.postJson(endpoint(stalled), {}, 50);
    CHECK(waitUntil([&] { return failure.count() == 4; }));
    CHECK(failure[3][1].toString().contains(QStringLiteral("timed out")));
    return true;
}
bool cancelAndResponseLimit()
{
    csn::HttpClient http;
    QSignalSpy success(&http, &csn::HttpClient::succeeded);
    QSignalSpy failure(&http, &csn::HttpClient::failed);
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    serve(server, session, 200, false, nullptr, true);
    const quint64 id = http.postJson(endpoint(server), {}, 50);
    http.cancel(id);
    const quint64 invalid = http.postJson(QUrl(), {});
    http.cancel(invalid);
    QTest::qWait(90);
    CHECK(success.isEmpty() && failure.isEmpty());

    QTcpServer large;
    CHECK(large.listen(QHostAddress::LocalHost, 0));
    serve(large, QByteArray(int(csn::HttpClient::MaxResponseBytes + 2), 'x'));
    http.postJson(endpoint(large), {});
    CHECK(waitUntil([&] { return failure.count() == 1; }));
    CHECK(failure[0][1].toString().contains(QStringLiteral("1 MiB")));
    return true;
}
bool mvcLoginAndLogout()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    QByteArray request;
    serve(server, session, 200, false, &request, true);
    MainWindow view;
    csn::HttpClient http;
    csn::LoginModel model;
    csn::LoginController controller(&view, &model, &http);
    view.show();
    auto *address = view.findChild<QLineEdit *>(QStringLiteral("endpointEdit"));
    auto *account = view.findChild<QLineEdit *>(QStringLiteral("accountEdit"));
    auto *password = view.findChild<QLineEdit *>(QStringLiteral("passwordEdit"));
    auto *login = view.findChild<QPushButton *>(QStringLiteral("loginButton"));
    auto *logout = view.findChild<QPushButton *>(QStringLiteral("logoutButton"));
    CHECK(address && account && password && login && logout);
    address->setText(endpoint(server).toString());
    account->setText(QStringLiteral("alice"));
    password->setText(QStringLiteral("fixture-password"));
    QTest::mouseClick(login, Qt::LeftButton);
    CHECK(model.state() == csn::LoginModel::State::LoggingIn);
    CHECK(password->text().isEmpty());
    CHECK(!login->isEnabled());
    CHECK(waitUntil([&] { return model.state() == csn::LoginModel::State::LoggedIn; }));
    CHECK(model.accessToken() == QStringLiteral("fixture-token"));
    CHECK(model.userId() == QStringLiteral("u1"));
    CHECK(model.expiresAt() > QDateTime::currentDateTimeUtc());
    CHECK(logout->isEnabled());
    QTest::mouseClick(logout, Qt::LeftButton);
    CHECK(model.state() == csn::LoginModel::State::LoggedOut);
    CHECK(model.accessToken().isEmpty() && model.userId().isEmpty());
    CHECK(login->isEnabled());
    if (!qEnvironmentVariableIsEmpty("CSN_SCREENSHOT_PATH"))
        CHECK(view.grab().save(qEnvironmentVariable("CSN_SCREENSHOT_PATH")));
    return true;
}
bool mvcFailureAndCancel()
{
    QTcpServer bad;
    CHECK(bad.listen(QHostAddress::LocalHost, 0));
    serve(bad, QByteArrayLiteral("{\"accessToken\":\"incomplete\"}"));
    MainWindow view;
    csn::HttpClient http;
    csn::LoginModel model;
    csn::LoginController controller(&view, &model, &http);
    controller.login(endpoint(bad), {}, QStringLiteral("x"));
    CHECK(model.state() == csn::LoginModel::State::LoggedOut);
    controller.login(endpoint(bad), QStringLiteral("alice"), QStringLiteral("x"));
    CHECK(waitUntil([&] { return model.state() == csn::LoginModel::State::LoggedOut; }));
    CHECK(model.accessToken().isEmpty());

    QTcpServer stalled;
    CHECK(stalled.listen(QHostAddress::LocalHost, 0));
    serve(stalled, {}, 200, true);
    controller.login(endpoint(stalled), QStringLiteral("alice"), QStringLiteral("x"));
    CHECK(model.state() == csn::LoginModel::State::LoggingIn);
    controller.cancel();
    QTest::qWait(30);
    CHECK(model.state() == csn::LoginModel::State::LoggedOut);
    CHECK(model.accessToken().isEmpty());
    return true;
}
} // namespace
int main(int argc, char *argv[])
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const QByteArray bytes = message.toUtf8();
        std::fprintf(stderr, "%s\n", bytes.constData());
        std::fflush(stderr);
    });
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    // The offscreen platform does not discover Windows fonts automatically.
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (fontId >= 0)
        application.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).constFirst(), 10));
#endif
    int failures = 0;
    const auto run = [&failures](const char *name, bool (*test)()) {
        const bool passed = test();
        qInfo("%s %s", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    run("httpSuccessAndIdentity", httpSuccessAndIdentity);
    run("httpErrorsAndTimeout", httpErrorsAndTimeout);
    run("cancelAndResponseLimit", cancelAndResponseLimit);
    run("mvcLoginAndLogout", mvcLoginAndLogout);
    run("mvcFailureAndCancel", mvcFailureAndCancel);
    return failures == 0 ? 0 : 1;
}
