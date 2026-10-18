#include "Channel.h"
#include "MediaGateway.h"
#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/signalfd.h>
extern "C" {
#include <libavutil/log.h>
}

int main(int argc, char **argv) {
    try {
        // 1. 在任何工作线程启动前屏蔽终止信号，随后由 loop 的 signalfd 统一处理。
        sigset_t signals;
        ::sigemptyset(&signals);
        ::sigaddset(&signals, SIGINT);
        ::sigaddset(&signals, SIGTERM);
        if (::sigprocmask(SIG_BLOCK, &signals, nullptr) < 0)
            throw std::runtime_error("signal mask");
        std::string configPath = "media/config.json";
        if (argc == 3 && std::string(argv[1]) == "--config")
            configPath = argv[2];
        else if (argc != 1)
            throw std::invalid_argument("use --config path");
        std::ifstream file(configPath);
        const auto json = nlohmann::json::parse(file);
        obs::media::GatewayConfig config;
        const auto port = [&](const char *name) {
            const auto n = json.at(name).get<int>();
            if (n < 1 || n > 65535)
                throw std::invalid_argument("port range");
            return static_cast<std::uint16_t>(n);
        };
        config.httpPort = port("httpPort");
        config.firstPort = port("firstRtcPort");
        config.lastPort = port("lastRtcPort");
        config.bindAddress = json.at("bindAddress").get<std::string>();
        config.publicAddress = json.at("publicAddress").get<std::string>();
        config.rtmpBase = json.at("rtmpBase").get<std::string>();
        config.maxViewers = json.at("maxViewers").get<unsigned>();
        in_addr address{};
        if (::inet_pton(AF_INET, config.publicAddress.c_str(), &address) != 1 ||
            config.firstPort > config.lastPort || config.maxViewers < 1 || config.maxViewers > 8 ||
            config.maxViewers > unsigned(config.lastPort - config.firstPort + 1))
            throw std::invalid_argument("gateway limits");
        std::ifstream html(json.at("page").get<std::string>());
        if (!html)
            throw std::runtime_error("player page unavailable");
        config.page.assign(std::istreambuf_iterator<char>(html), {});
        // 2. 复用现有签名密钥，容器卷只读挂载；密码和令牌不出现在命令行或日志。
        const char *secretFile = std::getenv("OBS_TOKEN_SECRET_FILE");
        std::ifstream key(secretFile ? secretFile : "/auth/.config/obs/token-secret");
        std::string secret;
        key >> secret;
        if (secret.size() < 32)
            throw std::runtime_error("shared signing key unavailable");
        const char *password = std::getenv("OBS_LOGIN_PASSWORD");
        obs::services::AuthService auth(secret, password ? password : "root");
        rtc::InitLogger(rtc::LogLevel::None); // SDP/ICE 内含凭据，不启用上游详细日志。
        av_log_set_level(AV_LOG_QUIET); // 上游库错误可能附带完整媒体地址，业务状态单独报告。
        obs::net::EventLoop loop;
        obs::net::UniqueFd signalFd(::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
        if (signalFd.get() < 0)
            throw std::runtime_error("signalfd");
        auto channel = std::make_shared<obs::net::Channel>(loop, signalFd.get());
        channel->setReadCallback([&] {
            signalfd_siginfo info{};
            while (::read(signalFd.get(), &info, sizeof(info)) > 0) {
            }
            loop.quit();
        });
        channel->enableReading();
        // 3. main 创建/启动网关，再进入 loop；构造函数只持有资源、绑定回调。
        {
            obs::media::MediaGateway gateway(loop, auth, std::move(config));
            gateway.start();
            std::cout << "Media gateway ready (H264 + optional AAC/Opus audio).\n" << std::flush;
            loop.loop();
            gateway.stop(); // 4. 先停止入口，析构等待读线程；最后销毁信号通道和 loop。
        }
        channel->disableAll();
        return 0;
    } catch (const std::exception &) {
        std::cerr
            << "Media gateway failed; check configuration, shared key and port availability.\n";
        return 1;
    }
}
