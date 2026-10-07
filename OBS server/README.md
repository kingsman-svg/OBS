# OBS Linux 开发容器

本目录挂载到容器 `/workspace`，整个 CourseStudioNext 仓库另挂载到 `/repo`（包含 .git）。Windows 本地修改与 Linux 内修改直接同步，后续服务端代码集中在本目录的 code 中。

## VS Code 连接

1. 在 Windows VS Code 安装微软的 **Remote - SSH** 扩展（`ms-vscode-remote.remote-ssh`）。
2. 按 `Ctrl+Shift+P`，选择 `Remote-SSH: Connect to Host...`，选择 `OBS`。
3. 首次连接确认 SSH 主机指纹，输入 root 用户密码。
4. 远程窗口中选择“打开文件夹”，打开 `/repo`，源代码管理即可显示整个仓库及提交历史。

也可以直接打开本目录的 `OBS.code-workspace`；它指向 `OBS` 主机的 `/repo`，CMake 仍在 /workspace/code 构建，避免改变已有构建缓存路径。用户名 root，初始开发密码 root。密码不保存到 SSH 配置，连接时输入。

Windows SSH 主机配置位于 `%USERPROFILE%\.ssh\config`，本目录的 `ssh_config` 是对应配置备份。若换电脑，把该文件的 Host OBS 配置块添加到本机 SSH 配置。终端连接：

```powershell
ssh OBS
```

## 远程 Git 与 GitHub

容器和 Windows 共用同一份仓库及历史，不需要在 code 目录再次 git init。远程终端：

```bash
cd /repo
git status
git log --oneline --graph --all
git show --stat HEAD
```

提交前先查看差异，按文件暂存，避免把无关修改一并提交：

```bash
git diff
git add "OBS server/code/net/具体文件.cpp"
git commit -m "说明本次修改"
```

如果需要设置提交署名，在 /repo 中执行 `git config user.name "你的署名"` 和 `git config user.email "你的 GitHub 验证邮箱或 noreply 邮箱"`。这两个值影响后续提交作者，不代表 GitHub 登录。

镜像已安装 gh。GitHub 首次授权在远程终端执行：

```bash
gh auth login --hostname github.com --git-protocol https --web
gh auth setup-git
```

按终端提示在 Windows 浏览器打开 GitHub 授权页面并输入一次性代码。无需把令牌发到聊天中，root/root 是容器 SSH 登录，与 GitHub 授权无关。授权配置保存在持久化的 /root 卷，不放入仓库。

GitHub 地址确认后配置 origin；已有 origin 时先查看 `git remote -v`，避免覆盖不同项目。首次推送用 `git push -u origin main`。若远端已有提交，先 fetch 并检查历史，再决定合并方式，不覆盖远端历史。

参考：[GitHub CLI 浏览器授权](https://cli.github.com/manual/gh_auth_login)、[Git 凭据设置](https://cli.github.com/manual/gh_auth_setup-git)。

## 端口约定

| 用途 | Windows 本机端口 | 容器端口 | 当前状态 |
| --- | --- | --- | --- |
| SSH / VS Code | 2222 | 22 | 容器启动时运行 |
| 登录节点调度 | 8080 | 8080 | code/run_services.py 启动 |
| HTTP 登录节点 1 | 8081 | 8081 | 同上 |
| HTTP 登录节点 2 | 8082 | 8082 | 同上 |
| TCP JSON 信令 | 9000 | 9000 | 同上 |
| RTMP | 1935 | 1935 | 预留，媒体服务后续部署 |
| 媒体管理 API | 1985 | 1985 | 预留 |
| 点播 / HLS / HTTP-FLV | 8088 | 8088 | 预留 |

所有宿主端口绑定 `127.0.0.1`；Windows 客户端使用该地址。后续容器内业务进程须监听 `0.0.0.0` 对应端口，才能从宿主机访问。
容器入口自动启动 SSH；业务服务另用开发启动器启动，见 [完整说明](../docs/server-services.md)。媒体服务选型和部署另一步完成。若媒体服务独立部署到其他容器，需调整这几个预留映射，避免重复占用宿主端口。

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

Ubuntu 24.04，提供 GCC/G++、CMake、Ninja、GDB、Git、Python3、OpenSSL、nlohmann JSON 开发头文件和 clang-format。
已在 [code](code/README.md) 实现公共网络库、定时器、协议层及三个业务服务器。远程终端中完成构建后：

```bash
cd /workspace/code
python3 run_services.py start
python3 run_services.py status
python3 run_services.py stop
```

启动器创建两个登录节点、一个调度进程和一个信令进程。构建、协议与功能边界见 [服务端说明](../docs/server-services.md)。

## 当前验证结果

2026-10-07：OBS 已运行且健康检查通过。已验证 root 密码 SSH 认证、Windows 2222 端口的 SSH 握手、挂载目录写入同步、C++17 编译及 epoll 调用，全部端口映射与上表一致。
Remote - SSH 插件由用户安装，用户已确认连接成功。定时器与三个业务服务器的最新构建、测试及 Windows 端口链路验证见 [服务端记录](../docs/server-services.md#验证)。

参考：[Docker 目录挂载](https://docs.docker.com/engine/storage/bind-mounts/)、[端口映射](https://docs.docker.com/engine/network/port-publishing/)、[VS Code Remote - SSH](https://code.visualstudio.com/docs/remote/ssh)。
