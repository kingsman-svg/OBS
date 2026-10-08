#pragma once
#include <QString>
#include <QMetaType>
#include <QList>

namespace csn {
// 一个选择项就能描述摄像头、窗口、显示器或音频端点，不建立设备继承树。
struct CaptureSource {
    enum class Kind {
        Camera,     // WinRT 摄像头彩色帧。
        Window,     // WGC 指定窗口。
        Monitor,    // WGC 指定显示器。
        Microphone, // WASAPI 输入端点。
        Loopback    // WASAPI 输出端点回环，不是直接访问麦克风。
    };
    Kind kind = Kind::Camera;    // 决定选择哪个系统采集接口。
    QString id;                 // WinRT/WASAPI 设备 ID；桌面目标使用句柄的十六进制文本。
    QString name;               // 面向界面的友好名称，不用来定位设备。
    quintptr handle = 0;         // HWND/HMONITOR，仅在启动时使用并重新检查有效性。
};
} // namespace csn
Q_DECLARE_METATYPE(csn::CaptureSource)
Q_DECLARE_METATYPE(QList<csn::CaptureSource>)
