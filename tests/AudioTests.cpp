#include "AudioMixer.h"
#include "MediaTimeline.h"
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
bool sampleNear(float a, float b, float tolerance = 0.0001f) { return std::abs(a - b) < tolerance; }
bool rejected(const std::function<void()> &action)
{
    try { action(); } catch (const std::exception &) { return true; }
    return false;
}

bool resampling()
{
    // 实际 swr 处理 44.1kHz 正弦，任意分包必须与大包处理保持同一结果。
    QByteArray chunkedPcm, bulkPcm;
    for (int chunk : {137, 22050}) {
        AudioResampler resampler;
        auto &bytes = chunk == 137 ? chunkedPcm : bulkPcm;
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
    CHECK(chunkedPcm.size() == 48000 * 8 && chunkedPcm.size() == bulkPcm.size());
    AudioPacket output; output.pcm = chunkedPcm; output.channels = 2;
    double squaredError = 0;
    for (int frame = 100; frame < 47900; ++frame) {
        CHECK(sampleNear(sample(output, frame, 0), sample(output, frame, 1)));
        const double expected = 0.4 * std::sin(2 * Pi * 1000 * frame / 48000);
        squaredError += std::pow(sample(output, frame) - expected, 2);
        float other; std::memcpy(&other, bulkPcm.constData() + frame * 8, 4);
        CHECK(sampleNear(sample(output, frame), other, 0.00001f));
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
    CHECK(!output.pcm.isEmpty() && output.pcm.size() <= 96 * 8 && sampleNear(sample(output, 10, 0), 0.25f) && sampleNear(sample(output, 10, 1), 0.25f));
    CHECK(resampler.convert(input).pcm.isEmpty()); // 2ms短包重复也不能再输出。
    input = packet(32000, 2, 320, Base + 1000000, 0.1f, 0.2f);
    output = resampler.convert(input);
    CHECK(output.discontinuity && output.timestamp100ns == input.timestamp100ns);
    CHECK(output.channels == 2 && output.sampleRate == 48000);
    resampler.reset();
    input = packet(48000, 6, 480, Base, 0.1f, 0.2f); input.channelMask = 0x3f;
    output = resampler.convert(input);
    output.pcm += resampler.drain().pcm;
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
    auto output = mixer.takeFrames(Base + 1000000);
    CHECK(output.size() == 1); // swr和限幅的预读不产生伪造起始静音，最后有效包由finish交付。
    output += mixer.finish();
    CHECK(output.size() == 3 && output[0].pcm.size() == 480 * 8);
    CHECK(output[0].discontinuity && sampleNear(sample(output[0], 10), 0.6f));
    CHECK(!output[1].discontinuity && sampleNear(sample(output[1], 100), .98f) && sampleNear(sample(output[1], 100, 1), .98f * .5f / 1.2f));
    CHECK(!output[2].discontinuity && output[2].timestamp100ns == Base + 200000);
    CHECK(mixer.clippedSamples() == 960 && mixer.limitedFrames() > 0 && mixer.safetyClips() == 0);
    CHECK(mixer.finish().isEmpty());
    // 时间跳变不能抹掉已经排队但尚未混合的有效前段。
    mixer.begin(true, false);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 1, 480, Base, 0.1f));
    auto afterGap = packet(48000, 1, 480, Base + 200000, 0.3f); afterGap.discontinuity = true;
    mixer.push(CaptureSource::Kind::Microphone, afterGap);
    auto gap = mixer.finish();
    CHECK(gap.size() == 3 && sampleNear(sample(gap[0], 100), 0.1f));
    CHECK(gap[1].discontinuity && sampleNear(sample(gap[1], 100), 0));
    CHECK(gap[2].discontinuity && sampleNear(sample(gap[2], 100), 0.3f));
    qInfo("PASS QPC offset / fixed packets / missing source silence / stereo-linked limiting / no lost tail");
    return true;
}

