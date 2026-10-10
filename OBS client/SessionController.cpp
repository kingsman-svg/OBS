#include "SessionController.h"
#include "LoginModel.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "mainwindow.h"
#include "VideoCapture.h"
#include "GpuFaceDetector.h"
#include "WasapiCapture.h"
#include <QCoreApplication>
#include <windows.h>
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
// 只读当前QPC，音频输出时钟仍按样本数推进，不按GUI回调次数累计。
qint64 audioNow()
{
    LARGE_INTEGER value{}, frequency{};
    QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    return value.QuadPart / frequency.QuadPart * 10000000
        + value.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
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
    m_faces->stop();
    // 日常停止异步进行；析构等待退出，保证工作线程不访问已销毁对象。
    if (m_enumerator) m_enumerator->wait();
    m_video->wait(); m_microphone->wait(); m_system->wait();
    m_faces->wait();
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
    setupFaceDetection();
    connect(m_view, &MainWindow::captureRequested, this, &SessionController::startCapture);
    connect(m_view, &MainWindow::stopCaptureRequested, this, &SessionController::stopCapture);
    connect(m_view, &MainWindow::refreshDevicesRequested, this, &SessionController::refreshDevices);
    connect(m_view, &MainWindow::audioMixChanged, this, [this](CaptureSource::Kind kind, float gain, bool muted) {
        try { m_audioMixer.setControl(kind, gain, muted); }
        catch (const std::exception &error) { m_audioError = QString::fromUtf8(error.what()); }
        refreshAudioView();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &SessionController::stopCapture);
    bindCapture(m_video, 0); bindCapture(m_microphone, 1); bindCapture(m_system, 2);
    connect(m_video, &VideoCapture::opened, this, [this] {
        if (!m_stoppingCapture) m_captureMessages[0] = tr("视频采集中");
        refreshCaptureView();
    });
    connect(m_video, &VideoCapture::failed, this, [this](const QString &error) {
        m_media.disableVideo();
        m_captureErrors.insert(0); m_captureMessages[0] = error; refreshCaptureView();
    });
    for (int index : {1, 2}) {
        auto *worker = index == 1 ? m_microphone : m_system;
        connect(worker, &WasapiCapture::opened, this, [this, index] {
            if (!m_stoppingCapture) m_captureMessages[index] = tr("音频采集中");
            refreshCaptureView();
        });
        connect(worker, &WasapiCapture::failed, this, [this, index](const QString &error) {
            m_audioMixer.disable(index == 1 ? CaptureSource::Kind::Microphone : CaptureSource::Kind::Loopback);
            if (!m_audioMixer.active()) m_media.disableAudio();
            m_captureErrors.insert(index); m_captureMessages[index] = error; refreshCaptureView();
            refreshAudioView();
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
        // 1. finished之前端点缓冲已归还；仍需取走最后一批拥有字节数据的包。
        if (index > 0) consumeAudio();
        // 直到 finished 被 GUI 处理才允许重新启动，避免迟到的旧信号覆盖新状态。
        m_pendingCapture.remove(worker);
        // 2. 两个音频线程均退出才排空滤波尾部；不必等待视频线程。
        if (index > 0 && !m_pendingCapture.contains(m_microphone) && !m_pendingCapture.contains(m_system)) finishAudio();
        if (!m_captureErrors.contains(index)) m_captureMessages[index] = tr("已停止");
        if (index == 0) {
            m_faces->stop(); m_view->showCapturePreview({});
            if (!m_stoppingCapture) m_media.disableVideo();
        }
        if (m_pendingCapture.isEmpty()) { finishMedia(); m_stoppingCapture = false; }
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
    if (!m_video || !m_loggedIn || m_enumerator || m_stoppingCapture || !m_pendingCapture.isEmpty()) return;
    if (selection.isEmpty()) { m_captureMessages[0] = tr("请至少选择一个采集源"); refreshCaptureView(); return; }
    m_captureErrors.clear();
    m_captureMessages = {tr("未启用"), tr("未启用"), tr("未启用")};
    m_lastSequence = 0;
    m_lastFaceSequence = 0; m_faceFailed = false;
    bool videoSelected = false;
    for (const auto &source : selection)
        videoSelected = videoSelected || (source.kind != CaptureSource::Kind::Microphone && source.kind != CaptureSource::Kind::Loopback);
    // 检测只在选择视频源时启用；文件读取和 CUDA 初始化都在检测线程。
    m_faceMessage = tr("人脸检测未启用");
    if (m_view->faceDetectionEnabled() && videoSelected) {
        m_faceMessage = tr("正在等待视频帧并加载引擎…");
        if (m_faces->begin(m_view->faceEnginePath())) m_pendingCapture.insert(m_faces);
        else { m_faceFailed = true; m_faceMessage = tr("检测线程尚未退出，当前显示原画面"); }
    }
    m_view->showFaceStatus(m_faceMessage);
    // 1. 按用户本轮选择启用混音路，先清理上轮数据与处理错误。
    bool microphone = false, system = false;
    for (const auto &source : selection) {
        microphone = microphone || source.kind == CaptureSource::Kind::Microphone;
        system = system || source.kind == CaptureSource::Kind::Loopback;
    }
    m_audioMixer.begin(microphone, system); m_mixedPackets = 0; m_audioError.clear();
    m_media.begin(audioNow(), videoSelected, microphone || system);
    refreshMediaView();
    refreshAudioView();
    // 2. 启动各采集线程；采集层仍输出设备原始采样率和布局。
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
    m_faces->stop();
    if (!m_faceFailed) { m_faceMessage = tr("人脸检测已停止"); m_view->showFaceStatus(m_faceMessage); }
    // 普通停止等音频finished后排空；退出登录立即丢弃，禁止交付迟到媒体。
    if (!m_loggedIn) { m_audioMixer.clear(); m_media.clear(); refreshAudioView(); refreshMediaView(); }
    else if (!m_pendingCapture.contains(m_microphone) && !m_pendingCapture.contains(m_system)) finishAudio();
    const QList<QThread *> workers{m_video, m_microphone, m_system};
    for (int index = 0; index < workers.size(); ++index)
        if (m_pendingCapture.contains(workers[index]) && !m_captureErrors.contains(index)) m_captureMessages[index] = tr("正在停止");
    m_view->showCapturePreview({});
    if (m_pendingCapture.isEmpty()) finishMedia();
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
        if (m_pendingCapture.contains(m_faces) && !m_faceFailed) m_faces->submit(frame);
        else m_media.pushVideo(FaceFrame{frame});
    }
    // 检测结果连同同一原帧交付，禁止用旧框点叠加到最新采集帧。
    if (deliver && m_pendingCapture.contains(m_video) && m_pendingCapture.contains(m_faces) && !m_faceFailed) {
        const auto result = m_faces->latestResult();
        if (result.video.texture && result.video.sequence != m_lastFaceSequence) {
            m_lastFaceSequence = result.video.sequence;
            m_media.pushVideo(result);
            const qint64 ageMs = std::max<qint64>(0, (audioNow() - result.video.timestamp100ns) / 10000);
            m_faceMessage = tr("%1 张人脸 · GPU %2 ms · 处理 %3 ms · 帧龄 %4 ms\n替换待处理帧 %5 次 · 纹理注册 %6 次")
                .arg(result.faces.size()).arg(result.gpuMs, 0, 'f', 2).arg(result.processingMs).arg(ageMs)
                .arg(result.replacedFrames).arg(result.registrations);
            m_view->showFaceStatus(m_faceMessage);
        }
    }
    // 原始包可继续排入停止尾部，但普通交付只在采集运行期间进行。
    consumeAudio();
    if (deliver) {
        for (const auto &packet : m_audioMixer.takeFrames(audioNow())) {
            if (!m_loggedIn) break;
            ++m_mixedPackets; emit mixedAudioReady(packet);
            if (m_loggedIn) m_media.pushAudio(packet);
        }
        if (m_loggedIn && !m_stoppingCapture) deliverMedia(m_media.takeFrames(audioNow()), true);
    }
    refreshMediaView();
    refreshAudioView();
    m_view->showAudioLevels(deliver ? m_microphone->level() : 0, deliver ? m_system->level() : 0);
}

void SessionController::consumeAudio()
{
    // 1. 两路依次取走队列；即使退出也清掉迟到包，避免下一次登录重放。
    for (int index : {1, 2}) {
        auto *worker = index == 1 ? m_microphone : m_system;
        const auto kind = index == 1 ? CaptureSource::Kind::Microphone : CaptureSource::Kind::Loopback;
        for (const auto &packet : worker->takePackets()) {
            if (!m_loggedIn || m_captureErrors.contains(index)) continue;
            emit audioPacketReady(kind, packet); // 原始包保留为诊断入口，编码使用synchronizedAudioReady。
            // 2. 重采样失败只撤掉这一路，另一成功路仍输出，错误保留在混音状态。
            try { m_audioMixer.push(kind, packet); }
            catch (const std::exception &error) {
                m_audioMixer.disable(kind);
                if (!m_audioMixer.active()) m_media.disableAudio();
                if (!m_audioError.isEmpty()) m_audioError += QStringLiteral("；");
                m_audioError += tr("%1处理失败：%2").arg(index == 1 ? tr("麦克风") : tr("系统声音"), QString::fromUtf8(error.what()));
            }
        }
    }
}

void SessionController::setupFaceDetection()
{
    // 1. 线程只负责模型和 GPU；登录、采集和 View 仍由本控制器编排。
    m_faces = new GpuFaceDetector(this);
    connect(m_faces, &GpuFaceDetector::opened, this, [this] {
        if (!m_stoppingCapture && m_loggedIn && !m_faceFailed) {
            m_faceMessage = tr("引擎已加载，等待首个检测结果…"); m_view->showFaceStatus(m_faceMessage);
        }
    });
    // 2. 本轮失败锁存一次，后续交付恢复原帧，音频和采集不受影响。
    connect(m_faces, &GpuFaceDetector::failed, this, [this](const QString &message) {
        if (m_stoppingCapture || !m_loggedIn) return;
        m_faceFailed = true; m_faceMessage = message; m_view->showFaceStatus(message);
        if (m_pendingCapture.contains(m_video)) m_media.pushVideo(FaceFrame{m_video->latestFrame()});
    });
    // 3. finished 前 GPU 资源已注销；控制器收到 finished 才允许重新启动。
    connect(m_faces, &QThread::finished, this, [this] {
        m_pendingCapture.remove(m_faces);
        if (!m_faceFailed) { m_faceMessage = tr("人脸检测已停止"); m_view->showFaceStatus(m_faceMessage); }
        if (m_pendingCapture.isEmpty()) { finishMedia(); m_stoppingCapture = false; }
        refreshCaptureView();
    });
}

void SessionController::finishAudio()
{
    // 1. 退出只丢弃；2. 正常停止输出滤波尾部和最后补零包；3. 更新已停止状态。
    if (!m_loggedIn) { m_audioMixer.clear(); m_media.clear(); }
    else {
        try {
            for (const auto &packet : m_audioMixer.finish()) {
                if (!m_loggedIn) break;
                ++m_mixedPackets; emit mixedAudioReady(packet);
                if (m_loggedIn) m_media.pushAudio(packet);
            }
        } catch (const std::exception &error) { m_audioMixer.clear(); m_audioError = QString::fromUtf8(error.what()); }
    }
    refreshAudioView();
}

void SessionController::deliverMedia(const MediaBatch &batch, bool preview)
{
    // 1. 同步层已完成等待与PTS计算；GUI只交付，消费者不得在信号槽里阻塞编码/网络。
    for (const auto &packet : batch.audio) {
        if (!m_loggedIn || (preview && m_stoppingCapture)) return;
        emit synchronizedAudioReady(packet);
    }
    for (const auto &frame : batch.video) {
        if (!m_loggedIn || (preview && m_stoppingCapture)) return;
        emit synchronizedVideoReady(frame);
        if (preview && m_loggedIn && !m_stoppingCapture) m_view->showFacePreview(frame.source);
    }
}

void SessionController::finishMedia()
{
    // 2. 混音尾部已入队，采集/检测全部退出后才排空；退出时拒绝任何迟到媒体。
    if (m_loggedIn) deliverMedia(m_media.finish(), false);
    else m_media.clear();
    refreshMediaView();
}

void SessionController::refreshMediaView()
{
    const auto &stats = m_media.stats();
    m_view->showMediaSyncStatus(tr("同步%1 · 视频30fps / 音频48kHz · 总等待120ms\n视频 %2 帧（复用 %3，丢弃 %4）· 音频 %5 包（丢弃 %6）")
        .arg(m_media.active() ? tr("运行中") : tr("已停止"))
        .arg(stats.videoFrames).arg(stats.repeatedVideo).arg(stats.droppedVideo)
        .arg(stats.audioPackets).arg(stats.droppedAudio));
}

void SessionController::refreshAudioView()
{
    QString message;
    if (m_audioMixer.active())
        message = m_mixedPackets ? tr("48kHz · 立体声 · 10ms\n已输出 %1 包，削波 %2 次").arg(m_mixedPackets).arg(m_audioMixer.clippedSamples())
            : tr("48kHz · 立体声 · 10ms\n等待首包音频，缓冲80ms");
    else message = m_mixedPackets ? tr("混音已停止，共 %1 包，削波 %2 次").arg(m_mixedPackets).arg(m_audioMixer.clippedSamples()) : tr("未启用音频混音");
    if (!m_audioError.isEmpty()) message += QStringLiteral("\n") + m_audioError;
    m_view->showMixedAudio(m_audioMixer.level(), message);
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
