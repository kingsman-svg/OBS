# HTTP 登录接口契约 v1

这是客户端使用的新 HTTP 接口，不兼容 yzj 现有 TCP 登录数据包。两端业务程序默认先访问 8080 调度，再连接独立 Linux LoginServer（8081/8082）。Qt LocalAuthServer 仅供原登录测试使用。开发账号默认 root / root；完整生产账号系统仍待实现。

## 请求

`POST /auth/login`，`Content-Type: application/json`。

```json
{"account":"alice","password":"example-password"}
```

账号为非空字符串，客户端去除首尾空白；密码为非空字符串，保持原样。账号查找、密码校验和会话签发由服务端完成。
独立服务联调地址为 `http://127.0.0.1:8081/auth/login`；8080 是调度服务，先 GET `/login/server` 获得登录节点地址。界面可手动填写完整登录接口地址；真实部署使用 HTTPS。
客户端不自动跟随重定向；不记录密码、认证请求正文或会话令牌。

## 成功响应

`200 OK`：

```json
{
  "accessToken":"opaque-session-token",
  "expiresIn":3600,
  "user":{"id":"u1","displayName":"Alice"}
}
```

- `accessToken`：非空字符串，客户端视为不透明凭据。
- `expiresIn`：正整数，单位秒；本版本限制为 C++ int 可表示范围。
- `user.id`、`user.displayName`：非空字符串。

缺少有效字段时，客户端保留未登录状态，即使 HTTP 返回 2xx。
客户端将 token 与过期时间仅保存在内存，不实现自动续期或持久化。独立服务令牌用于 TCP 信令认证，两个工作台已经接入；本地到期定时器会清除会话并断开信令。

## 失败响应

```json
{"error":{"code":"INVALID_CREDENTIALS","message":"账号或密码错误"}}
```

| 状态 | 含义 |
| --- | --- |
| 400 | 请求字段不合法 |
| 401 | 账号或密码错误 |
| 429 | 请求过于频繁 |
| 500 | 服务内部错误 |

客户端当前显示 HTTP 状态与 `error.message`；网络失败、超时及无效 JSON 单独报告。
请求默认 5 秒超时。请求 JSON 最大 64 KiB，响应最大 1 MiB。

## 取消和退出

取消中止当前请求并忽略其后续回调。HTTP abort 不保证撤销服务端已经完成的工作。
“退出登录”清除客户端内存会话并关闭当前 TCP 信令连接，由服务器清理成员或关闭主播房间；尚不提供中心令牌撤销接口。

## 实际认证服务下一步

独立 Linux 登录服务已执行开发账号校验、限流与签名令牌签发，信令会校验签名和到期时间。令牌暂不提供中心撤销表，退出仍只清除客户端会话。生产阶段再接入账号存储、安全密码哈希、HTTPS 与撤销机制。
启动、节点调度、信令协议与验证见 [Linux 服务端说明](server-services.md)。
