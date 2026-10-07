#pragma once
#include "Buffer.h"
#include <map>
#include <string>

namespace obs::protocol {
// headers 的键已统一为小写；body 持有数据，不借用连接缓冲区。
struct HttpRequest {
    std::string method, target, body;
    std::map<std::string, std::string> headers;
};

// 小型 HTTP/1.1 请求解析器：仅支持 Content-Length，每条连接处理一个请求。
class HttpParser final {
  public:
    static constexpr std::size_t MaxHeader = 16 * 1024;
    static constexpr std::size_t MaxBody = 64 * 1024;
    // 0：等待半包；200：完整并消费；其他：应返回的 HTTP 错误状态。
    static int parse(net::Buffer &input, HttpRequest &request);
};
} // namespace obs::protocol
