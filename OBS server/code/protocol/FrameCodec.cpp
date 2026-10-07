#include "FrameCodec.h"
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace obs::protocol {
FrameCodec::Result FrameCodec::next(net::Buffer &input, std::string &body) {
    if (input.readableBytes() < 4)
        return Result::Incomplete;
    std::uint32_t networkLength;
    std::memcpy(&networkLength, input.view().data(), 4); // 不做可能未对齐的指针强转。
    const auto length = ntohl(networkLength);
    if (length == 0 || length > MaxFrame)
        return Result::Invalid;
    if (input.readableBytes() < 4 + length)
        return Result::Incomplete;
    body = std::string(input.view().substr(4, length));
    input.retrieve(4 + length);
    return Result::Complete;
}
std::string FrameCodec::encode(const std::string &body) {
    // 长度是字节数而非字符数。帧组装成独立字符串，交给 TcpConnection 处理部分写。
    if (body.empty() || body.size() > MaxFrame)
        throw std::length_error("invalid signal frame length");
    const auto length = htonl(static_cast<std::uint32_t>(body.size()));
    std::string wire(reinterpret_cast<const char *>(&length), 4);
    wire += body;
    return wire;
}
} // namespace obs::protocol
