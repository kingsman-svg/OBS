#include "HttpClient.h"
#include "LoginController.h"
#include "LoginModel.h"
#include "SessionController.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "mainwindow.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QtEndian>
#include <functional>
#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { qCritical("Check failed at line %d: %s", __LINE__, #condition); return false; } } while (false)
using namespace csn;
namespace {
bool waitUntil(const std::function<bool()> &predicate, int milliseconds = 6000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds) QTest::qWait(5);
    return predicate();
}
QByteArray frame(const QJsonObject &object)
{
    const auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray bytes(4, '\0');
    qToBigEndian<quint32>(quint32(payload.size()), bytes.data());
    return bytes + payload;
}
void reply(QTcpSocket *socket, const QJsonObject &request, const QJsonObject &data, bool fragment = false)
{
    const auto bytes = frame({{"id", request.value("id")}, {"type", "response"}, {"ok", true}, {"data", data}});
    if (!fragment) socket->write(bytes);
    else {
        socket->write(bytes.left(2));
        QTimer::singleShot(20, socket, [socket, bytes] { socket->write(bytes.mid(2)); });
    }
}
void serveSignal(QTcpServer &server, const std::function<void(QTcpSocket *, const QJsonObject &)> &handler)
{
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, handler] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, handler] {
                auto buffer = socket->property("input").toByteArray() + socket->readAll();
                while (buffer.size() >= 4) {
                    const auto length = qFromBigEndian<quint32>(buffer.constData());
                    if (buffer.size() < qsizetype(length) + 4) break;
                    const auto object = QJsonDocument::fromJson(buffer.mid(4, length)).object();
                    buffer.remove(0, length + 4);
                    handler(socket, object);
                }
                socket->setProperty("input", buffer);
            });
        }
    });
}
bool signalFramesAndHeartbeat()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    int heartbeats = 0;
    serveSignal(server, [&heartbeats](QTcpSocket *socket, const QJsonObject &request) {
        const auto type = request.value("type").toString();
        if (type == "auth") reply(socket, request, {{"userId", "root"}, {"heartbeatSeconds", 1}}, true);
        else if (type == "heartbeat") { ++heartbeats; reply(socket, request, {{"serverTime", 1}}); }
        else {
            // 同一次写入包含事件和响应，验证拆包及请求编号关联。
            socket->write(frame({{"type", "event"}, {"event", "live.changed"}, {"data", QJsonObject{{"roomId", "r1"}}}})
                + frame({{"type", "response"}, {"id", request.value("id")}, {"ok", true}, {"data", QJsonObject{{"value", type}}}}));
        }
    });
    SignalClient signal;
    int events = 0;
    QHash<QString, QString> responses;
    QObject::connect(&signal, &SignalClient::eventReceived, &signal, [&events](const QString &, const QJsonObject &) { ++events; });
    QObject::connect(&signal, &SignalClient::response, &signal, [&responses](const QString &id, const QString &type, bool ok, const QJsonObject &data, const QString &) {
        if (ok && data.value("value").toString() == type) responses.insert(id, type);
    });
    signal.connectToServer("127.0.0.1", server.serverPort(), "test-token");
    CHECK(waitUntil([&] { return signal.isReady(); }));
    const auto first = signal.request("room.list");
    const auto second = signal.request("room.leave");
    CHECK(!first.isEmpty() && first != second);
    CHECK(waitUntil([&] { return responses.size() == 2 && events == 2 && heartbeats > 0; }));
    signal.disconnectFromServer();
    CHECK(!signal.isReady() && signal.request("room.list").isEmpty());
    signal.connectToServer("127.0.0.1", server.serverPort(), "test-token");
    CHECK(waitUntil([&] { return signal.isReady(); }));
    return true;
}
bool signalInvalidFrameAndTimeout()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    serveSignal(server, [](QTcpSocket *socket, const QJsonObject &request) {
        if (request.value("type") == "auth") reply(socket, request, {{"userId", "root"}, {"heartbeatSeconds", 15}});
        else if (request.value("type") == "bad") socket->write(QByteArray(4, '\0'));
        // timeout 请求故意不响应。
    });
    SignalClient signal;
    QString message;
    QObject::connect(&signal, &SignalClient::stateChanged, &signal, [&message](bool, bool, const QString &text) { message = text; });
    signal.connectToServer("127.0.0.1", server.serverPort(), "test-token");
    CHECK(waitUntil([&] { return signal.isReady(); }));
    signal.request("bad");
    CHECK(waitUntil([&] { return !signal.isReady(); }));
    CHECK(message.contains(QStringLiteral("长度")));
    signal.connectToServer("127.0.0.1", server.serverPort(), "test-token");
    CHECK(waitUntil([&] { return signal.isReady(); }));
    signal.request("timeout");
    CHECK(waitUntil([&] { return !signal.isReady(); }));
    CHECK(message.contains(QStringLiteral("超时")));
    return true;
}
bool discoveryAndCancellation()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    const QUrl base(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    QStringList methods;
    bool invalid = false;
    bool hang = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const auto bytes = socket->property("input").toByteArray() + socket->readAll();
                socket->setProperty("input", bytes);
                const auto end = bytes.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("handled").toBool()) return;
                if (bytes.startsWith("POST") && !bytes.endsWith('}')) return;
                socket->setProperty("handled", true);
                methods.append(QString::fromLatin1(bytes.left(bytes.indexOf(' '))));
                if (hang) return;
                const auto data = bytes.startsWith("GET")
                    ? QJsonObject{{"loginUrl", invalid ? "file:///invalid" : base.resolved(QUrl("/auth/login")).toString()}}
                    : QJsonObject{{"accessToken", "test-token"}, {"expiresIn", 60}, {"user", QJsonObject{{"id", "root"}, {"displayName", "root"}}}};
                const auto body = QJsonDocument(data).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    MainWindow view;
    HttpClient http;
    LoginModel model;
    LoginController controller(&view, &model, &http);
    controller.login(base.resolved(QUrl("/login/server")), "root", "root");
    CHECK(waitUntil([&] { return model.state() == LoginModel::State::LoggedIn; }));
    CHECK(methods == QStringList({"GET", "POST"}));
    controller.logout();
    invalid = true;
    controller.login(base.resolved(QUrl("/login/server")), "root", "root");
    CHECK(waitUntil([&] { return model.state() == LoginModel::State::LoggedOut; }));
    CHECK(methods.size() == 3);
    invalid = false;
    hang = true;
    controller.login(base.resolved(QUrl("/login/server")), "root", "root");
    CHECK(waitUntil([&] { return methods.size() == 4; }));
    controller.cancel();
    QTest::qWait(30);
    CHECK(model.state() == LoginModel::State::LoggedOut && model.accessToken().isEmpty());
    return true;
}

