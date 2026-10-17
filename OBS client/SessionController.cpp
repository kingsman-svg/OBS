#include "SessionController.h"
#include "LoginModel.h"
#include "SessionModel.h"
#include "SignalClient.h"
#include "mainwindow.h"
#include "VideoCapture.h"
#include "GpuFaceDetector.h"
#include "WasapiCapture.h"
#include "ffmpeg/MediaPublisher.h"
#include "ffmpeg/MediaPlayer.h"
#include <QCoreApplication>
#include <QDir>
#include <QUuid>
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
        m_leaveRequested = !m_model->room().isEmpty();
        if (m_livePending) stopCapture();
        if (m_playbackLive) stopPlayback();
        if (!m_model->room().isEmpty()) execute(QStringLiteral("room.leave"));
    });
    connect(signal, &SignalClient::stateChanged, this, [this](bool ready, bool connecting, const QString &message) {
        if (!ready) {
            m_pending.clear();
            m_listing = {};
            m_liveUrl.clear();
            m_leaveRequested = false;
            if (m_livePending) stopCapture();
            if (m_playbackLive) stopPlayback();
        }
        m_model->setConnection(ready, connecting, message);
        if (ready && m_view->role() == ClientRole::Player) refreshRooms();
    });
    connect(signal, &SignalClient::response, this, &SessionController::onResponse);
    connect(signal, &SignalClient::eventReceived, this, &SessionController::onEvent);
    refreshView();
    if (view->role() == ClientRole::Publisher) setupCapture();
    else setupPlayback();
    onLoginChanged();
}

SessionController::~SessionController()
{
    m_captureDelivery.stop();
    m_playbackDelivery.stop();
    if (m_player) { m_player->stop(); m_player->wait(); }
    if (!m_video) return;
    m_video->stop(); m_microphone->stop(); m_system->stop();
    m_faces->stop();
    // 日常停止异步进行；析构等待退出，保证工作线程不访问已销毁对象。
    if (m_enumerator) m_enumerator->wait();
    m_video->wait(); m_microphone->wait(); m_system->wait();
    m_faces->wait();
    // 应用退出仍封存已经送到编码队列的数据；GUI事件循环结束后不再补交同步层尾部。
    m_publisher->stop(); m_publisher->wait();
    m_livePublisher->stop(); m_livePublisher->wait();
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
        stopPlayback();
        m_expiry.stop();
        m_signal->disconnectFromServer(tr("已退出登录。"));
    }
    refreshCaptureView();
    refreshPlaybackView();
}

