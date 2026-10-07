#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace obs::net {
// TCP 只提供有序字节流。本类保留未消费的数据，不假设一次 recv 就是一个消息。
// 上层 HTTP / 长度头解析器应在收到完整消息后 retrieve()，可以一次解析多个消息。
class Buffer final {
  public:
    static constexpr std::size_t MaxBytes = 8 * 1024 * 1024;
    std::size_t readableBytes() const noexcept;
    std::string_view view() const noexcept;
    void append(std::string_view bytes);
    void retrieve(std::size_t count);
    void retrieveAll() noexcept;

  private:
    std::string m_data;
    std::size_t m_readIndex = 0;
};
} // namespace obs::net
