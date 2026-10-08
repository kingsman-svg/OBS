#include "SessionController.h"
#include "LoginModel.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "mainwindow.h"
#include "VideoCapture.h"
#include "WasapiCapture.h"
#include <QCoreApplication>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace csn {
namespace {
bool validRoom(const QJsonObject &room)
{
    return !room.value(QStringLiteral("roomId")).toString().isEmpty()
        && !room.value(QStringLiteral("title")).toString().isEmpty()
        && room.value(QStringLiteral("streaming")).isBool()
        && room.value(QStringLiteral("viewers")).isDouble();
}
}

SessionController::SessionController(MainWindow *view, LoginModel *login, SessionModel *model,
                                     SignalClient *signal, QObject *parent)
    : QObject(parent), m_view(view), m_login(login), m_model(model), m_signal(signal)
{
    m_expiry.setSingleShot(true);
    connect(&m_expiry, &QTimer::timeout, this, &SessionController::scheduleExpiry);
    connect(login, &LoginModel::changed, this, &SessionController::onLoginChanged);
    connect(model, &SessionModel::changed, this, &SessionController::refreshView);
    connect(view, &MainWindow::reconnectRequested, this, &SessionController::reconnect);
    connect(view, &MainWindow::refreshRoomsRequested, this, &SessionController::refreshRooms);
    connect(view, &MainWindow::createRoomRequested, this, [this](const QString &title) {
        if (m_view->role() != ClientRole::Publisher || !m_model->room().isEmpty()) return;
        if (title.trimmed().isEmpty() || title.toUtf8().size() > 128) {
            m_model->setBusy(false, tr("房间标题须为 1～128 个 UTF-8 字节。"));
            return;
        }
        execute(QStringLiteral("room.create"), {{QStringLiteral("title"), title.trimmed()}});
    });
    connect(view, &MainWindow::joinRoomRequested, this, [this](const QString &roomId) {
        if (m_view->role() != ClientRole::Player || !m_model->room().isEmpty() || roomId.isEmpty()) return;
        execute(QStringLiteral("room.join"), {{QStringLiteral("roomId"), roomId}});
    });
    connect(view, &MainWindow::leaveRoomRequested, this, [this] {
        if (!m_model->room().isEmpty()) execute(QStringLiteral("room.leave"));
    });
    connect(signal, &SignalClient::stateChanged, this, [this](bool ready, bool connecting, const QString &message) {
        if (!ready) {
            m_pending.clear();
            m_listing = {};
        }
        m_model->setConnection(ready, connecting, message);
        if (ready && m_view->role() == ClientRole::Player) refreshRooms();
    });
    connect(signal, &SignalClient::response, this, &SessionController::onResponse);
    connect(signal, &SignalClient::eventReceived, this, &SessionController::onEvent);
    refreshView();
    if (view->role() == ClientRole::Publisher) setupCapture();
    onLoginChanged();
}

SessionController::~SessionController()
{
    m_captureDelivery.stop();
    if (!m_video) return;
    m_video->stop(); m_microphone->stop(); m_system->stop();
    // 日常停止异步进行；析构等待退出，保证工作线程不访问已销毁对象。
    if (m_enumerator) m_enumerator->wait();
    m_video->wait(); m_microphone->wait(); m_system->wait();
}

void SessionController::onLoginChanged()
{
    const bool loggedIn = m_login->state() == LoginModel::State::LoggedIn;
    if (loggedIn == m_loggedIn) return;
    m_loggedIn = loggedIn;
    if (loggedIn) {
        scheduleExpiry();
        reconnect();
    } else {
        stopCapture();
        m_expiry.stop();
        m_signal->disconnectFromServer(tr("已退出登录。"));
    }
    refreshCaptureView();
}

void SessionController::scheduleExpiry()
{
    const qint64 remaining = QDateTime::currentDateTimeUtc().msecsTo(m_login->expiresAt());
    if (remaining <= 0) {
        m_login->failLogin(tr("登录已过期，请重新登录。"));
        return;
    }
    m_expiry.start(int(std::min<qint64>(remaining, std::numeric_limits<int>::max())));
}

