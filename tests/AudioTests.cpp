#include "AudioMixer.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>

using namespace csn;
#define CHECK(condition) do { if (!(condition)) { qCritical("Check failed at line %d: %s", __LINE__, #condition); return false; } } while (false)
namespace {
constexpr qint64 Base = 100000000;
constexpr double Pi = 3.14159265358979323846;
AudioPacket packet(int rate, int channels, int frames, qint64 timestamp, float left, float right = 0)
{
    AudioPacket result;
    result.sampleRate = rate; result.channels = channels; result.timestamp100ns = timestamp;
    result.channelMask = channels == 1 ? 4 : channels == 2 ? 3 : 0;
    result.pcm.resize(qsizetype(frames) * channels * sizeof(float));
    for (int frame = 0; frame < frames; ++frame)
        for (int channel = 0; channel < channels; ++channel) {
            const float value = channel == 0 ? left : right;
            std::memcpy(result.pcm.data() + (qsizetype(frame) * channels + channel) * sizeof(float), &value, sizeof(value));
        }
    return result;
}
AudioPacket tone(int rate, int channels, int first, int frames, double frequency, float amplitude)
{
    auto result = packet(rate, channels, frames, Base + qint64(first) * 10000000 / rate, 0);
    for (int frame = 0; frame < frames; ++frame) {
        const float value = amplitude * float(std::sin(2 * Pi * frequency * (first + frame) / rate));
        for (int channel = 0; channel < channels; ++channel)
            std::memcpy(result.pcm.data() + (qsizetype(frame) * channels + channel) * sizeof(float), &value, sizeof(value));
    }
    return result;
}
float sample(const AudioPacket &packet, int frame, int channel = 0)
{
    float value;
    std::memcpy(&value, packet.pcm.constData() + (qsizetype(frame) * packet.channels + channel) * sizeof(float), sizeof(value));
    return value;
}
bool near(float a, float b, float tolerance = 0.0001f) { return std::abs(a - b) < tolerance; }
bool rejected(const std::function<void()> &action)
{
    try { action(); } catch (const std::exception &) { return true; }
    return false;
}

bool resampling()
{
    // 实际 swr 处理 44.1kHz 正弦，任意分包必须与大包处理保持同一结果。
    QByteArray small, large;
    for (int chunk : {137, 22050}) {
        AudioResampler resampler;
        auto &bytes = chunk == 137 ? small : large;
        int outputFrames = 0;
        for (int first = 0; first < 44100; first += chunk) {
            const auto output = resampler.convert(tone(44100, 1, first, std::min(chunk, 44100 - first), 1000, 0.4f));
            if (output.pcm.isEmpty()) continue;
            CHECK(output.sampleRate == 48000 && output.channels == 2 && output.channelMask == 3);
            CHECK(std::abs(output.timestamp100ns - (Base + qint64(outputFrames) * 10000000 / 48000)) <= 1);
            outputFrames += int(output.pcm.size() / 8); bytes += output.pcm;
        }
        const auto tail = resampler.drain();
        bytes += tail.pcm; outputFrames += int(tail.pcm.size() / 8);
        CHECK(outputFrames == 48000 && resampler.drain().pcm.isEmpty());
    }
    CHECK(small.size() == 48000 * 8 && small.size() == large.size());
    AudioPacket output; output.pcm = small; output.channels = 2;
    double squaredError = 0;
    for (int frame = 100; frame < 47900; ++frame) {
        CHECK(near(sample(output, frame, 0), sample(output, frame, 1)));
        const double expected = 0.4 * std::sin(2 * Pi * 1000 * frame / 48000);
        squaredError += std::pow(sample(output, frame) - expected, 2);
        float other; std::memcpy(&other, large.constData() + frame * 8, 4);
        CHECK(near(sample(output, frame), other, 0.00001f));
    }
    CHECK(std::sqrt(squaredError / 47800) < 0.002);
    // 96kHz 中高于目标 Nyquist 的音调应被低通滤掉，不能直接抽样产生混叠。
    AudioResampler downsample;
    auto high = downsample.convert(tone(96000, 2, 0, 24000, 35000, 0.5f));
    high.pcm += downsample.drain().pcm;
    CHECK(high.pcm.size() == 12000 * 8);
    double energy = 0;
    for (int frame = 100; frame < 11900; ++frame) energy += std::pow(sample(high, frame), 2);
    CHECK(std::sqrt(energy / 11800) < 0.01);
    qInfo("PASS resampling / arbitrary packet boundaries / timestamps / anti-alias filter");
    return true;
}

bool formatsAndRestart()
{
    AudioResampler resampler;
    auto input = packet(48000, 1, 96, Base, 0.25f);
    auto output = resampler.convert(input);
    CHECK(output.pcm.size() == 96 * 8 && near(sample(output, 10, 0), 0.25f) && near(sample(output, 10, 1), 0.25f));
    CHECK(resampler.convert(input).pcm.isEmpty()); // 2ms短包重复也不能再输出。
    input = packet(32000, 2, 320, Base + 1000000, 0.1f, 0.2f);
    output = resampler.convert(input);
    CHECK(output.discontinuity && output.timestamp100ns == input.timestamp100ns);
    CHECK(output.channels == 2 && output.sampleRate == 48000);
    resampler.reset();
    input = packet(48000, 6, 480, Base, 0.1f, 0.2f); input.channelMask = 0x3f;
    output = resampler.convert(input);
    CHECK(output.pcm.size() == 480 * 8 && std::isfinite(sample(output, 100)));
    resampler.reset();
    input.channelMask = 0;
    CHECK(rejected([&] { resampler.convert(input); }));
    input = packet(48000, 1, 480, Base, 0.1f); input.channelMask = 3;
    CHECK(rejected([&] { resampler.convert(input); }));
    input = packet(48000, 1, 480, Base, 0.1f); input.pcm.chop(1);
    CHECK(rejected([&] { resampler.convert(input); }));
    input = packet(48000, 1, 480, Base, 0.1f); input.timestampEstimated = true;
    output = resampler.convert(input); CHECK(output.timestampEstimated);
    input.timestamp100ns += 500000; input.discontinuity = true;
    output = resampler.convert(input); CHECK(output.discontinuity && output.timestamp100ns == input.timestamp100ns);
    qInfo("PASS layouts / mono duplicate / format restart / duplicate / invalid input / discontinuity");
    return true;
}

bool alignedMixing()
{
    AudioMixer mixer; mixer.begin(true, true);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 2, 1440, Base, 0.6f, 0.2f));
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 960, Base + 100000, 0.6f, 0.3f));
    CHECK(mixer.takeFrames(Base + 800000).isEmpty());
    const auto output = mixer.takeFrames(Base + 1100000);
    CHECK(output.size() == 3 && output[0].pcm.size() == 480 * 8);
    CHECK(output[0].discontinuity && near(sample(output[0], 10), 0.6f));
    CHECK(!output[1].discontinuity && near(sample(output[1], 10), 1.0f) && near(sample(output[1], 10, 1), 0.5f));
    CHECK(!output[2].discontinuity && output[2].timestamp100ns == Base + 200000);
    CHECK(mixer.clippedSamples() == 960 && near(mixer.level(), float(std::sqrt(0.625))));
    CHECK(mixer.finish().isEmpty());
    // 时间跳变不能抹掉已经排队但尚未混合的有效前段。
    mixer.begin(true, false);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 1, 480, Base, 0.1f));
    auto afterGap = packet(48000, 1, 480, Base + 200000, 0.3f); afterGap.discontinuity = true;
    mixer.push(CaptureSource::Kind::Microphone, afterGap);
    const auto gap = mixer.takeFrames(Base + 1100000);
    CHECK(gap.size() == 3 && near(sample(gap[0], 100), 0.1f));
    CHECK(gap[1].discontinuity && near(sample(gap[1], 100), 0));
    CHECK(gap[2].discontinuity && near(sample(gap[2], 100), 0.3f));
    qInfo("PASS QPC offset / fixed packets / missing source silence / stereo sum / clipping");
    return true;
}