bool sessionPagesAndClosedEvent()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    bool badPage = false;
    const QJsonObject first{{"roomId", "r1"}, {"title", "first"}, {"streaming", false}, {"viewers", 0}};
    const QJsonObject second{{"roomId", "r2"}, {"title", "second"}, {"streaming", false}, {"viewers", 0}};
    serveSignal(server, [&](QTcpSocket *socket, const QJsonObject &request) {
        const auto type = request.value("type").toString();
        if (type == "auth") reply(socket, request, {{"userId", "root"}, {"heartbeatSeconds", 15}});
        else if (type == "room.list") {
            if (request.value("offset").toInt() == 0)
                reply(socket, request, {{"rooms", QJsonArray{first}}, {"nextOffset", badPage ? 0 : 1}});
            else reply(socket, request, {{"rooms", QJsonArray{second}}, {"nextOffset", QJsonValue::Null}});
        } else if (type == "room.join") {
            // 模拟服务器在加入响应前已因主播连接关闭而广播 room.closed。
            socket->write(frame({{"type", "event"}, {"event", "room.closed"}, {"data", QJsonObject{{"roomId", "r1"}}}})
                + frame({{"type", "response"}, {"id", request.value("id")}, {"ok", true}, {"data", first}}));
        }
    });
    MainWindow view(ClientRole::Player);
    view.setSignalEndpoint("127.0.0.1", server.serverPort());
    HttpClient http;
    LoginModel login;
    LoginController auth(&view, &login, &http);
    SignalClient signal;
    SessionModel session;
    SessionController controller(&view, &login, &session, &signal);
    login.completeLogin("test-token", "root", "root", 60);
    CHECK(waitUntil([&] { return session.ready() && !session.busy() && session.rooms().size() == 2; }));
    CHECK(view.findChild<QListWidget *>("roomList")->count() == 2);
    view.joinRoomRequested("r1");
    CHECK(waitUntil([&] { return !session.busy(); }));
    CHECK(session.room().isEmpty() && session.message().contains(QStringLiteral("关闭")));
    badPage = true;
    view.refreshRoomsRequested();
    CHECK(waitUntil([&] { return !session.ready(); }));
    CHECK(session.rooms().isEmpty() && session.message().contains(QStringLiteral("分页")));
    return true;
}