void SessionController::setupCapture()
{
    m_video = new VideoCapture(this);
    m_microphone = new WasapiCapture(this);
    m_system = new WasapiCapture(this);
    connect(m_view, &MainWindow::captureRequested, this, &SessionController::startCapture);
    connect(m_view, &MainWindow::stopCaptureRequested, this, &SessionController::stopCapture);
    connect(m_view, &MainWindow::refreshDevicesRequested, this, &SessionController::refreshDevices);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &SessionController::stopCapture);
    bindCapture(m_video, 0); bindCapture(m_microphone, 1); bindCapture(m_system, 2);
    connect(m_video, &VideoCapture::opened, this, [this] {
        if (!m_stoppingCapture) m_captureMessages[0] = tr("视频采集中");
        refreshCaptureView();
    });
    connect(m_video, &VideoCapture::failed, this, [this](const QString &error) {
        m_captureErrors.insert(0); m_captureMessages[0] = error; refreshCaptureView();
    });
    for (int index : {1, 2}) {
        auto *worker = index == 1 ? m_microphone : m_system;
        connect(worker, &WasapiCapture::opened, this, [this, index] {
            if (!m_stoppingCapture) m_captureMessages[index] = tr("音频采集中");
            refreshCaptureView();
        });
        connect(worker, &WasapiCapture::failed, this, [this, index](const QString &error) {
            m_captureErrors.insert(index); m_captureMessages[index] = error; refreshCaptureView();
        });
    }
    m_captureDelivery.setInterval(33);
    connect(&m_captureDelivery, &QTimer::timeout, this, &SessionController::deliverCapture);
    m_captureDelivery.start();
    refreshDevices();
}

void SessionController::bindCapture(QThread *worker, int index)
{
    connect(worker, &QThread::finished, this, [this, worker, index] {
        // 直到 finished 被 GUI 处理才允许重新启动，避免迟到的旧信号覆盖新状态。
        m_pendingCapture.remove(worker);
        if (!m_captureErrors.contains(index)) m_captureMessages[index] = tr("已停止");
        if (m_pendingCapture.isEmpty()) m_stoppingCapture = false;
        if (index == 0) m_view->showCapturePreview({});
        refreshCaptureView();
    });
}

void SessionController::refreshDevices()
{
    if (!m_video || m_enumerator || !m_pendingCapture.isEmpty()) return;
    m_enumerator = QThread::create([this] {
        QList<CaptureSource> sources;
        QStringList errors;
        bool initialized = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
            QString warning;
            try { sources += VideoCapture::sources(&warning); }
            catch (const winrt::hresult_error &error) { errors << QString::fromStdWString(error.message().c_str()); }
            if (!warning.isEmpty()) errors << warning;
            for (bool loopback : {false, true}) {
                try { sources += WasapiCapture::devices(loopback); }
                catch (const winrt::hresult_error &error) { errors << QString::fromStdWString(error.message().c_str()); }
            }
        } catch (const winrt::hresult_error &error) { errors << QString::fromStdWString(error.message().c_str()); }
        if (initialized) winrt::uninit_apartment();
        const auto message = errors.isEmpty() ? tr("已发现 %1 个采集目标；更换设备后可刷新。").arg(sources.size())
            : tr("部分设备枚举失败：%1").arg(errors.join(QStringLiteral("；")));
        QMetaObject::invokeMethod(this, [this, sources, message] {
            m_view->setCaptureSources(sources);
            m_deviceMessage = message;
        }, Qt::QueuedConnection);
    });
    m_enumerator->setParent(this);
    connect(m_enumerator, &QThread::finished, this, [this] {
        m_enumerator->deleteLater(); m_enumerator = nullptr; refreshCaptureView();
    });
    refreshCaptureView();
    m_enumerator->start();
}

