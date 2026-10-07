#include "Buffer.h"
#include <stdexcept>

namespace obs::net {
std::size_t Buffer::readableBytes() const noexcept {
    return m_data.size() - m_readIndex;
}
std::string_view Buffer::view() const noexcept {
    return std::string_view(m_data).substr(m_readIndex);
}
void Buffer::append(std::string_view bytes) {
    if (bytes.size() > MaxBytes - readableBytes())
        throw std::length_error("network buffer exceeds 8 MiB");
    // 拷贝后再整理，允许 append(view())，避免内部地址因 erase/扩容而失效。
    const std::string copy(bytes);
    if (m_readIndex != 0) {
        m_data.erase(0, m_readIndex);
        m_readIndex = 0;
    }
    m_data.append(copy);
}
void Buffer::retrieve(std::size_t count) {
    if (count > readableBytes())
        throw std::out_of_range("retrieve exceeds readable bytes");
    m_readIndex += count;
    if (m_readIndex == m_data.size())
        retrieveAll();
}
void Buffer::retrieveAll() noexcept {
    m_data.clear();
    m_readIndex = 0;
}
} // namespace obs::net