void SessionController::setupPlayback()
{
    // 1. 线程对象在GUI构造；文件/网络读取及音视频解码延迟到run执行。
    m_player = new MediaPlayer(this);
    connect(m_view, &MainWindow::playbackRequested, this, &SessionController::startPlayback);
    connect(m_view, &MainWindow::stopPlaybackRequested, this, &SessionController::stopPlayback);
    connect(m_view, &MainWindow::playLiveRequested, this, [this] { m_livePlaybackWanted = true; startLivePlayback(); });
    connect(m_view, &MainWindow::playbackVolumeChanged, m_player, &MediaPlayer::setVolume);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &SessionController::stopPlayback);
    connect(m_view, &MainWindow::playbackRenderFailed, this, [this](const QString &message) {
        if (!m_playbackPending || m_playbackStopping) return;
        m_playbackFailed = true; m_playbackMessage = message; stopPlayback();
    });
    // 2. 工作线程只发状态；GUI交付视频，不在这些槽里读文件/等待线程。
    connect(m_player, &MediaPlayer::opened, this, [this](const MediaInfo &info) {
        m_playbackDurationUs = info.durationUs;
        if (!m_playbackStopping) m_playbackMessage = tr("正在播放%1").arg(info.video ? tr(" · 画面显示在独立窗口") : tr(" · 仅音频"));
        refreshPlaybackView();
    });
    connect(m_player, &MediaPlayer::failed, this, [this](const QString &message) {
        m_livePlaybackWanted = false;
        m_playbackFailed = true; m_playbackStopping = true; m_playbackMessage = message;
        m_playbackDelivery.stop(); m_view->showPlaybackPreview({}); refreshPlaybackView();
    });
    connect(m_player, &MediaPlayer::completed, this, [this](bool eof) {
        m_playbackDelivery.stop();
        if (!m_playbackFailed) m_playbackMessage = eof ? tr("播放结束，可重新播放") : tr("已停止");
        refreshPlaybackView();
    });
    // 3. Decoder/输出端点均已归还，finished回到GUI后才解锁，防止上轮事件影响新一轮。
    connect(m_player, &QThread::finished, this, [this] {
        m_playbackPending = false; m_playbackStopping = false;
        m_playbackLive = false;
        m_playbackDelivery.stop(); refreshPlaybackView();
        if (m_livePlaybackWanted) startLivePlayback(); // 停止期间收到新开播事件，等资源归还后重开。
    });
    m_playbackDelivery.setInterval(10);
    connect(&m_playbackDelivery, &QTimer::timeout, this, &SessionController::deliverPlayback);
    m_playbackMessage = tr("选择本地文件或HTTP(S)媒体地址后播放");
    refreshPlaybackView();
}
void SessionController::startPlayback(const QString &source, bool audioOutput)
{
    // 1. 房间切换须先完成；本地/在线点播不要求信令已连接，但仍沿用登录工作台。
    if (!m_player || !m_loggedIn || !m_view->vodMode() || m_playbackPending || m_model->busy() || !m_model->room().isEmpty()) return;
    if (source.isEmpty()) { m_playbackMessage = tr("请选择媒体文件或输入在线地址"); refreshPlaybackView(); return; }
    m_view->showPlaybackPreview({}); m_playbackFailed = m_playbackStopping = false; m_playbackDurationUs = 0;
    m_playbackLive = false; m_livePlaybackWanted = false;
    m_playbackMessage = tr("正在打开媒体…"); m_playbackPending = true;
    // 2. begin只清状态并start，run负责打开/探测，GUI此时仍可停止。
    if (m_player->begin(source, audioOutput)) m_playbackDelivery.start();
    else { m_playbackPending = false; m_playbackMessage = tr("播放线程尚未退出，请稍后重试"); }
    refreshPlaybackView();
}
void SessionController::stopPlayback()
{
    if (!m_player) return;
    m_livePlaybackWanted = false;
    m_playbackDelivery.stop(); m_player->stop(); m_view->showPlaybackPreview({});
    if (m_playbackPending) {
        m_playbackStopping = true;
        if (!m_playbackFailed) m_playbackMessage = tr("正在停止播放…");
    }
    refreshPlaybackView();
}
void SessionController::deliverPlayback()
{
    if (!m_loggedIn || (!m_view->vodMode() && !m_playbackLive) || !m_playbackPending || m_playbackStopping) return;
    const auto frame = m_player->takeVideo();
    if (frame.source.texture) { emit playbackFrameReady(frame.source); m_view->showPlaybackPreview(frame.source); }
    refreshPlaybackView(false);
}
void SessionController::refreshPlaybackView(bool force)
{
    if (!m_player) return;
    if (!force && m_playbackStatsRefresh.isValid() && !m_playbackStatsRefresh.hasExpired(200)) return;
    m_playbackStatsRefresh.restart();
    const auto stats = m_player->stats();
    const auto latency = m_playbackLive ? tr("\n打开 %1 ms · 首张交付 %2\n视频队列 %3 帧 / %4 ms · PCM队列 %5 ms · 端点待播 %6 / 容量 %7 ms\n实际PCM提交 %8 秒 · 应用补零 %9 秒 · 音频追赶 %10 次")
        .arg(stats.openUs / 1000.0, 0, 'f', 1)
        .arg(stats.firstDeliveryUs ? QString::number(stats.firstDeliveryUs / 1000.0, 'f', 1) + tr(" ms") : tr("等待"))
        .arg(stats.queuedVideo).arg(stats.videoQueueSpanUs / 1000.0, 0, 'f', 1).arg(stats.audioQueuedUs / 1000.0, 0, 'f', 1)
        .arg(stats.audioPaddingUs / 1000.0, 0, 'f', 1).arg(stats.audioBufferUs / 1000.0, 0, 'f', 1)
        .arg(stats.renderedAudioSamples / 48000.0, 0, 'f', 2).arg(stats.insertedSilenceSamples / 48000.0, 0, 'f', 2).arg(stats.audioCatchups) : QString();
    const auto buffer = stats.adaptiveBuffer ? tr("\n自适应缓冲：%1 · 目标 %2 ms · 交付波动 %3 ms\n目标升/降 %4/%5 次 · 重新缓冲 %6 次 / %7 ms")
        .arg(!m_playbackPending ? tr("已停止") : stats.buffering ? tr("等待恢复") : tr("播放中")).arg(stats.bufferTargetUs / 1000.0, 0, 'f', 0)
        .arg(stats.deliveryJitterUs / 1000.0, 0, 'f', 1).arg(stats.bufferIncreases).arg(stats.bufferDecreases)
        .arg(stats.rebuffers).arg(stats.rebufferUs / 1000.0, 0, 'f', 0) : QString();
    m_view->applyPlaybackState(m_playbackPending, stats.positionUs, m_playbackDurationUs,
        tr("%1\n解码 %2 帧（GPU %3 / 软件 %4 / 硬件回读 %5）\n显示 %6 帧 · 跳过 %7 帧 · 音频 %8 秒 · 欠载 %9 次%10")
            .arg(m_playbackMessage).arg(stats.decodedVideo).arg(stats.gpuVideo).arg(stats.softwareVideo).arg(stats.readbackVideo)
            .arg(stats.displayedVideo).arg(stats.droppedVideo).arg(stats.audioSamples / 48000.0, 0, 'f', 2).arg(stats.audioUnderruns)
            .arg(stats.decoderNote.isEmpty() ? QString() : QStringLiteral("\n") + stats.decoderNote) + latency + buffer);
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
    setupRecording();
    setupLiveOutput();
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
        if (m_pendingCapture.isEmpty()) { finishMedia(); m_stoppingCapture = m_recordingPending || m_livePending; }
        refreshCaptureView();
    });
}

