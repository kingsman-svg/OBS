#pragma once
#include "Channel.h"
#include "EventLoop.h"
#include "HttpServer.h"
#include <memory>

namespace obs::protocol {
// 调度器的异步健康探测：非阻塞 connect、部分写、响应限长及超时一次性收敛。
class HttpProbe final : public std::enable_shared_from_this<HttpProbe> {
  public:
    using Callback = std::function<void(bool)>;
    HttpProbe(net::EventLoop &loop, std::string address, std::uint16_t port, Callback callback);
    ~HttpProbe();
    void start();
    void stop();

  private:
    void write();
    void read();
    void finish(bool healthy);
    net::EventLoop &m_loop;
    std::string m_address;
    std::uint16_t m_port;
    Callback m_callback;
    net::UniqueFd m_socket;
    std::shared_ptr<net::Channel> m_channel;
    net::TimerId m_timeout;
    std::string m_output, m_input;
    std::size_t m_written = 0;
    bool m_finished = false;
};
} // namespace obs::protocol