bool gainMuteAndTail()
{
    AudioMixer mixer; mixer.setControl(CaptureSource::Kind::Microphone, 2, false);
    mixer.setControl(CaptureSource::Kind::Loopback, 1, true); mixer.begin(true, true);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 1, 960, Base, 0.25f));
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 960, Base, 0.8f, 0.8f));
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 1, 480, Base + 200000, 0.2f));
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 480, Base + 200000, 0.1f, 0.1f));
    auto output = mixer.takeFrames(Base + 1000000);
    CHECK(output.size() == 1 && sampleNear(sample(output[0], 100), 0.5f));
    mixer.setControl(CaptureSource::Kind::Loopback, 1, false);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 1, 480, Base + 300000, 0.2f));
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 480, Base + 300000, 0.1f, 0.1f));
    output = mixer.takeFrames(Base + 1100000);
    CHECK(output.size() == 1 && sampleNear(sample(output[0], 100), 0.5f)); // 不能重放静音前0.8的旧包。
    output = mixer.takeFrames(Base + 1200000);
    CHECK(output.size() == 1 && sampleNear(sample(output[0], 100), .4f + .1f * 101 / 480, .001f));
    CHECK(sampleNear(sample(output[0], 400), .4f + .1f * 401 / 480, .001f));
    mixer.disable(CaptureSource::Kind::Microphone);
    mixer.push(CaptureSource::Kind::Loopback, packet(48000, 2, 100, Base + 400000, 0.1f, 0.1f));
    output = mixer.finish();
    CHECK(output.size() == 2 && output.last().discontinuity);
    CHECK(sampleNear(sample(output.last(), 99), 0.1f) && sampleNear(sample(output.last(), 100), 0));
    CHECK(!mixer.active() && mixer.finish().isEmpty());
    CHECK(rejected([&] { mixer.setControl(CaptureSource::Kind::Microphone, std::numeric_limits<float>::quiet_NaN(), false); }));
    mixer.clear(); mixer.begin(true, false);
    auto input = packet(48000, 1, 480, Base + 10000000, 0.1f); input.timestampEstimated = true;
    mixer.push(CaptureSource::Kind::Microphone, input);
    output = mixer.finish();
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
    CHECK(output.size() == 19 && output[0].discontinuity); // 还有一包保留在限幅预读中。
    CHECK(output[0].timestamp100ns == Base + 18000000 && sampleNear(sample(output[0], 10), 0.18f));
    CHECK(mixer.takeFrames(Base + 20800000).isEmpty());
    CHECK(mixer.finish().size() <= 50);
    qInfo("PASS bounded buffering / 200ms catch-up / monotonic output / no duplicate delivery");
    return true;
}

bool gainRamps()
{
    AudioMixer mixer; mixer.begin(true, false);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 2, 4800, Base, .25f, .125f));
    auto output = mixer.takeFrames(Base + 1000000); CHECK(output.size() == 1);
    mixer.setControl(CaptureSource::Kind::Microphone, 1, true); // 从当前时间20ms处开始10ms淡出。
    output = mixer.takeFrames(Base + 1200000); CHECK(output.size() == 2);
    const auto &fading = output.last();
    CHECK(sampleNear(sample(fading, 0), .25f * 479 / 480) && sampleNear(sample(fading, 479), 0));
    for (int i = 1; i < 480; ++i) CHECK(std::abs(sample(fading, i) - sample(fading, i - 1)) <= .25f / 480 + .00001f);
    CHECK(sampleNear(sample(fading, 100), sample(fading, 100, 1) * 2));
    mixer.setControl(CaptureSource::Kind::Microphone, 2, false);
    output = mixer.takeFrames(Base + 1400000); CHECK(output.size() == 2);
    CHECK(sampleNear(sample(output.first(), 100), 0) && sampleNear(sample(output.last(), 479), .5f));
    mixer.clear(); mixer.begin(true, false);
    mixer.push(CaptureSource::Kind::Microphone, packet(48000, 2, 960, Base, .1f, .1f)); output = mixer.finish();
    CHECK(output.size() == 2 && sampleNear(sample(output.first(), 100), .2f)); // 新会话保留设置、清过渡状态。
    qInfo("PASS gain ramps: sample-continuous fade / stereo gain / mute consumes / unmute reaches target / restart"); return true;
}

