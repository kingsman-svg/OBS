#pragma once
#include <QString>
#include <QMetaType>
#include <QList>

namespace csn {
// 一个选择项就能描述摄像头、窗口、显示器或音频端点，不建立设备继承树。
struct CaptureSource {
    enum class Kind { Camera, Window, Monitor, Microphone, Loopback };
    Kind kind = Kind::Camera;
    QString id;                 // WinRT/WASAPI 设备 ID；桌面目标使用句柄的十六进制文本。
    QString name;
    quintptr handle = 0;         // HWND/HMONITOR，仅在启动时使用并重新检查有效性。
};
} // namespace csn
Q_DECLARE_METATYPE(csn::CaptureSource)
Q_DECLARE_METATYPE(QList<csn::CaptureSource>)
