# Linux 服务端开发环境；业务服务将逐步写入挂载的 /workspace。
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
# 可通过 --build-arg 修改镜像源；APT 仍使用 Ubuntu 自带的签名密钥验证。
ARG UBUNTU_MIRROR=http://mirrors.ustc.edu.cn/ubuntu
RUN sed -i -E "s#http://(archive|security).ubuntu.com/ubuntu/?#${UBUNTU_MIRROR}/#g" /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
    openssh-server ca-certificates curl git build-essential cmake ninja-build \
    gdb pkg-config python3 libssl-dev clang-format iproute2 \
    && rm -rf /var/lib/apt/lists/*
COPY sshd_config /etc/ssh/sshd_config
COPY start.sh /usr/local/bin/obs-start
# Windows 文件系统上的脚本可能带 CRLF，镜像内统一为 LF。
RUN sed -i 's/\r$//' /usr/local/bin/obs-start && chmod 755 /usr/local/bin/obs-start \
    && mkdir -p /run/sshd /etc/ssh/keys /workspace
WORKDIR /workspace
USER root
EXPOSE 22 8080 8081 8082 9000 1935 1985 8088
ENTRYPOINT ["/usr/local/bin/obs-start"]