void SessionController::startCapture(const QList<CaptureSource> &selection)
{
    if (!m_video || !m_loggedIn || m_enumerator || !m_pendingCapture.isEmpty()) return;
    if (selection.isEmpty()) { m_captureMessages[0] = tr("请至少选择一个采集源"); refreshCaptureView(); return; }
    m_captureErrors.clear();
    m_captureMessages = {tr("未启用"), tr("未启用"), tr("未启用")};
    m_lastSequence = 0;
    for (const auto &source : selection) {
        const int index = source.kind == CaptureSource::Kind::Microphone ? 1 : source.kind == CaptureSource::Kind::Loopback ? 2 : 0;
        QThread *worker = index == 0 ? static_cast<QThread *>(m_video) : index == 1 ? m_microphone : m_system;
        if (m_pendingCapture.contains(worker)) continue;
        m_captureMessages[index] = tr("正在打开 %1").arg(source.name);
        m_pendingCapture.insert(worker);
        const bool started = index == 0 ? m_video->begin(source) : index == 1 ? m_microphone->begin(source) : m_system->begin(source);
        if (!started) { m_pendingCapture.remove(worker); m_captureMessages[index] = tr("采集线程正在退出，请稍后重试"); }
    }
    refreshCaptureView();
}

void SessionController::stopCapture()
{
    if (!m_video) return;
    m_stoppingCapture = !m_pendingCapture.isEmpty();
    m_video->stop(); m_microphone->stop(); m_system->stop();
    const QList<QThread *> workers{m_video, m_microphone, m_system};
    for (int index = 0; index < workers.size(); ++index)
        if (m_pendingCapture.contains(workers[index]) && !m_captureErrors.contains(index)) m_captureMessages[index] = tr("正在停止");
    m_view->showCapturePreview({});
    refreshCaptureView();
}

void SessionController::refreshCaptureView()
{
    if (!m_video) return;
    const bool active = !m_pendingCapture.isEmpty();
    m_view->applyCaptureState(m_loggedIn && !active && !m_enumerator, active, m_enumerator != nullptr,
        m_deviceMessage, m_captureMessages);
}

void SessionController::deliverCapture()
{
    const bool deliver = m_loggedIn && !m_stoppingCapture;
    const auto frame = m_video->latestFrame();
    if (deliver && m_pendingCapture.contains(m_video) && !m_captureErrors.contains(0)
        && frame.texture && frame.sequence != m_lastSequence) {
        m_lastSequence = frame.sequence;
        emit videoFrameReady(frame);
        m_view->showCapturePreview(frame);
    }
    for (int index : {1, 2}) {
        auto *worker = index == 1 ? m_microphone : m_system;
        const auto packets = worker->takePackets();
        if (deliver && m_pendingCapture.contains(worker) && !m_captureErrors.contains(index))
            for (const auto &packet : packets) emit audioPacketReady(
                index == 1 ? CaptureSource::Kind::Microphone : CaptureSource::Kind::Loopback, packet);
    }
    m_view->showAudioLevels(deliver ? m_microphone->level() : 0, deliver ? m_system->level() : 0);
}

void SessionController::reconnect()
{
    if (!m_loggedIn || m_model->connecting() || m_signal->isReady()) return;
    if (m_login->expiresAt() <= QDateTime::currentDateTimeUtc()) {
        m_login->failLogin(tr("登录已过期，请重新登录。"));
        return;
    }
    m_signal->connectToServer(m_view->signalHost(), m_view->signalPort(), m_login->accessToken());
}

void SessionController::execute(const QString &type, const QJsonObject &fields)
{
    if (!m_signal->isReady() || !m_pending.isEmpty()) return;
    if (type == QStringLiteral("room.create") || type == QStringLiteral("room.join")) m_closedRoom.clear();
    m_model->setBusy(true, tr("正在处理房间请求…"));
    m_pending = m_signal->request(type, fields);
    if (m_pending.isEmpty() && m_signal->isReady()) m_model->setBusy(false, tr("未能发送房间请求。"));
}

