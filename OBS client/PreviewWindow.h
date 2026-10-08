#pragma once
#include <QImage>
#include <QWidget>

class QPaintEvent;

// 只负责画面展示的非模态窗口；关闭窗口不改变采集状态。
class PreviewWindow final : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWindow(const QString &title, const QString &placeholder, QWidget *parent);
    void setFrame(const QImage &image);
    void present();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QString m_placeholder;
    QImage m_frame;
    bool m_presented = false;
};
