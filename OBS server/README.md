# OBS Linux 开发容器

本目录挂载到容器 `/workspace`。Windows 本地修改与 Linux 内修改直接同步，后续服务端代码集中在这里。

## VS Code 连接

1. 在 Windows VS Code 安装微软的 **Remote - SSH** 扩展（`ms-vscode-remote.remote-ssh`）。
2. 按 `Ctrl+Shift+P`，选择 `Remote-SSH: Connect to Host...`，选择 `OBS`。
3. 首次连接确认 SSH 主机指纹，输入 root 用户密码。
4. 远程窗口中选择“打开文件夹”，打开 `/workspace`。

也可以直接打开本目录的 `OBS.code-workspace`；它指向 `OBS` 主机的 `/workspace`。用户名 root，初始开发密码 root。密码不保存到 SSH 配置，连接时输入。

Windows SSH 主机配置位于 `%USERPROFILE%\.ssh\config`，本目录的 `ssh_config` 是对应配置备份。若换电脑，把该文件的 Host OBS 配置块添加到本机 SSH 配置。终端连接：

```powershell
ssh OBS
```

## 端口约定

| 用途 | Windows 本机端口 | 容器端口 | 当前状态 |
| --- | --- | --- | --- |
| SSH / VS Code | 2222 | 22 | 容器启动时运行 |
| 登录节点调度 | 8080 | 8080 | 预留 |
| HTTP 登录节点 1 | 8081 | 8081 | 预留 |
| HTTP 登录节点 2 | 8082 | 8082 | 预留 |
| TCP JSON 信令 | 9000 | 9000 | 预留 |
| RTMP | 1935 | 1935 | 预留，媒体服务后续部署 |
| 媒体管理 API | 1985 | 1985 | 预留 |
| 点播 / HLS / HTTP-FLV | 8088 | 8088 | 预留 |

所有宿主端口绑定 `127.0.0.1`；Windows 客户端使用该地址。后续容器内业务进程须监听 `0.0.0.0` 对应端口，才能从宿主机访问。
映射不代表业务已经实现，目前只启动 SSH；媒体服务选型和部署另一步完成。若媒体服务独立部署到其他容器，需调整这几个预留映射，避免重复占用宿主端口。

## 启停与重建

在本目录执行（Docker 命令需要在 PATH 中）：

```powershell
docker compose up -d --build
docker compose ps
docker compose logs --tail 50
docker compose stop
docker compose start
```

Docker Desktop 若未加入 PATH，本机程序路径为：
`C:\Users\Admin\AppData\Local\Programs\DockerDesktop\resources\bin`。
在 PowerShell 临时补充：

```powershell
$env:PATH = "$env:LOCALAPPDATA\Programs\DockerDesktop\resources\bin;$env:PATH"
```

`docker compose down` 删除容器但保留源码和命名卷；重新 up 会恢复挂载。root 的 VS Code Server 和 SSH 主机密钥放在命名卷，重建容器后保留。

`.env` 已被 Git 和镜像构建上下文忽略。新检出时先创建该文件并写入 `OBS_ROOT_PASSWORD=root`，再执行 up。修改密码后执行 `docker compose up -d --force-recreate`。

## 工具与后续工作

Ubuntu 24.04，提供 GCC/G++、CMake、Ninja、GDB、Git、Python3、OpenSSL 开发头文件和 clang-format。
已在 [code](code/README.md) 实现第一步公共网络库 `obs_net`：单 Reactor、TCP 接入、非阻塞收发、任务唤醒和资源清理。下一步补定时器和协议层，再迁移登录服务、增加调度和信令。

## 当前验证结果

2026-10-07：OBS 已运行且健康检查通过。已验证 root 密码 SSH 认证、Windows 2222 端口的 SSH 握手、挂载目录写入同步、C++17 编译及 epoll 调用，全部端口映射与上表一致。
Remote - SSH 插件由用户安装，VS Code 首次连接和 VS Code Server 下载尚未验证。

参考：[Docker 目录挂载](https://docs.docker.com/engine/storage/bind-mounts/)、[端口映射](https://docs.docker.com/engine/network/port-publishing/)、[VS Code Remote - SSH](https://code.visualstudio.com/docs/remote/ssh)。
