#include "PreviewWindow.h"
#include "VideoRenderer.h"
#include <QLabel>
#include <QStackedLayout>
#include <QScreen>

PreviewWindow::PreviewWindow(const QString &title, const QString &placeholder, QWidget *parent)
    : QWidget(parent, Qt::Window), m_placeholder(placeholder), m_message(new QLabel(placeholder, this)),
      m_renderer(new VideoRenderer(this)), m_layout(new QStackedLayout(this))
{
    // 1. 独立非模态窗口，关闭仅隐藏，主窗口负责其生命周期。
    setObjectName(QStringLiteral("previewWindow"));
    setWindowTitle(title);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(QSize(960, 540).boundedTo(screen()->availableGeometry().size() - QSize(40, 80)));
    setMinimumSize(320, 180);
    // 2. 原生画布独占像素；占位文字通过另一层显示，不覆盖交换链。
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setMargin(24);
    m_message->setStyleSheet("background:#101824;color:#b9c8dc");
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(m_message);
    m_layout->addWidget(m_renderer);
    // 3. GPU 失败只切换为错误文字，设备采集和音频继续工作。
    connect(m_renderer, &VideoRenderer::failed, this, [this](const QString &message) {
        m_renderFailed = true;
        m_message->setText(message);
        m_layout->setCurrentWidget(m_message);
        emit failed(message);
    });
    // 不设置 DeleteOnClose：主窗口拥有此对象，用户可重复隐藏、打开。
}

void PreviewWindow::setFrame(const csn::VideoFrame &frame)
{
    // 1. 无论是否隐藏，都让画布保留最新纹理。
    m_renderer->setFrame(frame);
    // 2. 空帧代表停止，清除资源/错误/自动打开标志。
    if (!frame.texture) {
        m_presented = false; m_renderFailed = false;
        m_message->setText(m_placeholder);
        m_layout->setCurrentWidget(m_message);
        hide();
        return;
    }
    // 3. 正常时展示 GPU 画布；用户关闭后等待主动打开。
    if (!m_renderFailed) m_layout->setCurrentWidget(m_renderer);
    if (!m_presented) { m_presented = true; show(); }
}

void PreviewWindow::present()
{
    m_presented = true;
    if (isMinimized()) showNormal();
    else show();
    raise();
    activateWindow();
}
