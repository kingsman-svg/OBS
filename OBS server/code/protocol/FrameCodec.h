#pragma once
#include "Buffer.h"
#include <string>

namespace obs::protocol {
// 四字节大端长度 + UTF-8 JSON。这里只处理帧边界，不判断 JSON 业务字段。
class FrameCodec final {
  public:
    static constexpr std::size_t MaxFrame = 64 * 1024;
    enum class Result { Incomplete, Complete, Invalid };
    static Result next(net::Buffer &input, std::string &body);
    static std::string encode(const std::string &body);
};
} // namespace obs::protocol