void SessionController::refreshDevices()
{
    if (!m_video || m_enumerator || !m_pendingCapture.isEmpty() || m_recordingPending || m_livePending) return;
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
    if (!m_video || !m_loggedIn || m_enumerator || m_stoppingCapture || !m_pendingCapture.isEmpty() || m_recordingPending) return;
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
    if ((m_view->faceDetectionEnabled() || m_view->beautySettings().enabled) && videoSelected) {
        m_faceMessage = tr("正在等待视频帧并加载引擎…");
        if (m_faces->begin(m_view->faceEnginePath(), m_view->beautySettings(), m_view->faceDetectionEnabled())) m_pendingCapture.insert(m_faces);
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
    if (m_livePending) {
        // 2a. 与同步原点一起建立RTMP入口；本轮PTS从零开始，不从中途录制时间线起推。
        RecordingSettings settings; settings.path = m_liveUrl; settings.live = true;
        settings.videoBitrate = m_view->liveVideoBitrate();
        settings.adaptiveBitrate = videoSelected && m_view->liveAutomaticBitrate();
        settings.minimumVideoBitrate = m_view->liveMinimumVideoBitrate();
        settings.video = videoSelected; settings.audio = microphone || system;
        if (!m_livePublisher->begin(settings)) { m_livePending = false; m_liveMessage = tr("推流线程尚未退出"); }
    }
    refreshMediaView();
    refreshAudioView();
    // 2. 同步原点已建立；先开启有界编码入口，再启动采集，首帧设备在run中读取。
    m_recordingMessage = tr("录制未启用");
    if (m_view->recordingEnabled()) {
        const auto directory = m_view->recordingDirectory();
        if (directory.isEmpty()) m_recordingMessage = tr("录制未启动：请选择保存目录");
        else {
            RecordingSettings settings;
            settings.video = videoSelected; settings.audio = microphone || system;
            settings.path = QDir(directory).filePath(QStringLiteral("录制_%1_%2.mp4")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")), QUuid::createUuid().toString(QUuid::Id128).left(8)));
            m_recordingPending = true;
            if (m_publisher->begin(settings)) m_recordingMessage = tr("正在初始化编码器，等待媒体…");
            else { m_recordingPending = false; m_recordingMessage = tr("录制线程尚未退出，请稍后重试"); }
        }
    }
    if (m_recordingPending) refreshRecordingView();
    else m_view->showRecordingStatus(m_recordingMessage); // 本轮不录制时不展示上轮计数。
    // 3. 启动各采集线程；采集层仍输出设备原始采样率和布局。
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
    m_stoppingCapture = !m_pendingCapture.isEmpty() || m_recordingPending || m_livePending;
    if (m_livePending) { m_liveMediaSent = false; m_livePublisher->stop(); updateLiveNotice(); }
    m_video->stop(); m_microphone->stop(); m_system->stop();
    m_faces->stop();
    if (!m_faceFailed) { m_faceMessage = tr("人脸检测已停止"); m_view->showFaceStatus(m_faceMessage); }
    // 普通停止等音频finished后排空；退出登录立即丢弃，禁止交付迟到媒体。
    if (!m_loggedIn) { m_audioMixer.clear(); m_media.clear(); m_publisher->stop(); refreshAudioView(); refreshMediaView(); }
    else if (!m_pendingCapture.contains(m_microphone) && !m_pendingCapture.contains(m_system)) finishAudio();
    const QList<QThread *> workers{m_video, m_microphone, m_system};
    for (int index = 0; index < workers.size(); ++index)
        if (m_pendingCapture.contains(workers[index]) && !m_captureErrors.contains(index)) m_captureMessages[index] = tr("正在停止");
    m_view->showCapturePreview({});
    if (m_pendingCapture.isEmpty()) finishMedia();
    if (m_recordingPending) { m_recordingMessage = tr("正在停止采集并封存 MP4…"); refreshRecordingView(); }
    refreshCaptureView();
}

void SessionController::refreshCaptureView()
{
    if (!m_video) return;
    const bool active = !m_pendingCapture.isEmpty() || m_recordingPending || m_livePending;
    m_view->applyCaptureState(m_loggedIn && !active && !m_enumerator, active, m_enumerator != nullptr,
        m_deviceMessage, m_captureMessages);
    refreshLiveView();
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
            m_faceMessage = tr("%1 张人脸 · 推理GPU %2 ms · 处理 %3 ms · 帧龄 %4 ms\n替换待处理帧 %5 次 · 纹理注册 %6 次 · 美颜%7（%8 ms）")
                .arg(result.faces.size()).arg(result.gpuMs, 0, 'f', 2).arg(result.processingMs).arg(ageMs)
                .arg(result.replacedFrames).arg(result.registrations).arg(result.beautified ? tr("已应用") : tr("透传")).arg(result.beautyMs);
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
    if (m_recordingPending) refreshRecordingView();
    if (m_livePending) refreshLiveView(false);
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
    connect(m_view, &MainWindow::beautyStrengthChanged, m_faces, &GpuFaceDetector::setBeautyStrength);
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
        if (m_pendingCapture.isEmpty()) { finishMedia(); m_stoppingCapture = m_recordingPending || m_livePending; }
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
    // 3. 最后一批同步数据已快速入队，随后关入口，run排空编码器并写trailer。
    if (m_recordingPending) m_publisher->stop();
    if (m_livePending) { m_liveMediaSent = false; m_livePublisher->stop(); updateLiveNotice(); }
}

void SessionController::setupRecording()
{
    // 1. GUI拥有线程对象；这两个槽只入队，不在GUI执行FFmpeg/GPU/文件操作。
    m_publisher = new MediaPublisher(this);
    connect(this, &SessionController::synchronizedVideoReady, m_publisher, &MediaPublisher::pushVideo);
    connect(this, &SessionController::synchronizedAudioReady, m_publisher, &MediaPublisher::pushAudio);
    // 2. worker结果排队回Controller；录制失败仍允许采集和独立画面继续运行。
    connect(m_publisher, &MediaPublisher::opened, this, [this] {
        if (!m_stoppingCapture) m_recordingMessage = tr("正在录制 MP4");
        refreshRecordingView();
    });
    connect(m_publisher, &MediaPublisher::failed, this, [this](const QString &message) {
        m_recordingMessage = message; refreshRecordingView();
    });
    connect(m_publisher, &MediaPublisher::completed, this, [this](const QString &path, const EncodingStats &) {
        m_recordingMessage = path.isEmpty() ? tr("本轮没有可保存的媒体") : tr("已保存：%1").arg(QDir::toNativeSeparators(path));
        refreshRecordingView();
    });
    // 3. 资源已在run结束前释放，GUI处理finished后才重新解锁开始按钮。
    connect(m_publisher, &QThread::finished, this, [this] {
        m_recordingPending = false;
        if (m_pendingCapture.isEmpty() && !m_livePending) m_stoppingCapture = false;
        refreshRecordingView(); refreshCaptureView();
    });
}

void SessionController::refreshRecordingView()
{
    const auto stats = m_publisher->stats();
    m_view->showRecordingStatus(tr("%1\n视频 %2 帧 · 音频 %3 秒 · 编码 %4 KB · 队列丢弃 %5 帧 / %6 包")
        .arg(m_recordingMessage).arg(stats.videoFrames).arg(stats.audioSamples / 48000.0, 0, 'f', 2)
        .arg(stats.bytes / 1024).arg(stats.droppedVideo).arg(stats.droppedAudio));
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
        if (type == QStringLiteral("live.start")) {
            m_liveMessage = tr("开播通知失败：%1").arg(error); stopCapture();
        }
        if (type == QStringLiteral("room.leave")) m_leaveRequested = false;
        if (m_leaveRequested && !m_model->room().isEmpty()) execute(QStringLiteral("room.leave"));
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
        m_liveUrl.clear(); m_leaveRequested = false;
        if (m_playbackLive) stopPlayback();
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
        if (type == QStringLiteral("room.create")) m_liveUrl = data.value(QStringLiteral("pushUrl")).toString();
        m_model->setRoom(data, data.value(QStringLiteral("streaming")).toBool() ? tr("房间已开播。") : tr("房间已就绪，等待开播。"));
        m_model->setBusy(false, m_model->message());
        if (type == QStringLiteral("room.join")) {
            m_livePlaybackWanted = data.value(QStringLiteral("streaming")).toBool(); startLivePlayback();
        }
    }
    if (m_leaveRequested && !m_model->room().isEmpty()) execute(QStringLiteral("room.leave"));
    else updateLiveNotice();
}

void SessionController::onEvent(const QString &event, const QJsonObject &data)
{
    if (event == QStringLiteral("room.closed")) m_closedRoom = data.value(QStringLiteral("roomId")).toString();
    if (data.value(QStringLiteral("roomId")) != m_model->room().value(QStringLiteral("roomId"))) return;
    if (event == QStringLiteral("room.closed")) {
        if (m_playbackLive) stopPlayback();
        if (m_livePending) stopCapture();
        m_liveUrl.clear(); m_leaveRequested = false;
        auto rooms = m_model->rooms();
        for (qsizetype index = rooms.size(); index > 0; --index)
            if (rooms.at(index - 1).toObject().value(QStringLiteral("roomId")) == data.value(QStringLiteral("roomId"))) rooms.removeAt(index - 1);
        m_model->setRooms(rooms);
        m_model->setRoom({}, tr("主播已关闭房间。可刷新列表重新选择。"));
    } else if ((event == QStringLiteral("live.changed") || event == QStringLiteral("room.updated")) && validRoom(data)) {
        m_model->setRoom(data, data.value(QStringLiteral("streaming")).toBool() ? tr("主播已开播。") : tr("房间已就绪，等待开播。"));
        if (event == QStringLiteral("live.changed") && m_player) {
            if (data.value(QStringLiteral("streaming")).toBool()) { m_livePlaybackWanted = true; startLivePlayback(); }
            else if (m_playbackLive) stopPlayback();
        }
    }
}

void SessionController::refreshView()
{
    m_view->applySessionState(m_model->ready(), m_model->busy(), m_model->connecting(),
                             m_model->message(), m_model->room(), m_model->rooms());
    refreshLiveView();
}

void SessionController::setupLiveOutput()
{
    // 1. Controller拥有第二个输出线程；录制/直播独立封装，复用同一批同步GPU帧和PCM。
    m_livePublisher = new MediaPublisher(this);
    // 参数入口只投递到工作线程邮箱；停止中/纯音频/非法参数明确拒绝，不覆盖当前目标。
    connect(m_view, &MainWindow::liveBitrateControlRequested, this, [this](bool automatic, int minimum, int maximum) {
        if (!m_loggedIn || !m_livePending || m_stoppingCapture) return;
        if (!m_livePublisher->requestBitrateControl({automatic, minimum, maximum})) m_liveMessage = tr("码率设置未接受：视频未运行或上下限无效");
        refreshLiveView();
    });
    connect(this, &SessionController::synchronizedVideoReady, m_livePublisher, &MediaPublisher::pushVideo);
    connect(this, &SessionController::synchronizedAudioReady, m_livePublisher, &MediaPublisher::pushAudio);
    connect(m_view, &MainWindow::startLiveRequested, this, &SessionController::startLive);
    connect(m_view, &MainWindow::stopLiveRequested, this, &SessionController::stopCapture);
    // 2. opened只说明头/编码器就绪；媒体写出后才发布房间开播通知。
    connect(m_livePublisher, &MediaPublisher::opened, this, [this] {
        if (m_livePending && !m_stoppingCapture) m_liveMessage = tr("RTMP已连接，等待首个编码包…");
        refreshLiveView();
    });
    connect(m_livePublisher, &MediaPublisher::transmitting, this, [this] {
        if (!m_loggedIn || !m_livePending || m_stoppingCapture || m_model->room().isEmpty()) return;
        m_liveMediaSent = true; m_liveMessage = tr("正在推流到SRS"); updateLiveNotice(); refreshLiveView();
    });
    connect(m_livePublisher, &MediaPublisher::failed, this, [this](const QString &message) {
        m_liveMediaSent = false; m_liveMessage = message; updateLiveNotice(); refreshLiveView();
    });
    connect(m_livePublisher, &MediaPublisher::completed, this, [this](const QString &, const EncodingStats &) {
        m_liveMediaSent = false; m_liveMessage = tr("直播媒体已停止"); updateLiveNotice(); refreshLiveView();
    });
    // 3. finished到GUI后解除重启锁；失败后采集预览可继续，重开直播先停止旧采集。
    connect(m_livePublisher, &QThread::finished, this, [this] {
        m_livePending = false; m_liveMediaSent = false;
        if (m_pendingCapture.isEmpty()) m_stoppingCapture = m_recordingPending;
        updateLiveNotice(); refreshCaptureView();
    });
    m_liveMessage = tr("创建房间、选择采集源后开始直播"); refreshLiveView();
}
void SessionController::startLive()
{
    // 1. 直播随一轮采集启动，使输出PTS从共同原点开始；不在采集中途重置MediaTimeline。
    if (!m_livePublisher || !m_loggedIn || !m_signal->isReady() || m_model->room().isEmpty()
        || m_model->busy() || m_liveUrl.isEmpty() || m_livePending || m_recordingPending
        || !m_pendingCapture.isEmpty() || m_enumerator || m_stoppingCapture) return;
    const auto sources = m_view->selectedCaptureSources();
    if (sources.isEmpty()) { m_liveMessage = tr("请至少选择一个采集源"); refreshLiveView(); return; }
    m_livePending = true; m_liveMediaSent = false; m_liveMessage = tr("正在启动采集和RTMP输出…");
    startCapture(sources); refreshLiveView();
}
void SessionController::updateLiveNotice()
{
    // 2. 信令单请求串行；停止发生在live.start在途期间时，响应后补发live.stop。
    if (!m_livePublisher || !m_loggedIn || !m_signal->isReady() || !m_pending.isEmpty()
        || m_model->room().isEmpty() || m_leaveRequested) return;
    const bool publishing = m_livePending && m_liveMediaSent && !m_stoppingCapture;
    if (publishing != m_model->room().value(QStringLiteral("streaming")).toBool())
        execute(publishing ? QStringLiteral("live.start") : QStringLiteral("live.stop"));
}
void SessionController::refreshLiveView(bool force)
{
    if (!m_livePublisher) return;
    if (!force && m_liveStatsRefresh.isValid() && m_liveStatsRefresh.elapsed() < 200) return;
    m_liveStatsRefresh.start(); // 诊断文本按5Hz更新；启停、错误和手动应用事件仍立即更新。
    const auto stats = m_livePublisher->stats();
    const bool canStart = m_loggedIn && m_model->ready() && !m_model->busy() && !m_model->room().isEmpty()
        && !m_liveUrl.isEmpty() && !m_livePending && !m_recordingPending && m_pendingCapture.isEmpty() && !m_enumerator && !m_stoppingCapture;
    const QString target = stats.encoderVideoBitrate ? QString::number(stats.encoderVideoBitrate / 1000) + tr(" kbps") : tr("等待视频帧");
    const QString pending = stats.targetVideoBitrate != stats.encoderVideoBitrate
        ? (m_livePending && !m_stoppingCapture ? tr("（等待下一帧提交）") : tr("（未提交）")) : QString();
    QString state;
    switch (stats.bitrateState) {
    case BitrateState::Manual: state = tr("手动固定目标"); break;
    case BitrateState::Observing: state = tr("观察反馈"); break;
    case BitrateState::Decreasing: state = tr("发送压力，降码率/冷却"); break;
    case BitrateState::AtMinimum: state = tr("已达最低码率，仍有发送压力"); break;
    case BitrateState::Recovering: state = tr("稳定后小步回升"); break;
    case BitrateState::ProcessingLimited: state = tr("处理耗时偏高，暂不判为网络拥塞"); break;
    case BitrateState::Idle: state = tr("媒体不足，暂停上探"); break;
    }
    const auto abr = tr("%1 · 计划目标 %2 kbps · 自动降/升 %3/%4 次 · 写出占时 %5% · %6%7")
        .arg(stats.automaticBitrate ? tr("自动码率") : tr("手动码率")).arg(stats.targetVideoBitrate / 1000)
        .arg(stats.bitrateReductions).arg(stats.bitrateIncreases).arg(stats.bitrateWriteLoad * 100, 0, 'f', 1).arg(state)
        .arg(stats.bitrateControlPending ? tr("（新设置待接收）") : QString());
    const auto latency = tr("\nVBV预算 %1 ms · GOP %2 帧 · 交织阈值 %3 ms\n建连 %4 ms · 就绪 %5 ms · 首次媒体写出 %6")
        .arg(stats.encoderVbvMs).arg(stats.encoderGopFrames).arg(stats.muxInterleaveMs)
        .arg(stats.connectUs / 1000.0, 0, 'f', 1).arg(stats.readyUs / 1000.0, 0, 'f', 1)
        .arg(stats.firstMediaUs ? QString::number(stats.firstMediaUs / 1000.0, 'f', 1) + tr(" ms") : tr("等待"));
    m_view->applyLiveOutputState(canStart, m_livePending,
        tr("%1\n视频 %2 帧 · 音频 %3 秒 · 编码 %4 KB\n用户目标/上限 %5 kbps · 编码器目标 %6%7\n视频压缩码率 %8 kbps（窗口 %9 ms）· 本机发送 %10 kbps（1秒）\n队列 视频 %11 / 音频 %12 · 跨度 %13 / %14 ms · 队头等待 %15 / %16 ms\n视频处理 %17 ms（不含写出）· 写出 最近 %18 / 最长 %19 / 正在等待 %20 ms\n过期丢弃 视频 %21 / 音频 %22 · 队列追赶 %23 次 · 音频跳过 %24 次 / %25 秒\n%26")
        .arg(m_liveMessage).arg(stats.videoFrames).arg(stats.audioSamples / 48000.0, 0, 'f', 2).arg(stats.bytes / 1024)
        .arg(stats.requestedVideoBitrate / 1000).arg(target).arg(pending).arg(stats.videoPayloadBitrate / 1000).arg(stats.videoRateWindowMs)
        .arg(stats.outputBitrate / 1000).arg(stats.queuedVideo).arg(stats.queuedAudio).arg(stats.videoQueueSpanMs).arg(stats.audioQueueSpanMs)
        .arg(stats.oldestVideoWaitMs).arg(stats.oldestAudioWaitMs).arg(stats.lastVideoEncodeUs / 1000.0, 0, 'f', 2)
        .arg(stats.lastWriteUs / 1000.0, 0, 'f', 2).arg(stats.maxWriteUs / 1000.0, 0, 'f', 2).arg(stats.currentWriteUs / 1000.0, 0, 'f', 2)
        .arg(stats.expiredVideo).arg(stats.expiredAudio).arg(stats.liveCatchups).arg(stats.audioResyncs).arg(stats.skippedAudioSamples / 48000.0, 0, 'f', 3).arg(abr) + latency,
        !m_stoppingCapture && m_pendingCapture.contains(m_video));
}
void SessionController::startLivePlayback()
{
    // 3. 加入等待中的房间只保留业务会话；live.changed到达后才启动读包/声音线程。
    if (!m_player || !m_loggedIn || m_view->vodMode() || !m_livePlaybackWanted || m_playbackPending
        || m_model->room().isEmpty() || !m_model->room().value(QStringLiteral("streaming")).toBool()) return;
    const auto source = m_model->room().value(QStringLiteral("pullUrl")).toString();
    m_view->showPlaybackPreview({}); m_playbackFailed = m_playbackStopping = false; m_playbackDurationUs = 0;
    m_playbackLive = true; m_playbackPending = true; m_playbackMessage = tr("正在连接直播媒体…");
    if (m_player->begin(source, m_view->playbackAudioEnabled(), true)) m_playbackDelivery.start();
    else { m_playbackPending = false; m_playbackLive = false; m_playbackMessage = tr("播放线程尚未退出，请稍后重试"); }
    refreshPlaybackView();
}
}
