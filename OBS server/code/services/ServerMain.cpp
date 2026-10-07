#include "LoginServer.h"
#include "SchedulerServer.h"
#include "SignalServer.h"
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/signalfd.h>

namespace {
std::uint16_t portNumber(const std::string &value) {
    unsigned int port = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port == 0 ||
        port > 65535)
        throw std::invalid_argument("invalid port");
    return static_cast<std::uint16_t>(port);
}
int bounded(const nlohmann::json &config, const char *field, int minimum, int maximum) {
    if (!config.at(field).is_number_integer())
        throw std::invalid_argument("integer configuration required");
    const auto value = config.at(field).get<std::int64_t>();
    if (value < minimum || value > maximum)
        throw std::invalid_argument("configuration out of range");
    return static_cast<int>(value);
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::string configPath = "services/config.json";
        std::uint16_t overridePort = 0;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing option value");
            const std::string option = argv[i];
            if (option == "--config")
                configPath = argv[i + 1];
            else if (option == "--port")
                overridePort = portNumber(argv[i + 1]);
            else
                throw std::invalid_argument("unknown option");
        }
        std::ifstream file(configPath);
        if (!file)
            throw std::runtime_error("configuration file unavailable");
        const auto config = nlohmann::json::parse(file);
        const auto address = config.at("bindAddress").get<std::string>();
        const std::string role = OBS_SERVER_ROLE;

        // 用 signalfd 将终止信号纳入 epoll；异步信号处理器中不调用 STL 或业务代码。
        sigset_t signals;
        ::sigemptyset(&signals);
        ::sigaddset(&signals, SIGINT);
        ::sigaddset(&signals, SIGTERM);
        if (::sigprocmask(SIG_BLOCK, &signals, nullptr) < 0)
            throw std::runtime_error("signal mask failed");
        obs::net::EventLoop loop;
        obs::net::UniqueFd signalFd(::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
        if (signalFd.get() < 0)
            throw std::runtime_error("signalfd failed");
        auto signalChannel = std::make_shared<obs::net::Channel>(loop, signalFd.get());
        signalChannel->setReadCallback([&] {
            signalfd_siginfo info{};
            while (::read(signalFd.get(), &info, sizeof(info)) > 0) {
            }
            loop.quit();
        });
        signalChannel->enableReading();

        std::unique_ptr<obs::services::AuthService> auth;
        std::unique_ptr<obs::services::LoginServer> login;
        std::unique_ptr<obs::services::SchedulerServer> scheduler;
        std::unique_ptr<obs::services::SignalServer> signal;
        std::uint16_t port = overridePort;
        if (role != "scheduler") {
            const char *secret = std::getenv("OBS_TOKEN_SECRET");
            const char *password = std::getenv("OBS_LOGIN_PASSWORD");
            if (!secret)
                throw std::runtime_error("OBS_TOKEN_SECRET must be set");
            auth = std::make_unique<obs::services::AuthService>(
                secret, password ? password : "root", bounded(config, "tokenTtlSeconds", 1, 86400));
        }
        if (role == "login") {
            if (!port)
                port = static_cast<std::uint16_t>(bounded(config, "loginPort", 1, 65535));
            login = std::make_unique<obs::services::LoginServer>(loop, address, port, *auth);
            login->start();
        } else if (role == "scheduler") {
            if (!port)
                port = static_cast<std::uint16_t>(bounded(config, "schedulerPort", 1, 65535));
            std::vector<obs::services::LoginNode> nodes;
            for (const auto &item : config.at("loginNodes")) {
                obs::services::LoginNode node;
                node.id = item.at("id").get<std::string>();
                node.address = item.at("address").get<std::string>();
                node.publicUrl = item.at("publicUrl").get<std::string>();
                node.port = static_cast<std::uint16_t>(bounded(item, "port", 1, 65535));
                if (node.id.empty() || node.publicUrl.rfind("http://", 0) != 0)
                    throw std::invalid_argument("invalid login node");
                nodes.push_back(std::move(node));
            }
            if (nodes.size() > 32)
                throw std::invalid_argument("too many login nodes");
            scheduler = std::make_unique<obs::services::SchedulerServer>(loop, address, port,
                                                                         std::move(nodes));
            scheduler->start();
        } else {
            if (!port)
                port = static_cast<std::uint16_t>(bounded(config, "signalPort", 1, 65535));
            signal = std::make_unique<obs::services::SignalServer>(
                loop, address, port, *auth, config.at("rtmpBase").get<std::string>(),
                config.at("playbackBase").get<std::string>(),
                bounded(config, "signalIdleSeconds", 1, 3600),
                bounded(config, "signalAuthSeconds", 1, 60));
            signal->start();
        }
        std::cout << role << " listening on " << address << ':' << port << std::endl;
        loop.loop();
        // 先销毁业务（取消定时器、停止连接），再撤销信号通道，最后销毁 EventLoop。
        signal.reset();
        scheduler.reset();
        login.reset();
        signalChannel->disableAll();
        return 0;
    } catch (const std::exception &) {
        // 启动错误不附带 JSON 正文/环境变量；命令和配置项范围见 README。
        std::cerr << "Server startup/runtime failed; check configuration, secret and port "
                     "availability.\n";
        return 1;
    }
}
