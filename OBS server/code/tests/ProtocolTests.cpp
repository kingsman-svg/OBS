#include "AuthService.h"
#include "FrameCodec.h"
#include "HttpParser.h"
#include "HttpServer.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

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
} // namespace
int main() {
    try {
        httpBoundaries();
        framing();
        tokens();
        std::cout << "3 protocol/auth behavior groups passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
