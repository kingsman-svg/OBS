#include "HttpParser.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <string_view>

namespace obs::protocol {
namespace {
bool token(std::string_view text) {
    if (text.empty())
        return false;
    for (unsigned char c : text)
        if (!std::isalnum(c) &&
            std::string_view("!#$%&'*+-.^_`|~").find(c) == std::string_view::npos)
            return false;
    return true;
}
std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return std::string(value.substr(first, value.find_last_not_of(" \t") - first + 1));
}
} // namespace
int HttpParser::parse(net::Buffer &input, HttpRequest &request) {
    // 第一步只找头部边界；未找到时不消费任何字节，后续读取仍从同一请求起点解析。
    const auto bytes = input.view();
    const auto end = bytes.find("\r\n\r\n");
    if (end == std::string_view::npos)
        return bytes.size() > MaxHeader ? 431 : 0;
    if (end + 4 > MaxHeader)
        return 431;
    const auto lineEnd = bytes.find("\r\n");
    const auto line = bytes.substr(0, lineEnd);
    const auto first = line.find(' '), last = line.rfind(' ');
    if (first == std::string_view::npos || first == last || line.substr(last + 1) != "HTTP/1.1" ||
        !token(line.substr(0, first)))
        return 400;
    const auto target = line.substr(first + 1, last - first - 1);
    if (target.empty() || target.front() != '/' || target.size() > 2048)
        return 400;
    for (unsigned char c : target)
        if (c <= 32 || c == 127 || c == '#')
            return 400;
    HttpRequest parsed;
    parsed.method = std::string(line.substr(0, first));
    parsed.target = std::string(target);
    std::size_t position = lineEnd + 2;
    // 先规范化并校验全部头，再解释正文长度。禁止宽松修复歧义头部。
    while (position < end) {
        const auto next = bytes.find("\r\n", position);
        const auto header = bytes.substr(position, next - position);
        const auto colon = header.find(':');
        if (colon == std::string_view::npos || !token(header.substr(0, colon)))
            return 400; // 包括折行、冒号前空白等歧义写法，统一拒绝。
        auto name = std::string(header.substr(0, colon));
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (unsigned char c : header.substr(colon + 1))
            if ((c < 32 && c != '\t') || c == 127)
                return 400;
        // 当前接口无需重复头，直接拒绝，特别避免多个 Content-Length 的歧义。
        if (!parsed.headers.emplace(name, trim(header.substr(colon + 1))).second ||
            parsed.headers.size() > 100)
            return 400;
        position = next + 2;
    }
    if (!parsed.headers.count("host") || parsed.headers.at("host").empty() ||
        parsed.headers.count("transfer-encoding") || parsed.headers.count("expect"))
        return 400;
    std::size_t length = 0;
    if (const auto it = parsed.headers.find("content-length"); it != parsed.headers.end()) {
        const auto &value = it->second;
        const auto converted = std::from_chars(value.data(), value.data() + value.size(), length);
        if (value.empty() || converted.ec != std::errc{} ||
            converted.ptr != value.data() + value.size())
            return 400;
    } else if (parsed.method == "POST")
        return 411;
    if (length > MaxBody)
        return 413;
    // 长度已通过溢出和上限检查，只有正文完整时才发布请求并移动读索引。
    if (bytes.size() < end + 4 + length)
        return 0;
    parsed.body = std::string(bytes.substr(end + 4, length));
    request = std::move(parsed);
    input.retrieve(end + 4 + length);
    return 200;
}
} // namespace obs::protocol