bool limiterChecks()
{
    StereoLimiter limiter;
    auto low = packet(48000, 2, 480, Base, .6f, .2f), loud = packet(48000, 2, 480, Base + 100000, 1.5f, .5f);
    CHECK(limiter.push(low).pcm.isEmpty());
    auto output = limiter.push(loud); CHECK(output.timestamp100ns == Base && output.pcm.size() == 480 * 8);
    CHECK(sampleNear(sample(output, 0), .6f) && sample(output, 479) < .6f); // 峰值尚未到达，已经提前衰减。
    for (int i = 1; i < 480; ++i) CHECK(std::abs(sample(output, i) - sample(output, i - 1)) <= .6f / 240 + .00001f);
    auto quiet = packet(48000, 2, 480, Base + 200000, .15f, .05f);
    output = limiter.push(quiet); CHECK(output.timestamp100ns == loud.timestamp100ns);
    for (int i = 0; i < 480; ++i) CHECK(std::abs(sample(output, i)) <= .980001f && sampleNear(sample(output, i), sample(output, i, 1) * 3));
    output = limiter.drain(); CHECK(output.timestamp100ns == quiet.timestamp100ns && limiter.drain().pcm.isEmpty());
    CHECK(limiter.safetyClips() == 0 && limiter.limitedFrames() > 0 && limiter.gain() < 1);
    // 检查稳态正弦的2～5次谐波，相比原硬削波应明显减少；不把整体音量变小当作唯一验收。
    limiter.reset(); QByteArray pcm;
    for (int index = 0; index < 30; ++index) {
        auto block = tone(48000, 2, index * 480, 480, 1000, 1.5f);
        const auto limited = limiter.push(block); if (!limited.pcm.isEmpty()) pcm += limited.pcm;
    }
    pcm += limiter.drain().pcm; CHECK(pcm.size() == 30 * 480 * 8 && limiter.safetyClips() == 0);
    double power[2][6]{};
    for (int mode = 0; mode < 2; ++mode) for (int harmonic = 1; harmonic <= 5; ++harmonic) {
        double real = 0, imaginary = 0;
        for (int i = 4800; i < 14400; ++i) {
            float value; std::memcpy(&value, pcm.constData() + i * 8, 4);
            if (mode) value = std::clamp(1.5f * float(std::sin(2 * Pi * 1000 * i / 48000)), -1.0f, 1.0f);
            const double angle = 2 * Pi * 1000 * harmonic * i / 48000;
            real += value * std::cos(angle); imaginary += value * std::sin(angle);
        }
        power[mode][harmonic] = real * real + imaginary * imaginary;
    }
    double thd[2]{};
    for (int mode = 0; mode < 2; ++mode) { for (int harmonic = 2; harmonic <= 5; ++harmonic) thd[mode] += power[mode][harmonic]; thd[mode] = std::sqrt(thd[mode] / power[mode][1]); }
    CHECK(thd[0] < thd[1] / 4);
    qInfo("PASS linked limiter: 5ms anticipation / bounded slope / stereo ratio / valid tail / zero safety clips; harmonic THD %.4f%% vs hard clip %.4f%%", thd[0] * 100, thd[1] * 100);
    return true;
}

bool driftChecks()
{
    for (const int ppm : {-300, -100, 100, 300}) {
        AudioResampler resampler; qint64 outputFrames = 0, lastTimestamp = -1; double previousCorrection = 0; quint64 previousUpdates = 0;
        for (int index = 0; index < 600; ++index) {
            auto input = packet(48000, 2, 4800, Base + qint64(std::llround(index * 1000000.0 * (1 + ppm / 1000000.0))), .1f, .2f);
            const auto converted = resampler.convert(input);
            CHECK(!converted.pcm.isEmpty() && !converted.discontinuity && converted.timestamp100ns > lastTimestamp);
            CHECK(converted.timestamp100ns == Base + qint64(std::llround(outputFrames * 10000000.0 / 48000)));
            lastTimestamp = converted.timestamp100ns; outputFrames += converted.pcm.size() / 8;
            const auto state = resampler.drift(); CHECK(std::abs(state.correctionPpm) <= 1000.1 && state.restarts == 0 && state.rejected == 0);
            if (state.adjustments != previousUpdates) { CHECK(std::abs(state.correctionPpm - previousCorrection) <= 111); previousCorrection = state.correctionPpm; previousUpdates = state.adjustments; }
        }
        outputFrames += resampler.drain().pcm.size() / 8;
        const auto state = resampler.drift(); const double errorMs = (outputFrames - 48000 * 60 * (1 + ppm / 1000000.0)) * 1000 / 48000;
        CHECK(state.adjustments >= 50 && state.reliable && state.outliers == 0 && std::abs(errorMs) < 1);
        CHECK(std::abs(state.clockPpm - ppm) < 2 && state.correctionPpm * ppm > 0);
        qInfo("PASS drift %dppm: output=%lld error=%.4fms measured=%.2fppm correction=%.2fppm updates=%llu restarts=%llu", ppm, outputFrames, errorMs, state.clockPpm, state.correctionPpm, state.adjustments, state.restarts);
    }
    AudioResampler resampler;
    for (int i = 0; i < 30; ++i) {
        auto input = packet(48000, 1, 4800, Base + qint64(i) * 1001000, .1f); input.timestampEstimated = true;
        resampler.convert(input);
    }
    CHECK(!resampler.drift().reliable && resampler.drift().adjustments == 0 && resampler.drift().restarts == 0);
    auto jumped = packet(48000, 1, 4800, Base + 40000000, .1f); jumped.discontinuity = true;
    CHECK(resampler.convert(jumped).discontinuity && resampler.drift().restarts == 1);
    jumped.discontinuity = false;
    CHECK(resampler.convert(jumped).pcm.isEmpty() && resampler.drift().rejected == 1);
    resampler.reset(); CHECK(resampler.drift().adjustments == 0 && resampler.drift().restarts == 0);
    qint64 produced = 0;
    for (int i = 0; i < 600; ++i) {
        // 30秒后时钟速度改变，QPC连续；叠加±0.1ms包级抖动，不能误判成跳变或无限积分。
        const qint64 jitter = i % 3 == 0 ? 1000 : i % 3 == 1 ? -1000 : 0;
        auto input = packet(48000, 2, 4800, Base + qint64(i) * 1000000 + std::max(0, i - 300) * 300 + jitter, .1f, .1f);
        produced += resampler.convert(input).pcm.size() / 8;
    }
    produced += resampler.drain().pcm.size() / 8;
    CHECK(resampler.drift().restarts == 0 && resampler.drift().rejected == 0 && std::abs(produced - (2880000 + 432)) < 48);
    resampler.reset();
    for (int i = 0; i < 40; ++i) resampler.convert(packet(48000, 1, 4800, Base + qint64(i) * 1002500, .1f));
    CHECK(resampler.drift().outliers >= 2 && resampler.drift().correctionPpm == 0 && resampler.drift().restarts == 0);
    qInfo("PASS drift: estimated timestamps excluded / true jump / duplicate / reset / changing clock plus jitter / outlier suppression"); return true;
}

