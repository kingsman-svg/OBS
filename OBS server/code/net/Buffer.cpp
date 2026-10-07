#include "Buffer.h"
#include <stdexcept>

namespace obs::net {
// m_readIndex 之前的内容已被业务消费；解析半包时只移动索引，不反复搬移字符串。
std::size_t Buffer::readableBytes() const noexcept {
    return m_data.size() - m_readIndex;
}
std::string_view Buffer::view() const noexcept {
    return std::string_view(m_data).substr(m_readIndex);
}
void Buffer::append(std::string_view bytes) {
    // 上限针对“未消费数据”，并用减法检查，避免 size_t 加法溢出。
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
    // 越界属于协议解析器的编程错误，保留异常，不默默截断掩盖问题。
    if (count > readableBytes())
        throw std::out_of_range("retrieve exceeds readable bytes");
    m_readIndex += count;
    if (m_readIndex == m_data.size())
        retrieveAll();
}
void Buffer::retrieveAll() noexcept {
    // clear 保留字符串容量，后续收包可复用内存；连接关闭时随 Buffer 一起释放。
    m_data.clear();
    m_readIndex = 0;
}
} // namespace obs::net