bool gainMuteAndTail()
{
    AudioMixer mixer; mixer.setControl(CaptureSource::Kind::Microphone, 2, false);
    mixer.setControl(CaptureSource::Kind::Loopback, 1, true); mixer.begin(true, true);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 1, 960, Base, 0.25f));
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 960, Base, 0.8f, 0.8f));
    auto output = mixer.takeFrames(Base + 1000000);
    CHECK(output.size() == 2 && near(sample(output[1], 100), 0.5f));
    mixer.setControl(CaptureSource::Kind::Loopback, 1, false);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 1, 480, Base + 200000, 0.2f));
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 480, Base + 200000, 0.1f, 0.1f));
    output = mixer.takeFrames(Base + 1100000);
    CHECK(output.size() == 1 && near(sample(output[0], 100), 0.5f)); // 不能重放静音前0.8的旧包。
    mixer.disable(CaptureSource::Kind::Microphone);
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 100, Base + 300000, 0.1f, 0.1f));
    output = mixer.finish();
    CHECK(output.size() == 1 && output[0].discontinuity);
    CHECK(near(sample(output[0], 99), 0.1f) && near(sample(output[0], 100), 0));
    CHECK(!mixer.active() && mixer.finish().isEmpty());
    CHECK(rejected([&] { mixer.setControl(CaptureSource::Kind::Microphone, std::numeric_limits<float>::quiet_NaN(), false); }));
    mixer.clear(); mixer.begin(true, false);
    auto input = packet(48000, 1, 480, Base + 10000000, 0.1f); input.timestampEstimated = true;
    mixer.push(CaptureSource::Kind::Microphone, input);
    output = mixer.takeFrames(input.timestamp100ns + 900000);
    CHECK(output.size() == 1 && output[0].timestamp100ns == input.timestamp100ns && output[0].timestampEstimated);
    qInfo("PASS gains / mute consumption / failed source isolation / partial tail / reset / estimated timestamp");
    return true;
}

