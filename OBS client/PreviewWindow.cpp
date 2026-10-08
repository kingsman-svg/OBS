#include "PreviewWindow.h"
#include <QPainter>
#include <QScreen>

PreviewWindow::PreviewWindow(const QString &title, const QString &placeholder, QWidget *parent)
    : QWidget(parent, Qt::Window), m_placeholder(placeholder)
{
    setObjectName(QStringLiteral("previewWindow"));
    setWindowTitle(title);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(QSize(960, 540).boundedTo(screen()->availableGeometry().size() - QSize(40, 80)));
    setMinimumSize(320, 180);
    // 不设置 DeleteOnClose：主窗口拥有此对象，用户可重复隐藏、打开。
}

void PreviewWindow::setFrame(const QImage &image)
{
    m_frame = image;
    if (image.isNull()) {
        m_presented = false;
        hide(); // 停止或退出后清除旧画面，下次采集的首帧重新打开窗口。
    } else if (!m_presented) {
        m_presented = true;
        show(); // 首帧自动显示一次；用户关闭后，后续帧不能反复弹窗。
    }
    update();
}

void PreviewWindow::present()
{
    m_presented = true;
    if (isMinimized()) showNormal();
    else show();
    raise();
    activateWindow();
}

void PreviewWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#101824"));
    if (m_frame.isNull()) {
        painter.setPen(QColor("#b9c8dc"));
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        return;
    }
    // 每次重绘都按窗口大小重新计算，保留原始 QImage，缩放后不会拉伸或裁掉画面。
    const QSize size = m_frame.size().scaled(this->size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, m_frame);
}