void SessionController::refreshRooms()
{
    if (m_view->role() != ClientRole::Player || !m_pending.isEmpty() || !m_signal->isReady()) return;
    m_listing = {};
    m_offset = 0;
    execute(QStringLiteral("room.list"), {{QStringLiteral("offset"), 0}});
}

void SessionController::onResponse(const QString &id, const QString &type, bool ok,
                                   const QJsonObject &data, const QString &error)
{
    if (id != m_pending) return;
    m_pending.clear();
    if (!ok) {
        m_model->setBusy(false, error);
        return;
    }
    if (type == QStringLiteral("room.list")) {
        if (!data.value(QStringLiteral("rooms")).isArray()) {
            m_signal->disconnectFromServer(tr("房间列表格式无效。"));
            return;
        }
        const auto page = data.value(QStringLiteral("rooms")).toArray();
        for (const auto &room : page) {
            if (!validRoom(room.toObject()) || m_listing.size() >= 128) {
                m_signal->disconnectFromServer(tr("房间列表内容无效或超限。"));
                return;
            }
            m_listing.append(room);
        }
        const auto next = data.value(QStringLiteral("nextOffset"));
        if (!next.isNull()) {
            const double offset = next.toDouble(-1);
            if (!next.isDouble() || !std::isfinite(offset) || std::floor(offset) != offset
                || offset <= m_offset || offset > 128 || page.isEmpty()) {
                m_signal->disconnectFromServer(tr("房间列表分页无效。"));
                return;
            }
            m_offset = int(offset);
            execute(type, {{QStringLiteral("offset"), m_offset}});
            return;
        }
        m_model->setRooms(m_listing);
        m_model->setBusy(false, tr("已刷新 %1 个房间。未开播的房间可以先加入等待。").arg(m_listing.size()));
    } else if (type == QStringLiteral("room.leave")) {
        m_model->setRoom({}, tr("已退出房间。"));
        m_model->setBusy(false, tr("已退出房间。"));
        if (m_view->role() == ClientRole::Player) refreshRooms();
    } else {
        if (!validRoom(data)) {
            m_signal->disconnectFromServer(tr("房间响应格式无效。"));
            return;
        }
        // 广播发送失败时，服务器可能先通知关闭、再返回加入响应。
        // 不能用后来的响应恢复一个已经关闭的房间。
        if (data.value(QStringLiteral("roomId")).toString() == m_closedRoom) {
            m_model->setBusy(false, tr("房间已关闭，请刷新后重新选择。"));
            return;
        }
        m_model->setRoom(data, tr("房间已就绪；媒体链路待接入。"));
        m_model->setBusy(false, tr("房间已就绪；媒体链路待接入。"));
    }
}

void SessionController::onEvent(const QString &event, const QJsonObject &data)
{
    if (event == QStringLiteral("room.closed")) m_closedRoom = data.value(QStringLiteral("roomId")).toString();
    if (data.value(QStringLiteral("roomId")) != m_model->room().value(QStringLiteral("roomId"))) return;
    if (event == QStringLiteral("room.closed")) {
        auto rooms = m_model->rooms();
        for (qsizetype index = rooms.size(); index > 0; --index)
            if (rooms.at(index - 1).toObject().value(QStringLiteral("roomId")) == data.value(QStringLiteral("roomId"))) rooms.removeAt(index - 1);
        m_model->setRooms(rooms);
        m_model->setRoom({}, tr("主播已关闭房间。可刷新列表重新选择。"));
    } else if ((event == QStringLiteral("live.changed") || event == QStringLiteral("room.updated")) && validRoom(data)) {
        m_model->setRoom(data, data.value(QStringLiteral("streaming")).toBool() ? tr("直播中；媒体链路待接入。") : tr("房间已就绪，尚未开播；媒体链路待接入。"));
    }
}

void SessionController::refreshView()
{
    m_view->applySessionState(m_model->ready(), m_model->busy(), m_model->connecting(),
                             m_model->message(), m_model->room(), m_model->rooms());
}
}