bool boundedCatchup()
{
    AudioMixer mixer; mixer.begin(true, false);
    for (int index = 0; index < 20; ++index)
        mixer.push(CaptureSource::Kind::Microphone, packet(48000, 2, 4800, Base + qint64(index) * 1000000, index * 0.01f, index * 0.01f));
    const auto output = mixer.takeFrames(Base + 20800000);
    CHECK(output.size() == 20 && output[0].discontinuity);
    CHECK(output[0].timestamp100ns == Base + 18000000 && near(sample(output[0], 10), 0.18f));
    CHECK(mixer.takeFrames(Base + 20800000).isEmpty());
    CHECK(mixer.finish().size() <= 50);
    qInfo("PASS bounded buffering / 200ms catch-up / monotonic output / no duplicate delivery");
    return true;
}

bool syntheticWave()
{
    AudioMixer mixer; mixer.begin(true, true);
    QByteArray pcm;
    for (int index = 0; index < 100; ++index) {
        mixer.push(CaptureSource::Kind::Microphone, tone(44100, 1, index * 441, 441, 440, 0.3f));
        mixer.push(CaptureSource::Kind::Loopback, tone(48000, 2, index * 480, 480, 660, 0.3f));
        for (const auto &frame : mixer.takeFrames(Base + qint64(index + 1) * 100000)) pcm += frame.pcm;
    }
    for (const auto &frame : mixer.finish()) pcm += frame.pcm;
    CHECK(pcm.size() == 48000 * 8 && mixer.clippedSamples() == 0);
    const QString path = qEnvironmentVariable("OBS_AUDIO_TEST_WAV");
    if (!path.isEmpty()) {
        CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path); CHECK(file.open(QIODevice::WriteOnly));
        QDataStream writer(&file); writer.setByteOrder(QDataStream::LittleEndian);
        writer.writeRawData("RIFF", 4); writer << quint32(36 + 48000 * 4);
        writer.writeRawData("WAVEfmt ", 8); writer << quint32(16) << quint16(1) << quint16(2)
            << quint32(48000) << quint32(48000 * 4) << quint16(4) << quint16(16);
        writer.writeRawData("data", 4); writer << quint32(48000 * 4);
        for (qsizetype offset = 0; offset < pcm.size(); offset += sizeof(float)) {
            float value; std::memcpy(&value, pcm.constData() + offset, sizeof(value));
            writer << qint16(std::round(std::clamp(value, -1.0f, 1.0f) * 32767));
        }
        CHECK(writer.status() == QDataStream::Ok && file.size() == 44 + 48000 * 4);
    }
    qInfo("PASS one-second 44.1kHz mono + 48kHz stereo synthetic mix / drain / optional WAV");
    return true;
}
}
int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toUtf8(); std::fprintf(stderr, "%s\n", bytes.constData()); std::fflush(stderr);
    });
    QCoreApplication app(argc, argv);
    if (!resampling() || !formatsAndRestart() || !alignedMixing() || !gainMuteAndTail() || !boundedCatchup() || !syntheticWave()) return 1;
    qInfo("PASS 6 audio groups (synthetic data only; no capture devices opened)");
    return 0;
}
