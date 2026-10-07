#include "Buffer.h"
#include "EventLoop.h"
#include "TcpServer.h"
#include "UniqueFd.h"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>

using namespace obs::net;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
using Post = std::function<void(EventLoop::Task)>;

// 真正的 Linux TCP 测试。EventLoop 在工作线程构造和析构，客户端运行在测试线程。
// mutex 只保护测试控制入口的生命期，不参与被测库的 I/O 调度。
void withServer(const std::function<void(TcpServer &, EventLoop &)> &configure,
                const std::function<void(std::uint16_t, const Post &)> &client) {
    std::promise<std::uint16_t> ready;
    auto future = ready.get_future();
    std::mutex mutex;
    EventLoop *control = nullptr;
    std::exception_ptr serverError;
    std::thread worker([&] {
        try {
            EventLoop loop;
            {
                std::lock_guard<std::mutex> lock(mutex);
                control = &loop;
            }
            try {
                TcpServer server(loop, "127.0.0.1", 0);
                configure(server, loop);
                server.start();
                ready.set_value(server.port());
                loop.loop();
                server.stop();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                control = nullptr;
                throw;
            }
            std::lock_guard<std::mutex> lock(mutex);
            control = nullptr;
        } catch (...) {
            serverError = std::current_exception();
            try {
                ready.set_exception(serverError);
            } catch (const std::future_error &) {
            }
        }
    });
    Post post = [&](EventLoop::Task task) {
        std::lock_guard<std::mutex> lock(mutex);
        check(control != nullptr, "loop no longer available");
        control->queueInLoop(std::move(task));
    };
    std::exception_ptr clientError;
    try {
        check(future.wait_for(5s) == std::future_status::ready, "server start timed out");
        client(future.get(), post);
    } catch (...) {
        clientError = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (control)
            control->quit();
    }
    worker.join();
    if (serverError)
        std::rethrow_exception(serverError);
    if (clientError)
        std::rethrow_exception(clientError);
}
UniqueFd connectTo(std::uint16_t port) {
    UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    check(fd.get() >= 0, "client socket");
    const timeval timeout{4, 0};
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    check(::connect(fd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
          "client connect");
    return fd;
}
void sendAll(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        check(count > 0, "client send");
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}
std::string receive(int fd, std::size_t size) {
    std::string result(size, '\0');
    std::size_t read = 0;
    while (read < size) {
        const auto count = ::recv(fd, result.data() + read, size - read, 0);
        if (count < 0 && errno == EINTR)
            continue;
        check(count > 0, "client receive");
        read += static_cast<std::size_t>(count);
    }
    return result;
}
bool closed(int fd) {
    char byte;
    return ::recv(fd, &byte, 1, 0) == 0;
}
void echo(TcpServer &server, EventLoop &) {
    server.setMessageCallback([](const TcpConnection::Ptr &connection, Buffer &input) {
        const std::string data(input.view());
        input.retrieveAll();
        connection->send(data);
    });
}
std::size_t fdCount() {
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                      std::filesystem::directory_iterator()));
}
void bufferAndFdOwnership() {
    Buffer buffer;
    buffer.append(std::string_view("a\0b", 3));
    buffer.retrieve(1);
    check(buffer.view() == std::string_view("\0b", 2), "binary buffer");
    buffer.append(buffer.view());
    check(buffer.readableBytes() == 4, "self append");
    bool rejected = false;
    try {
        buffer.retrieve(5);
    } catch (const std::out_of_range &) {
        rejected = true;
    }
    check(rejected, "buffer retrieve bound");
    buffer.retrieveAll();
    rejected = false;
    try {
        buffer.append(std::string(Buffer::MaxBytes + 1, 'x'));
    } catch (const std::length_error &) {
        rejected = true;
    }
    check(rejected && buffer.readableBytes() == 0, "buffer append bound");
    const auto before = fdCount();
    {
        UniqueFd a(::socket(AF_INET, SOCK_STREAM, 0));
        UniqueFd b(std::move(a));
        check(a.get() == -1 && b.get() >= 0, "fd move ownership");
    }
    check(fdCount() == before, "fd release");
}
void crossThreadWakeup() {
    withServer(echo, [](std::uint16_t, const Post &post) {
        std::promise<bool> done;
        auto future = done.get_future();
        const auto caller = std::this_thread::get_id();
        post([&] { done.set_value(std::this_thread::get_id() != caller); });
        check(future.wait_for(500ms) == std::future_status::ready && future.get(),
              "eventfd prompt wakeup");
    });
    EventLoop loop;
    std::atomic<bool> rejected{false};
    std::thread wrong([&] {
        try {
            loop.assertInLoopThread();
        } catch (const std::logic_error &) {
            rejected = true;
        }
    });
    wrong.join();
    check(rejected, "thread affinity guard");
}
void fragmentedAndCombinedMessages() {
    withServer(
        [](TcpServer &server, EventLoop &) {
            server.setMessageCallback([](const TcpConnection::Ptr &conn, Buffer &input) {
                while (true) {
                    const auto end = input.view().find('\n');
                    if (end == std::string_view::npos)
                        return;
                    const std::string frame(input.view().substr(0, end + 1));
                    input.retrieve(end + 1);
                    conn->send(frame);
                }
            });
        },
        [](std::uint16_t port, const Post &) {
            auto fd = connectTo(port);
            sendAll(fd.get(), "hel");
            pollfd event{fd.get(), POLLIN, 0};
            check(::poll(&event, 1, 50) == 0, "incomplete frame must wait");
            const std::string expected = "hello\none\ntwo\n";
            sendAll(fd.get(), "lo\none\ntwo\n");
            check(receive(fd.get(), expected.size()) == expected, "fragmented and combined frames");
        });
}
void concurrentClientsAndFdReuse() {
    const auto before = fdCount();
    withServer(echo, [](std::uint16_t port, const Post &) {
        std::vector<std::thread> clients;
        std::atomic<int> failures{0};
        for (int i = 0; i < 12; ++i)
            clients.emplace_back([&, i] {
                try {
                    for (int j = 0; j < 8; ++j) {
                        auto fd = connectTo(port);
                        const std::string data(32768, static_cast<char>(i));
                        sendAll(fd.get(), data);
                        check(receive(fd.get(), data.size()) == data, "concurrent echo");
                    }
                } catch (...) {
                    ++failures;
                }
            });
        for (auto &client : clients)
            client.join();
        check(failures == 0, "concurrent clients");
    });
    check(fdCount() == before, "no descriptor leaks after connections and loop destruction");
}
void partialWriteAndPeerHalfClose() {
    const std::string payload(6 * 1024 * 1024, 'p');
    std::atomic<bool> buffered{false};
    withServer(
        [&](TcpServer &server, EventLoop &) {
            server.setMessageCallback([&](const TcpConnection::Ptr &connection, Buffer &input) {
                input.retrieveAll();
                connection->send(payload);
                buffered = connection->pendingBytes() > 0;
            });
        },
        [&](std::uint16_t port, const Post &) {
            auto fd = connectTo(port);
            sendAll(fd.get(), "request");
            ::shutdown(fd.get(), SHUT_WR);
            std::this_thread::sleep_for(30ms); // 明确制造慢读，要求服务端处理部分写。
            check(receive(fd.get(), payload.size()) == payload,
                  "all buffered output delivered after peer half-close");
            check(closed(fd.get()), "close after drained output");
            check(buffered, "partial write branch exercised");
        });
}
void gracefulShutdownAndReset() {
    std::atomic<int> connections{0};
    withServer(
        [&](TcpServer &server, EventLoop &) {
            server.setConnectionCallback([&](const TcpConnection::Ptr &connection) {
                if (++connections == 1) {
                    connection->send("bye");
                    connection->shutdown();
                } else
                    connection->send(std::string(1024 * 1024, 'x'));
            });
        },
        [](std::uint16_t port, const Post &) {
            auto first = connectTo(port);
            check(receive(first.get(), 3) == "bye" && closed(first.get()), "graceful SHUT_WR");
            first.reset();
            auto reset = connectTo(port);
            const linger abortive{1, 0};
            ::setsockopt(reset.get(), SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive));
            reset.reset();
            // RST / EPIPE 不应触发 SIGPIPE，后续连接仍能正常使用同一个 loop。
            auto next = connectTo(port);
            check(receive(next.get(), 1024) == std::string(1024, 'x'), "survive peer reset");
        });
}
void crossThreadSendAndForceClose() {
    std::promise<TcpConnection::Ptr> accepted;
    auto future = accepted.get_future();
    withServer(
        [&](TcpServer &server, EventLoop &) {
            server.setConnectionCallback(
                [&](const TcpConnection::Ptr &connection) { accepted.set_value(connection); });
        },
        [&](std::uint16_t port, const Post &) {
            auto fd = connectTo(port);
            check(future.wait_for(1s) == std::future_status::ready, "connection callback");
            auto connection = future.get();
            connection->send(std::string("cross-thread"));
            check(receive(fd.get(), 12) == "cross-thread", "cross-thread data copied and sent");
            connection->forceClose();
            check(closed(fd.get()), "cross-thread force close");
        });
}
void stopInsideAcceptCallback() {
    withServer(
        [](TcpServer &server, EventLoop &) {
            server.setConnectionCallback([&server](const TcpConnection::Ptr &) { server.stop(); });
        },
        [](std::uint16_t port, const Post &) {
            auto fd = connectTo(port);
            check(closed(fd.get()), "stop server during accept callback");
        });
}
void stopInsideMessageCallback() {
    withServer(
        [](TcpServer &server, EventLoop &) {
            server.setMessageCallback(
                [&server](const TcpConnection::Ptr &, Buffer &) { server.stop(); });
        },
        [](std::uint16_t port, const Post &) {
            auto fd = connectTo(port);
            sendAll(fd.get(), "stop");
            check(closed(fd.get()), "stop server during message callback");
        });
}
void bufferLimitsAndBindFailure() {
    withServer(
        [](TcpServer &server, EventLoop &) {
            server.setConnectionCallback([](const TcpConnection::Ptr &connection) {
                connection->send(std::string(Buffer::MaxBytes + 1, 'x'));
            });
        },
        [](std::uint16_t port, const Post &) {
            auto fd = connectTo(port);
            check(closed(fd.get()), "output limit closes connection");
        });
    std::promise<std::string> closeReason;
    auto future = closeReason.get_future();
    withServer(
        [&](TcpServer &server, EventLoop &) {
            // 不消费输入，模拟上层一直等不到合法消息结束，验证网络层容量保护。
            server.setMessageCallback([](const TcpConnection::Ptr &, Buffer &) {});
            server.setDisconnectCallback(
                [&](const TcpConnection::Ptr &, const std::string &reason) {
                    closeReason.set_value(reason);
                });
        },
        [&](std::uint16_t port, const Post &) {
            auto fd = connectTo(port);
            try {
                sendAll(fd.get(), std::string(Buffer::MaxBytes + 32768, 'x'));
            } catch (const std::runtime_error &) {
            } // 服务端可在客户端发送完成前关闭。
            check(future.wait_for(4s) == std::future_status::ready, "input limit disconnect");
            check(future.get() == "input buffer exceeds 8 MiB", "input limit reason");
        });
    const auto before = fdCount();
    {
        EventLoop loop;
        TcpServer server(loop, "127.0.0.1", 0);
        server.start();
        bool rejected = false;
        try {
            TcpServer duplicate(loop, "127.0.0.1", server.port());
            duplicate.start();
        } catch (const std::system_error &) {
            rejected = true;
        }
        check(rejected, "bind conflict reported");
    }
    check(fdCount() == before, "failed bind cleanup");
}
void descriptorExhaustion() {
    // 独立子进程降低 fd 上限，不能影响测试进程或开发容器的资源限制。
    int ends[2];
    check(::pipe(ends) == 0, "test pipe");
    UniqueFd readEnd(ends[0]), writeEnd(ends[1]);
    const pid_t child = ::fork();
    check(child >= 0, "fork test");
    if (child == 0) {
        readEnd.reset();
        const auto run = [&]() {
            try {
                const rlimit limit{64, 64};
                check(::setrlimit(RLIMIT_NOFILE, &limit) == 0, "setrlimit");
                EventLoop loop;
                TcpServer server(loop, "127.0.0.1", 0);
                bool notified = false;
                server.setErrorCallback([&](int error) {
                    notified = error == EMFILE;
                    server.stop();
                    loop.quit();
                });
                server.start();
                const auto port = server.port();
                std::vector<UniqueFd> occupied;
                while (true) {
                    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
                    if (fd.get() < 0)
                        break;
                    occupied.push_back(std::move(fd));
                }
                check(::write(writeEnd.get(), &port, sizeof(port)) == sizeof(port),
                      "publish child port");
                loop.loop();
                return notified ? 0 : 1;
            } catch (...) {
                return 2;
            }
        };
        ::_exit(run());
    }
    writeEnd.reset();
    std::uint16_t port = 0;
    const auto count = ::read(readEnd.get(), &port, sizeof(port));
    bool connected = false;
    if (count == sizeof(port)) {
        try {
            auto fd = connectTo(port);
            connected = true;
        } catch (...) {
        }
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child, "wait child");
    check(connected && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fd exhaustion notifies and stops without spinning");
}
} // namespace
int main() {
    int failures = 0;
    const auto run = [&](const char *name, void (*test)()) {
        try {
            test();
            std::printf("PASS %s\n", name);
        } catch (const std::exception &error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
        }
    };
    run("bufferAndFdOwnership", bufferAndFdOwnership);
    run("crossThreadWakeup", crossThreadWakeup);
    run("fragmentedAndCombinedMessages", fragmentedAndCombinedMessages);
    run("concurrentClientsAndFdReuse", concurrentClientsAndFdReuse);
    run("partialWriteAndPeerHalfClose", partialWriteAndPeerHalfClose);
    run("gracefulShutdownAndReset", gracefulShutdownAndReset);
    run("crossThreadSendAndForceClose", crossThreadSendAndForceClose);
    run("stopInsideAcceptCallback", stopInsideAcceptCallback);
    run("stopInsideMessageCallback", stopInsideMessageCallback);
    run("bufferLimitsAndBindFailure", bufferLimitsAndBindFailure);
    run("descriptorExhaustion", descriptorExhaustion);
    return failures == 0 ? 0 : 1;
}
