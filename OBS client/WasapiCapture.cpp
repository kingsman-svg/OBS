#include "WasapiCapture.h"
#include <QMutexLocker>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using Microsoft::WRL::ComPtr;
namespace {
// QPC 换算到 100ns，与视频使用同一时间域。
qint64 now100ns()
{
    LARGE_INTEGER value{}, frequency{};
    QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    return value.QuadPart / frequency.QuadPart * 10000000
        + value.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
}
// 调用线程必须已初始化 COM；返回活动端点枚举器。
ComPtr<IMMDeviceEnumerator> enumerator()
{
    ComPtr<IMMDeviceEnumerator> result;
    winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&result)));
    return result;
}
}
namespace csn {
WasapiCapture::WasapiCapture(QObject *parent) : QThread(parent) {}
WasapiCapture::~WasapiCapture() { stop(); wait(); }

QList<CaptureSource> WasapiCapture::devices(bool loopback)
{
    // 1. 回环枚举输出 eRender，麦克风枚举输入 eCapture，只返回活动端点。
    QList<CaptureSource> result;
    ComPtr<IMMDeviceCollection> collection;
    winrt::check_hresult(enumerator()->EnumAudioEndpoints(loopback ? eRender : eCapture,
        DEVICE_STATE_ACTIVE, &collection));
    UINT count = 0;
    winrt::check_hresult(collection->GetCount(&count));
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        winrt::check_hresult(collection->Item(index, &device));
        // 2. 设备 ID 用于定位，FriendlyName 用于显示；及时归还 COM 分配的字符串。
        LPWSTR id = nullptr;
        winrt::check_hresult(device->GetId(&id));
        const QString deviceId = QString::fromWCharArray(id);
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> properties;
        winrt::check_hresult(device->OpenPropertyStore(STGM_READ, &properties));
        PROPVARIANT name{};
        winrt::check_hresult(properties->GetValue(PKEY_Device_FriendlyName, &name));
        QString label = name.vt == VT_LPWSTR ? QString::fromWCharArray(name.pwszVal) : deviceId;
        PropVariantClear(&name);
        result.append({loopback ? CaptureSource::Kind::Loopback : CaptureSource::Kind::Microphone,
                       deviceId, label, 0});
    }
    return result;
}

AudioPacket WasapiCapture::decode(const uchar *data, quint32 frames, const WAVEFORMATEX &format,
                                  quint32 flags, quint64 timestamp)
{
    // 1. 识别 PCM/Float 及 EXTENSIBLE 子格式，检查声道、位宽和块对齐。
    bool floating = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcm = format.wFormatTag == WAVE_FORMAT_PCM;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto &extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format);
        floating = IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        pcm = IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    const int bits = format.wBitsPerSample;
    if (format.nChannels == 0 || format.nChannels > 32 || format.nSamplesPerSec == 0
        || format.nBlockAlign != format.nChannels * (bits / 8)
        || !((floating && (bits == 32 || bits == 64)) || (pcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32))))
        throw std::runtime_error("音频端点格式不受支持，需要 PCM 或 IEEE Float");
    if (!data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0)
        throw std::runtime_error("音频端点返回空缓冲");
    const quint64 samples = quint64(frames) * format.nChannels;
    if (samples > 4 * 1024 * 1024) throw std::runtime_error("音频数据包过大");
    // 2. 保存设备采样率/声道及不连续标记，时间无效时用当前 QPC 估计。
    AudioPacket packet;
    packet.sampleRate = int(format.nSamplesPerSec);
    packet.channels = format.nChannels;
    packet.timestampEstimated = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
    packet.timestamp100ns = packet.timestampEstimated ? now100ns() : qint64(timestamp);
    packet.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
    packet.pcm.resize(qsizetype(samples * sizeof(float)));
    // 3. 逐采样转 Float32；静音填 0，NaN/溢出归一处理，此处不重采样。
    for (quint64 index = 0; index < samples; ++index) {
        float output = 0;
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            const auto *input = data + index * (bits / 8);
            if (floating && bits == 32) std::memcpy(&output, input, 4);
            else if (floating) { double value; std::memcpy(&value, input, 8); output = float(value); }
            else if (bits == 8) output = (int(*input) - 128) / 128.0f;
            else if (bits == 16) { qint16 value; std::memcpy(&value, input, 2); output = value / 32768.0f; }
            else if (bits == 24) {
                qint32 value = qint32(input[0]) | (qint32(input[1]) << 8) | (qint32(input[2]) << 16);
                if (value & 0x800000) value -= 0x1000000;
                output = value / 8388608.0f;
            } else { qint32 value; std::memcpy(&value, input, 4); output = float(value / 2147483648.0); }
        }
        if (!std::isfinite(output)) output = 0;
        output = std::clamp(output, -1.0f, 1.0f);
        std::memcpy(packet.pcm.data() + index * sizeof(float), &output, sizeof(float));
    }
    return packet;
}

