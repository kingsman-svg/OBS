# HTTP 登录接口契约 v1

这是 CSN 首期新接口，不兼容 yzj 现有 TCP 登录数据包。当前客户端已实现，实际认证服务待下一步实现。

## 请求

`POST /auth/login`，`Content-Type: application/json`。

```json
{"account":"alice","password":"example-password"}
```

账号为非空字符串，客户端去除首尾空白；密码为非空字符串，保持原样。账号查找、密码校验和会话签发由服务端完成。
本机联调默认地址为 `http://127.0.0.1:8080/auth/login`，界面可填写完整接口地址；真实部署使用 HTTPS。
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
客户端将 token 与过期时间仅保存在内存；本轮不实现自动续期、持久化或后续受保护 API。

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
当前“退出登录”只清除客户端内存中的会话；服务端会话撤销接口将在实际认证服务阶段定义和实现。

## 实际认证服务下一步

确定账号存储、密码验证与会话生命周期，然后实现上述接口，使用实际成功/失败账号联调。
本轮 C++ 测试服务只有可控响应，用于验证网络与 MVC 行为，没有实际账号认证逻辑。