bool dualClockMixing()
{
    AudioMixer mixer; mixer.begin(true, true);
    MediaTimeline timeline; timeline.begin(Base, false, true);
    int next[2]{}, packets = 0, synchronized = 0; qint64 previous = -1, lastPts = -480; int checked = 0;
    const auto arrival = [&](int track) { return Base + qint64(std::llround((next[track] + 1) * 100000.0 * (1 + (track ? -300 : 300) / 1000000.0))); };
    const auto inspect = [&](const QList<AudioPacket> &batch) {
        for (const auto &output : batch) {
            if (!timeline.pushAudio(output)) return false;
            if (previous >= 0 && output.timestamp100ns != previous + 100000) return false;
            previous = output.timestamp100ns; ++packets;
            if (output.timestamp100ns >= Base + 10000000 && output.timestamp100ns < Base + 590000000) {
                if (output.discontinuity || !sampleNear(sample(output, 100), .3f, .001f) || !sampleNear(sample(output, 100, 1), .4f, .001f)) return false;
                ++checked;
            }
        }
        return true;
    };
    const auto inspectTimeline = [&](const MediaBatch &batch) {
        for (const auto &audio : batch.audio) {
            if (audio.pts != lastPts + 480) return false;
            lastPts = audio.pts; ++synchronized;
        }
        return true;
    };
    // 两设备按各自QPC交付时间合并调度，不能把快设备强行绑到慢设备每包一次。
    while (next[0] < 6000 || next[1] < 6000) {
        const int track = next[0] == 6000 ? 1 : next[1] == 6000 ? 0 : arrival(0) <= arrival(1) ? 0 : 1;
        const auto now = arrival(track), timestamp = Base + qint64(std::llround(next[track] * 100000.0 * (1 + (track ? -300 : 300) / 1000000.0)));
        mixer.push(track ? CaptureSource::Kind::Loopback : CaptureSource::Kind::Microphone,
            packet(48000, track ? 2 : 1, 480, timestamp, track ? .2f : .1f, .3f));
        ++next[track]; CHECK(inspect(mixer.takeFrames(now)));
        CHECK(inspectTimeline(timeline.takeFrames(now)));
    }
    CHECK(inspect(mixer.finish()));
    CHECK(inspectTimeline(timeline.finish()) && synchronized == packets && timeline.stats().droppedAudio == 0);
    const auto mic = mixer.drift(CaptureSource::Kind::Microphone), system = mixer.drift(CaptureSource::Kind::Loopback);
    CHECK(checked >= 5700 && packets >= 6000 && packets <= 6003 && mic.restarts == 0 && system.restarts == 0);
    CHECK(mic.adjustments >= 50 && system.adjustments >= 50 && mixer.safetyClips() == 0);
    qInfo("PASS dual clock mix: +300/-300ppm QPC-scheduled 60s; %d packets, %d steady blocks without holes/overlap/reset; mic/system correction %.2f/%.2fppm",
        packets, checked, mic.correctionPpm, system.correctionPpm);
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
    if (!resampling() || !formatsAndRestart() || !alignedMixing() || !gainMuteAndTail() || !boundedCatchup() || !syntheticWave()
        || !gainRamps() || !limiterChecks() || !driftChecks() || !dualClockMixing()) return 1;
    qInfo("PASS 10 audio groups (synthetic data only; no capture devices opened)");
    return 0;
}