bool liveTwoClients()
{
    MainWindow publisher(ClientRole::Publisher), player(ClientRole::Player);
    HttpClient publisherHttp, playerHttp;
    LoginModel publisherLogin, playerLogin;
    LoginController publisherAuth(&publisher, &publisherLogin, &publisherHttp), playerAuth(&player, &playerLogin, &playerHttp);
    SignalClient publisherSignal, playerSignal;
    SessionModel publisherSession, playerSession;
    SessionController publisherController(&publisher, &publisherLogin, &publisherSession, &publisherSignal);
    SessionController playerController(&player, &playerLogin, &playerSession, &playerSignal);
    publisher.show(); player.show();
    auto login = [](MainWindow &view, const QString &password) {
        view.findChild<QLineEdit *>("passwordEdit")->setText(password);
        QTest::mouseClick(view.findChild<QPushButton *>("loginButton"), Qt::LeftButton);
    };
    login(publisher, "wrong");
    CHECK(waitUntil([&] { return publisherLogin.state() == LoginModel::State::LoggedOut; }));
    CHECK(!publisherSignal.isReady());
    login(publisher, "root"); login(player, "root");
    CHECK(waitUntil([&] { return publisherSession.ready() && playerSession.ready() && !playerSession.busy(); }));
    QTest::mouseClick(publisher.findChild<QPushButton *>("createRoomButton"), Qt::LeftButton);
    CHECK(waitUntil([&] { return !publisherSession.room().isEmpty() && !publisherSession.busy(); }));
    const QString roomId = publisherSession.room().value("roomId").toString();
    QTest::mouseClick(player.findChild<QPushButton *>("refreshRoomsButton"), Qt::LeftButton);
    CHECK(waitUntil([&] { return !playerSession.busy() && !playerSession.rooms().isEmpty(); }));
    auto *list = player.findChild<QListWidget *>("roomList");
    for (int index = 0; index < list->count(); ++index)
        if (list->item(index)->data(Qt::UserRole).toString() == roomId) list->setCurrentRow(index);
    CHECK(list->currentItem());
    QTest::mouseClick(player.findChild<QPushButton *>("joinRoomButton"), Qt::LeftButton);
    CHECK(waitUntil([&] { return playerSession.room().value("roomId").toString() == roomId && !playerSession.busy(); }));
    CHECK(playerSession.room().value("viewers").toInt() == 1);
    CHECK(waitUntil([&] { return publisherSession.room().value("viewers").toInt() == 1; }));
    const auto screenshot = qEnvironmentVariable("CSN_SCREENSHOT_PATH");
    if (!screenshot.isEmpty()) {
        const auto directory = QFileInfo(screenshot).absolutePath();
        CHECK(publisher.grab().save(directory + QStringLiteral("/推流端页面.png")));
        CHECK(player.grab().save(directory + QStringLiteral("/播放端直播页面.png")));
    }
    // 点播切换必须退出当前直播房间，再切回来重新加入。
    player.findChild<QComboBox *>("modeCombo")->setCurrentIndex(1);
    CHECK(waitUntil([&] { return playerSession.room().isEmpty() && !playerSession.busy(); }));
    CHECK(waitUntil([&] { return publisherSession.room().value("viewers").toInt() == 0; }));
    if (!screenshot.isEmpty()) CHECK(player.grab().save(QFileInfo(screenshot).absolutePath() + QStringLiteral("/播放端点播页面.png")));
    player.findChild<QComboBox *>("modeCombo")->setCurrentIndex(0);
    player.joinRoomRequested(roomId);
    CHECK(waitUntil([&] { return !playerSession.room().isEmpty() && !playerSession.busy(); }));
    QTest::mouseClick(publisher.findChild<QPushButton *>("leaveRoomButton"), Qt::LeftButton);
    CHECK(waitUntil([&] { return publisherSession.room().isEmpty() && playerSession.room().isEmpty(); }));
    CHECK(playerSession.message().contains(QStringLiteral("关闭")));
    publisher.createRoomRequested(QString(100, QChar(0x4e2d)));
    CHECK(publisherSession.room().isEmpty() && !publisherSession.busy());
    publisher.createRoomRequested(QStringLiteral("断线清理测试"));
    CHECK(waitUntil([&] { return !publisherSession.room().isEmpty() && !publisherSession.busy(); }));
    const QString secondRoom = publisherSession.room().value("roomId").toString();
    player.joinRoomRequested(secondRoom);
    CHECK(waitUntil([&] { return !playerSession.room().isEmpty() && !playerSession.busy(); }));
    publisherSignal.disconnectFromServer();
    CHECK(waitUntil([&] { return playerSession.room().isEmpty(); }));
    CHECK(publisherSession.room().isEmpty());
    publisher.reconnectRequested();
    CHECK(waitUntil([&] { return publisherSession.ready(); }));
    publisher.createRoomRequested(QStringLiteral("退出清理测试"));
    CHECK(waitUntil([&] { return !publisherSession.room().isEmpty() && !publisherSession.busy(); }));
    player.joinRoomRequested(publisherSession.room().value("roomId").toString());
    CHECK(waitUntil([&] { return !playerSession.room().isEmpty() && !playerSession.busy(); }));
    publisher.close();
    CHECK(waitUntil([&] { return playerSession.room().isEmpty(); }));
    CHECK(publisherLogin.accessToken().isEmpty() && !publisherSignal.isReady());
    playerAuth.logout();
    CHECK(playerLogin.accessToken().isEmpty() && !playerSignal.isReady());
    // 本地过期计时器必须退出登录并断开信令。
    playerLogin.completeLogin("invalid-test-token", "root", "root", 1);
    CHECK(waitUntil([&] { return playerLogin.state() == LoginModel::State::LoggedOut; }, 2500));
    return true;
}
}
int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toUtf8();
        std::fprintf(stderr, "%s\n", bytes.constData());
        std::fflush(stderr);
    });
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (fontId >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).constFirst(), 10));
#endif
    if (app.arguments().contains("--integration")) {
        const bool ok = liveTwoClients();
        qInfo("%s liveTwoClients", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    int failures = 0;
    const auto run = [&failures](const char *name, bool (*test)()) {
        const bool ok = test();
        qInfo("%s %s", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    run("signalFramesAndHeartbeat", signalFramesAndHeartbeat);
    run("signalInvalidFrameAndTimeout", signalInvalidFrameAndTimeout);
    run("discoveryAndCancellation", discoveryAndCancellation);
    run("sessionPagesAndClosedEvent", sessionPagesAndClosedEvent);
    return failures ? 1 : 0;
}
