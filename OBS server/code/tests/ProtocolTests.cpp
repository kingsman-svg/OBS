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
#include "H264Adapter.h"
extern "C" {
#include <libavutil/mem.h>
}
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
#endif
        std::cout << "protocol/auth and deferred HTTP behavior groups passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
