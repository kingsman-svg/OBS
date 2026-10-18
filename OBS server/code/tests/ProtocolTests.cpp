#include "AuthService.h"
#include "FrameCodec.h"
#include "HttpParser.h"
#include "HttpServer.h"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#ifdef OBS_HAVE_MEDIA
#include "AudioTranscoder.h"
#include "H264Adapter.h"
#include "RtpClock.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}
#include <cmath>
#endif

using namespace obs::protocol;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int parse(const std::string &wire) {
    obs::net::Buffer input;
    input.append(wire);
    HttpRequest request;
    return HttpParser::parse(input, request);
}
void httpBoundaries() {
    const std::string wire =
        "POST /auth/login HTTP/1.1\r\nHost: local\r\nContent-Length: 2\r\n\r\n{}";
    obs::net::Buffer input;
    HttpRequest request;
    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        input.append(wire.substr(i, 1));
        check(HttpParser::parse(input, request) == 0, "HTTP fragmented request");
    }
    input.append(wire.substr(wire.size() - 1));
    input.append("NEXT");
    check(HttpParser::parse(input, request) == 200 && request.body == "{}" &&
              input.view() == "NEXT",
          "HTTP exact consumption");
    check(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n") ==
              400,
          "duplicate length rejected");
    check(parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: "
                "0\r\n\r\n") == 400,
          "TE/CL ambiguity rejected");
    check(parse("POST / HTTP/1.1\r\nHost: x\r\n\r\n") == 411, "missing POST length");
    check(parse("GET / HTTP/1.1\r\n\r\n") == 400, "missing host");
    check(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n") == 400,
          "negative length");
    check(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n") ==
              400,
          "overflow length");
    check(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\n\r\n") == 413,
          "body limit");
    check(parse(std::string(16385, 'x')) == 431, "header limit");
    check(parse("GET / HTTP/1.1\r\nHost : x\r\n\r\n") == 400, "header whitespace");
    check(parse("GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n") == 400, "obs-fold rejected");
}
void framing() {
    obs::net::Buffer input;
    const auto a = FrameCodec::encode("{}"), b = FrameCodec::encode("{\"type\":\"heartbeat\"}");
    std::string body;
    input.append(a.substr(0, 2));
    check(FrameCodec::next(input, body) == FrameCodec::Result::Incomplete, "partial header");
    input.append(a.substr(2, 3));
    check(FrameCodec::next(input, body) == FrameCodec::Result::Incomplete, "partial body");
    input.append(a.substr(5) + b);
    check(FrameCodec::next(input, body) == FrameCodec::Result::Complete && body == "{}",
          "first frame");
    check(FrameCodec::next(input, body) == FrameCodec::Result::Complete &&
              input.readableBytes() == 0,
          "coalesced second frame");
    input.append(std::string(4, '\0'));
    check(FrameCodec::next(input, body) == FrameCodec::Result::Invalid, "zero frame rejected");
    bool rejected = false;
    try {
        FrameCodec::encode(std::string(65537, 'x'));
    } catch (const std::length_error &) {
        rejected = true;
    }
    check(rejected, "frame output limit");
}
void tokens() {
    obs::services::AuthService auth(std::string(32, 'a'), "root", 1);
    obs::services::AuthService other(std::string(32, 'b'), "root");
    check(auth.checkPassword("root", "root") && !auth.checkPassword("root", "wrong") &&
              !auth.checkPassword("other", "root"),
          "development credentials");
    std::string user;
    std::int64_t expires = 0;
    auto token = auth.issue("root");
    check(auth.verify(token, user, expires) && user == "root", "signed token accepted");
    check(!other.verify(token, user, expires), "different signing key rejected");
    auto changed = token;
    changed.back() = changed.back() == 'a' ? 'b' : 'a';
    check(!auth.verify(changed, user, expires), "tampered signature rejected");
    check(!auth.verify("v1.invalid.bad", user, expires), "malformed token rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(!auth.verify(token, user, expires) && user.empty() && expires == 0,
          "expired token rejected");
}
void deferredHttp() {
    obs::net::EventLoop loop;
    int dispatched = 0;
    HttpServer::Reply late;
    auto server = std::make_unique<HttpServer>(
        loop, "127.0.0.1", 0,
        HttpServer::AsyncHandler([&](const HttpRequest &, HttpServer::Reply reply) {
            ++dispatched;
            late = reply;
            loop.runAfter(std::chrono::milliseconds(20), [reply] {
                reply({200, "deferred-html", "text/html; charset=utf-8"});
                reply(HttpReply::json(500, {{"duplicate", true}}));
            });
        }));
    server->start();
    const auto port = server->port();
    auto client = std::async(std::launch::async, [port] {
        obs::net::UniqueFd socket(::socket(AF_INET, SOCK_STREAM, 0));
        timeval timeout{2, 0};
        ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        check(::connect(socket.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
              "deferred HTTP connect");
        const std::string request = "GET / HTTP/1.1\r\nHost: local\r\n\r\n";
        const auto pipeline = request + request;
        check(::send(socket.get(), pipeline.data(), pipeline.size(), MSG_NOSIGNAL) ==
                  ssize_t(pipeline.size()),
              "deferred HTTP send");
        std::string response;
        char buffer[1024];
        for (;;) {
            const auto count = ::recv(socket.get(), buffer, sizeof(buffer), 0);
            if (count == 0)
                break;
            check(count > 0, "deferred HTTP receive timeout");
            response.append(buffer, count);
        }
        return response;
    });
    auto deadline = loop.runAfter(std::chrono::seconds(3), [&] { loop.quit(); });
    auto poll = loop.runEvery(std::chrono::milliseconds(5), [&] {
        if (client.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            loop.quit();
    });
    loop.loop();
    loop.cancelTimer(deadline);
    loop.cancelTimer(poll);
    const auto response = client.get();
    check(dispatched == 1 && response.find("deferred-html") != std::string::npos &&
              response.find("text/html") != std::string::npos &&
              response.find("duplicate") == std::string::npos,
          "async reply must dispatch once and ignore duplicate response");
    server.reset();
    late(HttpReply::json(200, {{"late", true}})); // 已析构：弱存活标记防止访问失效 this。
}
#ifdef OBS_HAVE_MEDIA
void h264Adapter() {
    using obs::media::H264Adapter;
    // 最小 avcC：保留 SPS/PPS 元数据；输入 IDR 只有 AVCC 长度前缀。
    const unsigned char extra[] = {1,    0x42, 0xc0, 0x1f, 0xff, 0xe1, 0,    4,   0x67,
                                   0x42, 0xc0, 0x1f, 1,    0,    2,    0x68, 0xce};
    auto *parameters = avcodec_parameters_alloc();
    auto *packet = av_packet_alloc();
    check(parameters && packet, "allocate H264 test resources");
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_H264;
    parameters->extradata_size = sizeof(extra);
    parameters->extradata =
        static_cast<std::uint8_t *>(av_mallocz(sizeof(extra) + AV_INPUT_BUFFER_PADDING_SIZE));
    std::copy(std::begin(extra), std::end(extra), parameters->extradata);
    {
        H264Adapter adapter(*parameters, AVRational{1, 1000});
        check(adapter.profile() == "42c01f", "real H264 profile retained");
        check(av_new_packet(packet, 6) == 0, "allocate AVCC packet");
        const unsigned char avcc[] = {0, 0, 0, 2, 0x65, 0x88};
        std::copy(std::begin(avcc), std::end(avcc), packet->data);
        packet->pts = packet->dts = 1234;
        const auto result = adapter.convert(*packet, 7);
        check(result.idr && result.ptsUs == 1234000 && result.epoch == 7 &&
                  result.bytes->size() > 6,
              "AVCC to Annex B with parameter sets and unchanged source PTS");
        av_packet_unref(packet);
        check(H264Adapter::hasIdr(*result.bytes),
              "access unit must own bytes after packet release");
        const unsigned char pframe[] = {0, 0, 1, 0x41, 0x80};
        std::vector<std::byte> bytes;
        for (auto value : pframe)
            bytes.push_back(std::byte(value));
        check(!H264Adapter::hasIdr(bytes), "P frame is not IDR");
        bool invalid = false;
        try {
            H264Adapter::hasIdr({std::byte{1}, std::byte{2}});
        } catch (const std::invalid_argument &) {
            invalid = true;
        }
        check(invalid, "invalid H264 boundaries rejected");
        packet->pts = 1;
        packet->dts = 0;
        invalid = false;
        try {
            adapter.convert(*packet, 7);
        } catch (const std::invalid_argument &) {
            invalid = true;
        }
        check(invalid, "reordering input explicitly rejected");
    }
    av_packet_free(&packet);
    avcodec_parameters_free(&parameters);
}
void audioTranscoding(int rate) {
    // 真正 AAC 编码→生产转码器→真正 Opus 解码，覆盖 44.1k mono 和 48k stereo。
    const int channels = rate == 44100 ? 1 : 2;
    auto *aac = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_AAC));
    check(aac != nullptr, "AAC test encoder");
    aac->sample_rate = rate;
    aac->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aac->time_base = {1, rate};
    aac->bit_rate = 96000;
    av_channel_layout_default(&aac->ch_layout, channels);
    check(avcodec_open2(aac, aac->codec, nullptr) == 0, "open AAC test encoder");
    auto *parameters = avcodec_parameters_alloc();
    check(parameters && avcodec_parameters_from_context(parameters, aac) == 0,
          "copy AAC parameters");
    std::vector<AVPacket *> packets;
    auto *packet = av_packet_alloc();
    auto collect = [&] {
        while (avcodec_receive_packet(aac, packet) == 0) {
            packets.push_back(av_packet_clone(packet));
            av_packet_unref(packet);
        }
    };
    for (int n = 0; n < 31; ++n) {
        auto *input = av_frame_alloc();
        input->format = AV_SAMPLE_FMT_FLTP;
        input->sample_rate = rate;
        check(av_channel_layout_copy(&input->ch_layout, &aac->ch_layout) == 0, "AAC layout");
        input->nb_samples = aac->frame_size;
        input->pts = rate + std::int64_t(n) * input->nb_samples;
        check(av_frame_get_buffer(input, 0) == 0, "AAC samples");
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < input->nb_samples; ++i)
                reinterpret_cast<float *>(input->data[c])[i] =
                    .2f *
                    std::sin(2 * 3.141592653589793 * 997 * (n * input->nb_samples + i) / rate);
        check(avcodec_send_frame(aac, input) == 0, "send AAC test samples");
        collect();
        av_frame_free(&input);
    }
    check(avcodec_send_frame(aac, nullptr) == 0, "flush AAC test samples");
    collect();
    check(packets.size() >= 31, "AAC packets generated");
    obs::media::AudioTranscoder transcoder(*parameters, AVRational{1, rate});
    std::vector<obs::media::MediaUnit> units;
    for (auto *input : packets) {
        auto out = transcoder.push(*input, 9);
        units.insert(units.end(), out.begin(), out.end());
    }
    auto tail = transcoder.finish(9);
    units.insert(units.end(), tail.begin(), tail.end());
    check(transcoder.finish(9).empty(), "audio tail is drained once");
    const auto stats = transcoder.stats();
    check(stats.resets == 0 && stats.inputRate == rate && stats.encoderDelaySamples > 0 &&
              units.size() > 20,
          "resampling should retain a continuous source clock");
    check(std::llabs(std::int64_t(stats.resampledSamples) -
                     av_rescale(stats.decodedSamples, 48000, rate)) <= 1,
          "SWR drain must preserve converted sample count");
    const auto firstSample =
        av_rescale_q(packets.front()->pts, AVRational{1, rate}, AVRational{1, 48000}) -
        stats.encoderDelaySamples;
    check(std::llabs(units.front().ptsUs -
                     av_rescale_q(firstSample, AVRational{1, 48000}, AVRational{1, 1000000})) <= 1,
          "Opus delay is represented once by the encoder packet PTS");
    auto *opus = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_OPUS));
    check(opus != nullptr, "Opus test decoder");
    opus->sample_rate = 48000;
    av_channel_layout_default(&opus->ch_layout, 2);
    check(avcodec_open2(opus, opus->codec, nullptr) == 0, "open Opus decoder");
    auto *decoded = av_frame_alloc();
    double energy = 0, stereoError = 0;
    std::uint64_t samples = 0;
    std::int64_t previous = -1;
    for (const auto &unit : units) {
        check(unit.kind == obs::media::MediaKind::Audio && unit.epoch == 9 &&
                  unit.durationUs == 20000,
              "Opus unit must own its timeline metadata");
        if (previous >= 0)
            check(unit.ptsUs - previous == 20000, "Opus timestamps step by 960 samples");
        previous = unit.ptsUs;
        check(av_new_packet(packet, static_cast<int>(unit.bytes->size())) == 0,
              "Opus packet allocation");
        std::copy(unit.bytes->begin(), unit.bytes->end(),
                  reinterpret_cast<std::byte *>(packet->data));
        check(avcodec_send_packet(opus, packet) == 0, "decode generated Opus");
        av_packet_unref(packet);
        while (avcodec_receive_frame(opus, decoded) == 0) {
            check(decoded->sample_rate == 48000 && decoded->ch_layout.nb_channels == 2 &&
                      decoded->format == AV_SAMPLE_FMT_FLTP,
                  "decoded Opus is 48k stereo");
            const auto *left = reinterpret_cast<const float *>(decoded->data[0]);
            const auto *right = reinterpret_cast<const float *>(decoded->data[1]);
            for (int i = 0; i < decoded->nb_samples; ++i) {
                energy += left[i] * left[i];
                stereoError += std::pow(left[i] - right[i], 2);
            }
            samples += decoded->nb_samples;
            av_frame_unref(decoded);
        }
    }
    check(samples > 30000 && energy / samples > .001 && stereoError / samples < .0004,
          "Opus roundtrip must contain real audio and preserve duplicated stereo");
    // 同一源制造 500ms 缺口：只重建输出段，不把缺口补为静音并重新积压。
    obs::media::AudioTranscoder gap(*parameters, AVRational{1, rate});
    unsigned discontinuities = 0;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        auto *copy = av_packet_clone(packets[i]);
        if (i >= 8) {
            copy->pts += rate / 2;
            copy->dts += rate / 2;
        }
        for (const auto &unit : gap.push(*copy, 10))
            discontinuities += unit.discontinuity;
        av_packet_free(&copy);
    }
    gap.finish(10);
    check(gap.stats().resets == 1 && discontinuities == 1, "audio gap must mark one new segment");
    std::cout << "audio gateway " << rate << "Hz: decoded=" << stats.decodedSamples
              << " resampled=" << stats.resampledSamples << " packets=" << units.size()
              << " delay=" << stats.encoderDelaySamples << " samples\n";
    av_frame_free(&decoded);
    avcodec_free_context(&opus);
    avcodec_free_context(&aac);
    avcodec_parameters_free(&parameters);
    av_packet_free(&packet);
    for (auto *p : packets)
        av_packet_free(&p);
}
void commonRtpClock() {
    using namespace obs::media;
    using namespace std::chrono;
    const auto start = RtpClock::Steady::now();
    RtpClock clock(start, system_clock::time_point(seconds(1000)));
    MediaUnit video;
    video.epoch = 1;
    video.ptsUs = 1000000;
    video.idr = true;
    MediaUnit audio;
    audio.kind = MediaKind::Audio;
    audio.epoch = 1;
    audio.ptsUs = 1080000;
    audio.durationUs = 20000;
    check(!clock.map(audio, start), "audio must wait for shared IDR start");
    const auto v = clock.map(video, start + seconds(1));
    const auto a = clock.map(audio, start + seconds(1) + milliseconds(90));
    check(v && a && *a - *v == 80000,
          "source 80ms audio offset must survive unequal arrival times");
    const auto t = start + hours(15) + microseconds(12345);
    const auto ticks = std::uint32_t(15LL * 3600 * 90000 + 12345LL * 90000 / 1000000);
    check(clock.rtpNow(90000, 0xfffffff0U, t) == 0xfffffff0U + ticks, "RTP clock wrap is defined");
    clock.resync();
    check(!clock.map(audio, start + seconds(2)), "crop must gate both tracks");
    video.epoch = 2;
    video.ptsUs = 0;
    const auto next = clock.map(video, start + seconds(3));
    check(next && *next > *a && clock.reanchors() == 2,
          "source reset must map both tracks forward");
    // 实际序列化两份 SR：包内假装是旧帧，SR 必须仍报告现在的共同理论钟。
    auto shared = std::make_shared<RtpClock>(start - seconds(2), system_clock::now() - seconds(2));
    auto vr = std::make_shared<rtc::RtpPacketizationConfig>(11, "same-cname", 96, 90000);
    auto ar = std::make_shared<rtc::RtpPacketizationConfig>(12, "same-cname", 111, 48000);
    vr->startTimestamp = 0xffffff00U;
    ar->startTimestamp = 1234;
    ClockSenderReport vs(vr, shared), as(ar, shared);
    auto report = [](ClockSenderReport &sender, std::uint32_t ssrc) {
        auto packet = rtc::make_message(32);
        auto *header = reinterpret_cast<rtc::RtpHeader *>(packet->data());
        header->preparePacket();
        header->setSsrc(ssrc);
        header->setTimestamp(42);
        rtc::message_vector messages{packet};
        rtc::message_ptr result;
        sender.outgoing(messages, [&](auto message) { result = message; });
        check(bool(result), "SR must be emitted with first packet");
        return result;
    };
    const auto vb = report(vs, 11), ab = report(as, 12);
    const auto *srV = reinterpret_cast<const rtc::RtcpSr *>(vb->data());
    const auto *srA = reinterpret_cast<const rtc::RtcpSr *>(ab->data());
    const double rtpDelta =
        double(std::uint32_t(srA->rtpTimestamp() - ar->startTimestamp)) / 48000 -
        double(std::uint32_t(srV->rtpTimestamp() - vr->startTimestamp)) / 90000;
    const double ntpDelta = double(srA->ntpTimestamp() - srV->ntpTimestamp()) / 4294967296.0;
    check(std::abs(rtpDelta - ntpDelta) < 1.0 / 48000,
          "audio/video SR mapping differs by less than one audio sample");
    check(srV->packetCount() == 1 && srV->octetCount() == 20 && srV->rtpTimestamp() != 42,
          "SR counters and send-time theoretical clock");
    std::cout << "gateway SR common-clock error=" << std::abs(rtpDelta - ntpDelta) * 1000000
              << "us\n";
}
#endif
} // namespace
int main() {
    try {
        httpBoundaries();
        framing();
        tokens();
        deferredHttp();
#ifdef OBS_HAVE_MEDIA
        h264Adapter();
        audioTranscoding(44100);
        audioTranscoding(48000);
        commonRtpClock();
#endif
        std::cout << "protocol/auth and deferred HTTP behavior groups passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