bool WasapiCapture::begin(const CaptureSource &source)
{
    // 1. 校验源及线程状态；2. 清空队列和电平；3. start 进入 run。
    if (isRunning() || (source.kind != CaptureSource::Kind::Microphone && source.kind != CaptureSource::Kind::Loopback)) return false;
    m_source = source;
    { QMutexLocker lock(&m_mutex); m_packets.clear(); m_level = 0; m_lastPacketTime = 0; }
    start();
    return true;
}
void WasapiCapture::stop() { requestInterruption(); }
QList<AudioPacket> WasapiCapture::takePackets()
{
    QMutexLocker lock(&m_mutex);
    QList<AudioPacket> result;
    result.swap(m_packets);
    return result;
}
float WasapiCapture::level() const
{
    QMutexLocker lock(&m_mutex);
    return now100ns() - m_lastPacketTime > 5000000 ? 0 : m_level;
}

void WasapiCapture::run()
{
    // 1. 工作线程建立 MTA，所有端点 COM 对象都在此线程使用和销毁。
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) { emit failed(QStringLiteral("音频采集 COM 初始化失败")); return; }
    // COM 对象全部在此作用域销毁，然后再 CoUninitialize。
    {
        ComPtr<IAudioClient> client;
        try {
            // 2. 按设备 ID 打开端点，空 ID 使用默认端点；激活 IAudioClient。
            ComPtr<IMMDevice> device;
            auto manager = enumerator();
            if (m_source.id.isEmpty())
                winrt::check_hresult(manager->GetDefaultAudioEndpoint(
                    m_source.kind == CaptureSource::Kind::Loopback ? eRender : eCapture, eConsole, &device));
            else winrt::check_hresult(manager->GetDevice(reinterpret_cast<LPCWSTR>(m_source.id.utf16()), &device));
            winrt::check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void **>(client.GetAddressOf())));
            // 3. GetMixFormat 读取设备原始格式，并预先验证解码支持。
            WAVEFORMATEX *rawFormat = nullptr;
            winrt::check_hresult(client->GetMixFormat(&rawFormat));
            std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawFormat, &CoTaskMemFree);
            // 先验证格式，设备即使静音也不会把不认识的格式误当 Float32。
            decode(nullptr, 0, *format, AUDCLNT_BUFFERFLAGS_SILENT, 0);
            // 4. 初始化共享模式；回环仅增加 LOOPBACK 标志，获取采集缓冲接口。
            winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                m_source.kind == CaptureSource::Kind::Loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                1000000, 0, format.get(), nullptr));
            ComPtr<IAudioCaptureClient> capture;
            winrt::check_hresult(client->GetService(IID_PPV_ARGS(&capture)));
            // 5. Start 成功发 opened；WASAPI 此处按包轮询，无 FrameArrived 回调。
            if (!isInterruptionRequested()) {
                winrt::check_hresult(client->Start());
                emit opened();
            }
            // 6. 查询可读包；无数据短暂休眠，避免空转占用 CPU。
            while (!isInterruptionRequested()) {
                UINT32 available = 0;
                winrt::check_hresult(capture->GetNextPacketSize(&available));
                if (!available) { msleep(5); continue; }
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 timestamp = 0;
                winrt::check_hresult(capture->GetBuffer(&data, &frames, &flags, nullptr, &timestamp));
                // 7. GetBuffer → Float32 复制 → ReleaseBuffer，异常也必须归还。
                AudioPacket packet;
                // GetBuffer 与 ReleaseBuffer 成对，即使格式转换抛出异常也归还端点缓冲。
                try { packet = decode(data, frames, *format, flags, timestamp); }
                catch (...) { capture->ReleaseBuffer(frames); throw; }
                winrt::check_hresult(capture->ReleaseBuffer(frames));
                // 8. 计算 RMS 电平；队列最多 50 包，丢旧包后标记不连续。
                double sum = 0;
                const auto samples = packet.pcm.size() / qsizetype(sizeof(float));
                for (qsizetype index = 0; index < samples; ++index) {
                    float value;
                    std::memcpy(&value, packet.pcm.constData() + index * sizeof(float), sizeof(float));
                    sum += double(value) * value;
                }
                QMutexLocker lock(&m_mutex);
                m_level = samples ? float(std::sqrt(sum / samples)) : 0;
                m_lastPacketTime = now100ns();
                if (m_packets.size() >= 50) {
                    m_packets.removeFirst();
                    if (!m_packets.isEmpty()) m_packets.first().discontinuity = true;
                }
                m_packets.append(std::move(packet));
            }
        } catch (const winrt::hresult_error &error) {
            if (!isInterruptionRequested()) emit failed(QStringLiteral("音频采集失败（0x%1）：%2")
                .arg(quint32(error.code().value), 8, 16, QLatin1Char('0'))
                .arg(QString::fromStdWString(error.message().c_str())));
        } catch (const std::exception &error) {
            if (!isInterruptionRequested()) emit failed(QString::fromUtf8(error.what()));
        }
        // 9. 正常/异常均 Stop，再释放接口，最后才 CoUninitialize。
        if (client) client->Stop();
    }
    { QMutexLocker lock(&m_mutex); m_level = 0; }
    CoUninitialize();
}
} // namespace csn
