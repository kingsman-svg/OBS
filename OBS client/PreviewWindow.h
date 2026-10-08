#pragma once
#include "VideoCapture.h"
#include <QWidget>

class QLabel;
class QStackedLayout;
class VideoRenderer;

// 只负责画面展示的非模态窗口；关闭窗口不改变采集状态。
class PreviewWindow final : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWindow(const QString &title, const QString &placeholder, QWidget *parent); // 创建文字层和原生画布。
    void setFrame(const csn::VideoFrame &frame); // 换最新帧，每轮首帧仅自动打开一次。
    void present();                            // 用户主动打开或恢复窗口，不改变采集状态。
signals:
    void failed(const QString &message);        // 转发预览错误，便于主窗口显示。
private:
    QString m_placeholder;                     // 尚无画面时的提示文字。
    QLabel *m_message;                         // 占位及错误文字，由 Qt 绘制。
    VideoRenderer *m_renderer;                 // 本窗口拥有的原生 GPU 画布。
    QStackedLayout *m_layout;                  // 文字和 GPU 画布互斥显示。
    bool m_presented = false;                  // 用户关闭后，新帧不能反复弹窗。
    bool m_renderFailed = false;               // 错误保留到本轮采集结束。
};
