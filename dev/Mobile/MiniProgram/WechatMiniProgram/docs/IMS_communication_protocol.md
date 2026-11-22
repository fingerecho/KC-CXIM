# KC-CXIM IMS 通信协议文档

> 本文根据 `dev/IMS/KC-CXIM-Svc/basic_service_20261107/readme.md` 与
> `step1_tcp_echo/` 下的服务端实现整理，描述客户端与服务端之间的完整通信协议。
> 文档以**当前代码行为**为准；当 README 与代码不一致时，以代码为准并在文中注明。

- 服务端：C++17 + Boost.Asio/Beast + MariaDB
- HTTP 短连接：注册、登录、用户搜索、好友/群关系管理等“写事实”接口
- TCP 长连接：登录鉴权、心跳、聊天消息、历史消息、回执、实时事件推送
- 设计原则：**HTTP 负责写事实，TCP 负责推事件**

---

## 1. 总览

### 1.1 两个通信通道

| 通道 | 默认端口 | 用途 | 连接特性 |
| --- | --- | --- | --- |
| HTTP | `8080`（`HTTP_PORT`） | 注册、登录、刷新 token、好友/群操作、列表查询 | 支持 keep-alive；UTF-8 JSON |
| TCP | `8888`（`TCP_PORT`） | 登录鉴权、心跳、消息收发、历史、回执、事件推送 | 长连接；4 字节长度头分帧 |

### 1.2 核心约定

1. **身份只信 JWT**：HTTP 从 `Authorization: Bearer <JWT>` 取身份；TCP 登录用 `type=100` 消息里的 JWT 验签，`sub` 即真实 `userId`，服务端不信任客户端自报的 `from`。
2. **access / refresh 分离**：`access` 用于 HTTP Bearer 与 TCP 登录；`refresh` 只能用于 `POST /api/token/refresh`，不可当访问凭证使用。
3. **事件不承载事实**：TCP 事件只是“事实已变更”的通知。好友列表、群成员等事件不持久化，客户端重连后应主动拉取全量数据对齐。
4. **消息幂等**：单聊/群聊发送使用客户端 `msg_id`（非 0 时）作为 `client_msg_id` 去重；重试必须保持同一 `msg_id`。
5. **过载要退避**：收到 `429/503`、TCP `"server busy"` 时按 `Retry-After` / 抖动退避重试，不应视为业务失败。

---

## 2. 通用约定

### 2.1 编码与类型

| 项目 | 约定 |
| --- | --- |
| 字符编码 | UTF-8 |
| HTTP `Content-Type` | `application/json` |
| HTTP 响应 `Content-Type` | `application/json; charset=utf-8` |
| 时间戳 | `uint64`，Unix 毫秒 |
| ID | `uint64` 数字；`userId`、`groupId`、`conversationId`、`msg_id`、`requestId` 等 |
| 长度限制 | 除特别说明外，文中的“长度”均按**字节**计算 |

### 2.2 HTTP 统一响应

成功：

```json
{ "code": 0, "message": "ok", "data": { } }
```

失败：

```json
{ "code": 1, "message": "错误原因" }
```

- `code=0` 表示业务成功；`code=1` 表示业务失败。
- HTTP 状态码同时表达语义（400/401/404/405/413/429/503）。
- 部分成功接口没有 `data` 字段，例如修改备注、删除好友、拉黑等。

### 2.3 HTTP 状态码

| 状态码 | 含义 | 客户端处理 |
| --- | --- | --- |
| `200` | 成功 | 正常处理 `data` |
| `400` | 参数错误 / 业务规则拒绝 | 修正请求，不要盲目重试 |
| `401` | 未带 token、token 无效/过期、账号密码错误 | 先尝试 refresh，必要时重新登录 |
| `404` | 路径不存在 | 检查接口路径 |
| `405` | HTTP 方法不允许 | 检查请求方法 |
| `413` | 请求体超过 `HTTP_BODY_MAX`（默认 64KB） | 修正请求；服务端随后关闭连接 |
| `429` | 认证类接口触发限流 | 按 `Retry-After` 退避，不计入业务失败率 |
| `503` | 服务端过载（在途请求/任务队列超阈值） | 按 `Retry-After` 退避后重连重试 |

### 2.4 HTTP 鉴权

除以下三个公开接口外，其余接口都必须携带：

```http
Authorization: Bearer <access_token>
```

公开接口：

- `POST /api/register`
- `POST /api/login`
- `POST /api/token/refresh`

鉴权失败返回 `401`：

| `message` | 触发条件 |
| --- | --- |
| `missing authorization` | 未带 `Authorization` 头 |
| `expected Bearer token` | 头存在但不是 `Bearer ` 前缀 |
| `invalid or expired token` | 验签失败、过期、issuer 不匹配、typ 不是 access |

### 2.5 HTTP 连接复用与限制

- 客户端使用 HTTP/1.1 且未声明 `Connection: close` 时，服务端复用同一连接继续读下一个请求。
- 空闲 `60s` 无新请求关闭连接。
- 单连接最多处理 `256` 个请求后关闭。
- 显式发送 `Connection: close` 时，一问一答即关。
- 请求体默认上限 `64KB`；超过返回 `413` 并关闭连接。
- 在途 HTTP 响应数或 SQL 队列深度超过阈值时，新请求返回 `503 + Retry-After`。

### 2.6 过载保护与限流（以代码默认值为准）

| 项目 | 代码默认值 | 说明 |
| --- | --- | --- |
| `HTTP_MAX_INFLIGHT` | `512` | 同时在途 HTTP 响应上限 |
| `HTTP_BODY_MAX` | `65536` 字节 | HTTP 请求体上限 |
| `AUTH_RATE_PER_SEC` | `20` | 注册/登录/刷新全局令牌补充速率（每秒） |
| `AUTH_RATE_BURST` | `40` | 全局令牌桶容量 |
| `AUTH_IP_PER_SEC` | `30` | 单 IP 每秒令牌 |
| 单 IP 突发容量 | `2 × AUTH_IP_PER_SEC`（默认 60） | 代码中 `ipBurst = ipPerSec * 2` |
| `TCP_MAX_SESSIONS` | `2000` | TCP 并发连接上限，超过立即关闭新连接 |
| `TCP_MAX_OUTSTANDING` | `64` | 单条 TCP 连接未完成的 DB 任务上限 |
| `IDLE_TIMEOUT_SEC` | `60`（钳制到 15~3600） | TCP 空闲踢人秒数 |

> 注：README 参数字段中 `AUTH_IP_PER_SEC` 出现了两行（30 与 5）。当前代码默认值是 **30**，
> 且单 IP 桶容量为 `30 × 2 = 60`。README 正文也明确“每 IP 阈值必须宽于全局阈值”。

过载时的表现：

| 触发 | HTTP 表现 | TCP 表现 | 客户端策略 |
| --- | --- | --- | --- |
| 认证接口限流 | `429 + Retry-After` | — | 按 `Retry-After` 退避 |
| HTTP 在途/队列过载 | `503 + Retry-After`，并关闭连接 | — | 退避后重连 |
| 热路径 DB 队列满 | `503`，`message="server busy"` | `type=102`，`content="server busy"` | 降速重试 |
| TCP 连接数超限 | — | 新连接被立即关闭 | 退避重连 |
| 单连接在途任务超限 | — | `type=102`，`content="server busy"` | 用同一 `msg_id` 重试 |
| 单连接写队列超限 | — | 服务端主动断开 | 重连后靠补推/历史对齐 |

---

## 3. 鉴权协议

### 3.1 JWT 结构

服务端使用 **HS256** 签发无状态 JWT。

Header：

```json
{ "alg": "HS256", "typ": "JWT" }
```

Payload 字段：

| claim | 类型 | 说明 |
| --- | --- | --- |
| `iss` | string | 签发者，来自 `JWTConfig.json` 的 `jwt_issuer`，验签时会比对 |
| `sub` | string | 用户 ID 的十进制字符串，是服务端唯一认可的身份来源 |
| `username` | string | 用户名，便于日志/展示 |
| `typ` | string | `access` 或 `refresh` |
| `jti` | string | 32 位十六进制随机串，仅用于区分 token，暂未做撤销表 |
| `iat` | number | 签发时间（秒） |
| `exp` | number | 过期时间（秒） |

`JWTConfig.json` 示例：

```json
{
  "jwt_secret": "a3f8c2e1b94d7f60...（64位十六进制字符串）",
  "jwt_issuer": "your-im-service",
  "expire_seconds": 86400,
  "refresh_expire_seconds": 2592000
}
```

### 3.2 access 与 refresh 的区别

| 类型 | `typ` | 默认有效期 | 可用于 | 不可用于 |
| --- | --- | --- | --- | --- |
| access | `access` | `86400s`（1 天） | HTTP Bearer、TCP `type=100` 登录 | 刷新接口 |
| refresh | `refresh` | `2592000s`（30 天） | `POST /api/token/refresh` | HTTP Bearer、TCP 登录 |

兼容规则：**没有 `typ` claim 的旧 token 只按 access 处理**；refresh 一定带 `typ=refresh`。

安全边界：refresh token 是**无状态** JWT，服务端没有撤销表。修改密码/封号无法让已签发的 refresh 立即失效；每次刷新会轮换签发新 refresh，但旧 refresh 在自己的 `exp` 前仍可使用。

### 3.3 断线重连推荐流程

1. 保存 `token`（access）与 `refreshToken`。
2. access 到期前，或 TCP 登录收到 `invalid or expired token` 时，先调用 `POST /api/token/refresh`。
3. 只有 refresh 也失效（过期/非法）时，才回落到 `POST /api/login` 用密码重新登录。
4. 重连必须带**抖动退避**，例如 `min(60s, 1s × 2^n) + rand(0,1s)`。
5. 收到 `429/503` 时按 `Retry-After` 等待，不要立即重试。

---

## 4. HTTP API 参考

### 4.0 路由总表

| 方法 | 路径 | 鉴权 | 说明 |
| --- | --- | --- | --- |
| POST | `/api/register` | 否 | 注册 |
| POST | `/api/login` | 否 | 登录，签发 access + refresh |
| POST | `/api/token/refresh` | 否 | 用 refresh 换新 token |
| GET | `/api/users/search` | 是 | 搜索用户 |
| GET | `/api/friends` | 是 | 好友列表 |
| GET | `/api/friends/requests` | 是 | 好友申请列表 |
| POST | `/api/friends/requests` | 是 | 发送好友申请 |
| POST | `/api/friends/requests/batch` | 是 | 批量发送好友申请 |
| POST | `/api/friends/requests/{requestId}/accept` | 是 | 通过好友申请 |
| POST | `/api/friends/requests/{requestId}/reject` | 是 | 拒绝好友申请 |
| PATCH | `/api/friends/{friendId}` | 是 | 修改好友备注 |
| DELETE | `/api/friends/{friendId}` | 是 | 删除好友 |
| POST | `/api/friends/{friendId}/block` | 是 | 拉黑 |
| POST | `/api/friends/{friendId}/unblock` | 是 | 取消拉黑 |
| POST | `/api/friends/batch-delete` | 是 | 批量删除好友 |
| GET | `/api/groups` | 是 | 我的群列表 |
| POST | `/api/groups` | 是 | 建群 |
| GET | `/api/groups/{groupId}` | 是 | 群详情 + 成员 |
| PATCH | `/api/groups/{groupId}` | 是 | 修改群资料 |
| POST | `/api/groups/{groupId}/members` | 是 | 邀请成员 |
| DELETE | `/api/groups/{groupId}/members/{userId}` | 是 | 移出成员 |
| POST | `/api/groups/{groupId}/admins` | 是 | 任命/取消管理员 |
| POST | `/api/groups/{groupId}/transfer` | 是 | 转让群主 |
| POST | `/api/groups/{groupId}/quit` | 是 | 退群 |
| POST | `/api/groups/{groupId}/dismiss` | 是 | 解散群 |
| POST | `/api/groups/requests` | 是 | 申请入群 |
| GET | `/api/groups/requests` | 是 | 入群申请列表 |
| POST | `/api/groups/requests/{requestId}/accept` | 是 | 通过入群申请 |
| POST | `/api/groups/requests/{requestId}/reject` | 是 | 拒绝入群申请 |
| POST | `/api/files/upload-token` | 是 | 签发上传凭证（七牛云直传，见 4.15） |
| POST | `/api/files/commit` | 是 | 直传完成后提交登记（存储端核验） |
| GET | `/api/files/{uuid}` | Bearer 或签名 | 下载换取入口，302 到短时效签名 URL |
| DELETE | `/api/files/{uuid}` | 是 | 删除文件（仅上传者） |
| POST | `/api/files/mock-put` | 是 | 仅 `QINIU_ENABLED=0`：模拟直传端点（真实模式 404） |
| GET | `/api/files/mock-object/{key}` | 签名 | 仅 `QINIU_ENABLED=0`：模拟私有下载端点 |

> 路由先匹配 `/api/groups/requests` 再匹配 `/api/groups/{id}`；路径中的 ID 必须是十进制数字且非 0；
> `/api/files/{uuid}` 的 uuid 必须是 36 位 v4 形状（`/api/files/commit` 等具体路径先于 uuid 通配匹配）。

---

### 4.1 注册

```http
POST /api/register
```

请求体：

```json
{ "username": "alice", "password": "secret123" }
```

约束：

- `username` 非空，长度 `<= 64`
- `password` 长度 `6 ~ 72`
- 用户名全局唯一

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": { "userId": 1001 }
}
```

常见失败：`missing username/password`、`invalid username`、`invalid password`、用户名已存在（数据库唯一键冲突）、`server busy`（503）。

---

### 4.2 登录

```http
POST /api/login
```

请求体：

```json
{ "username": "alice", "password": "secret123" }
```

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "userId": 1001,
    "username": "alice",
    "token": "eyJhbGciOiJIUzI1NiIs...",
    "refreshToken": "eyJhbGciOiJIUzI1NiIs...",
    "expiresIn": 86400,
    "refreshExpiresIn": 2592000,
    "tokenType": "Bearer"
  }
}
```

失败：账号/密码错误返回 `401`；`server busy` 返回 `503`。

---

### 4.3 刷新 access token

```http
POST /api/token/refresh
```

请求体：

```json
{ "refreshToken": "eyJhbGciOiJIUzI1NiIs..." }
```

成功响应：与登录同构，包含新的 `token`、轮换后的 `refreshToken`、`expiresIn`、`refreshExpiresIn`。

失败：

| 状态码 | `message` | 说明 |
| --- | --- | --- |
| 400 | `missing refreshToken` | 缺少字段 |
| 401 | `invalid or expired refresh token` | 签名错/过期/typ 不是 refresh |
| 429 | `too many auth requests` | 认证限流 |

服务端成本：一次 HMAC 验签 + 两次 HMAC 签发，**不查库、不做 bcrypt**。

---

### 4.4 搜索用户

```http
GET /api/users/search?keyword=<关键字>&limit=<条数>
```

需要鉴权。

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `keyword` | 是 | 匹配用户名、昵称，或精确用户 ID |
| `limit` | 否 | 默认 20，代码上限 50 |

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "users": [
      { "userId": 1002, "username": "bob", "nickname": "Bob", "avatarUrl": "" }
    ]
  }
}
```

说明：

- 只返回公开字段，不返回 `password_hash`、token。
- 只搜索 `status=1` 的正常用户，且排除自己。
- 用户名精确匹配排在前面。

失败：`missing keyword`、`server busy`。

---

### 4.5 好友列表

```http
GET /api/friends
```

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "friends": [
      {
        "userId": 1002,
        "username": "bob",
        "nickname": "Bob",
        "avatarUrl": "",
        "remark": "B-remark",
        "status": 1
      }
    ]
  }
}
```

字段：

| 字段 | 说明 |
| --- | --- |
| `userId` | 好友 userId |
| `username` / `nickname` / `avatarUrl` | 好友公开资料 |
| `remark` | 我给该好友设置的备注 |
| `status` | `1`=好友；`2`=拉黑（当前列表只返回 `1`） |

---

### 4.6 好友申请列表

```http
GET /api/friends/requests?direction=incoming|outgoing|all
```

| direction | 说明 |
| --- | --- |
| `incoming` | 我收到的申请（默认） |
| `outgoing` | 我发出的申请 |
| `all` | 收发都返回 |

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "requests": [
      {
        "requestId": 10,
        "requesterId": 1001,
        "targetId": 1002,
        "requesterUsername": "alice",
        "requesterNickname": "Alice",
        "targetUsername": "bob",
        "targetNickname": "Bob",
        "message": "hello from A",
        "status": 0,
        "createdTs": 1760000000000
      }
    ]
  }
}
```

申请状态：

| status | 含义 |
| --- | --- |
| `0` | 待处理 |
| `1` | 已通过 |
| `2` | 已拒绝 |
| `3` | 已取消（保留） |

失败：`invalid direction`、`server busy`。

---

### 4.7 发送好友申请

```http
POST /api/friends/requests
```

请求体：

```json
{ "toUserId": 1002, "message": "你好，我是 Alice" }
```

- `message` 可选，最大 `255` 字节。
- 成功返回 `data.request`，字段同 4.6 单条申请。
- 若对方在线，实时收到 TCP `FriendRequest(130)` 事件；离线则在对方登录时补推。

业务校验（失败返回 `400`）：

- 不能加自己：`cannot add yourself`
- 目标用户必须存在且正常：`target user not found`
- 已是好友：`already friends`
- 被对方拉黑：`blocked by target user`
- 对方已向你发待处理申请：`target already sent you a pending request`
- 同方向已有待处理申请：`request already pending`
- 历史申请被拒绝/取消：重置为待处理，允许重新申请

---

### 4.8 批量发送好友申请

```http
POST /api/friends/requests/batch
```

请求体：

```json
{ "toUserIds": [1002, 1003, 1004], "message": "我是小李" }
```

- 单次最多 `200` 个目标。
- `message` 最大 `255` 字节；超长直接拒绝，不截断。
- 语义是**尽力而为**：不满足条件的目标进入 `skipped`，不会导致整批失败。

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "requests": [
      {
        "requestId": 88,
        "requesterId": 1001,
        "targetId": 1003,
        "targetUsername": "carol",
        "targetNickname": "Carol",
        "message": "我是小李",
        "status": 0,
        "createdAt": 1730000000000
      }
    ],
    "skipped": [
      { "toUserId": 1002, "reason": "already friends" },
      { "toUserId": 1004, "reason": "blocked by target user" }
    ]
  }
}
```

注意：批量接口成功项的字段是 `createdAt`；单条接口字段是 `createdTs`。

每个成功项都会实时推送 `FriendRequest(130)` 给被申请人，离线者登录补推。

---

### 4.9 通过 / 拒绝好友申请

```http
POST /api/friends/requests/{requestId}/accept
POST /api/friends/requests/{requestId}/reject
```

需要鉴权，且只能由被申请人本人操作。

成功响应：`data.request`，`status` 分别为 `1`（通过）或 `2`（拒绝）。

通过后的副作用：

- 数据库写入双向好友关系 `(A,B)` 与 `(B,A)`
- 申请人收到 TCP `FriendRequestResult(131)`，`event="accepted"`
- 双方在线时收到 `FriendListChanged(132)`，`action="added"`

拒绝后：申请人收到 `FriendRequestResult(131)`，`event="rejected"`。

失败：`request not found`、`request already handled`、`server busy`。

---

### 4.10 修改好友备注

```http
PATCH /api/friends/{friendId}
```

请求体：

```json
{ "remark": "B-remark" }
```

- `remark` 最大 `64` 字节。
- 备注是私有的，只影响自己的好友列表。
- 成功返回 `{ "code": 0, "message": "ok" }`。
- 成功后自己会收到 `FriendListChanged(132)`，`action="updated"`。

失败：`missing remark`、`remark too long`、`friend not found`。

---

### 4.11 删除好友

```http
DELETE /api/friends/{friendId}
```

- 同时删除双向关系，并清理双方待处理申请。
- 成功返回 `{ "code": 0, "message": "ok" }`。
- 双方在线时各收到 `FriendListChanged(132)`，`action="deleted"`。

---

### 4.12 批量删除好友

```http
POST /api/friends/batch-delete
```

请求体：

```json
{ "friendIds": [1002, 1003, 1004] }
```

- 单次最多 `200` 个。
- 一个事务内完成双向删除并清理双方待处理申请。
- 成功响应：

```json
{ "code": 0, "message": "ok", "data": { "requested": 3, "deleted": 3 } }
```

- `deleted` 表示真正从我的好友列表消失的人数，不是请求中 ID 的个数。
- 对被删除的每个好友，双方都会收到 `FriendListChanged(132)`，`action="deleted"`。

失败：`missing friendIds`、`friendIds is empty`、`too many friendIds`。

---

### 4.13 拉黑 / 取消拉黑

```http
POST /api/friends/{friendId}/block
POST /api/friends/{friendId}/unblock
```

成功返回：

```json
{ "code": 0, "message": "ok" }
```

拉黑后：

- 删除双方好友关系
- 写入 `(A,B,status=2)` 的单向拉黑行
- 清理双方好友申请
- 自己收到 `FriendListChanged(132)`，`action="blocked"`，`status=2`

取消拉黑：

- 只删除自己的拉黑记录，不自动恢复好友关系，需要重新申请
- 自己收到 `FriendListChanged(132)`，`action="unblocked"`

失败：`cannot block yourself`、`target user not found`。

---

### 4.14 群聊接口

角色定义：

| role | 含义 |
| --- | --- |
| `1` | 群主 |
| `2` | 管理员 |
| `3` | 普通成员 |

权限矩阵：

| 操作 | 群主(1) | 管理员(2) | 成员(3) |
| --- | --- | --- | --- |
| 发言 / 拉历史 / 回执 | ✓ | ✓ | ✓ |
| 改群资料 | ✓ | ✓ | ✗ |
| 邀请成员 | ✓ | ✓ | ✗ |
| 移出普通成员 | ✓ | ✓（仅移成员） | ✗ |
| 任命/取消管理员 | ✓ | ✗ | ✗ |
| 转让群主 / 解散 | ✓ | ✗ | ✗ |
| 退群 | 需先转让 | ✓ | ✓ |

群对象字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `groupId` | uint64 | 群 ID |
| `conversationId` | uint64 | 对应群会话 ID |
| `name` | string | 群名，最大 128 字节 |
| `announcement` | string | 群公告，最大 512 字节 |
| `intro` | string | 群简介，最大 255 字节 |
| `avatarUrl` | string | 群头像，最大 255 字节 |
| `ownerId` | uint64 | 群主 userId |
| `status` | int | `0`=正常，`1`=已解散 |
| `memberLimit` | int | 成员上限，默认 200 |
| `memberCount` | int | 当前成员数（查询者视角） |
| `myRole` | int | 查询者角色：1/2/3；0=非成员 |
| `createdTs` | uint64 | 创建时间（毫秒） |

成员字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `userId` | uint64 | 成员 userId |
| `username` / `nickname` / `avatarUrl` | string | 用户公开资料 |
| `groupNickname` | string | 群内昵称 |
| `role` | int | 1/2/3 |
| `joinedTs` | uint64 | 入群时间（毫秒） |

---

#### 4.14.1 建群

```http
POST /api/groups
```

请求体：

```json
{ "name": "项目讨论组", "memberIds": [1002, 1003] }
```

约束：

- `name` 非空且 `<= 128` 字节
- `memberIds` 至少 1 个，自动去重并排除群主自己
- 初始成员总数（含群主）不能超过 200
- 初始成员必须是发起人的好友
- 成员必须是存在且状态正常的用户

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "group": {
      "groupId": 1,
      "conversationId": 5,
      "name": "项目讨论组",
      "announcement": "",
      "intro": "",
      "avatarUrl": "",
      "ownerId": 1001,
      "status": 0,
      "memberLimit": 200,
      "memberCount": 3,
      "myRole": 1,
      "createdTs": 1760000000000
    }
  }
}
```

建群成功后，除发起人外的每个新成员收到 TCP `GroupMemberChanged(140)`，`action="added"`。

失败：`invalid group name`、`at least one initial member required`、`too many initial members`、`some member user not found`、`initial members must be your friends`。

---

#### 4.14.2 我的群列表 / 群详情

```http
GET /api/groups
GET /api/groups/{groupId}
```

- 列表只返回 `status=0`（未解散）且我是成员的群：

```json
{
  "code": 0,
  "message": "ok",
  "data": { "groups": [ { "groupId": 1, "...": "..." } ] }
}
```

- 详情返回 `data.group` 与 `data.members[]`：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "group": { "groupId": 1, "name": "项目讨论组", "...": "..." },
    "members": [
      {
        "userId": 1001,
        "username": "alice",
        "nickname": "Alice",
        "avatarUrl": "",
        "groupNickname": "",
        "role": 1,
        "joinedTs": 1760000000000
      }
    ]
  }
}
```

详情要求必须是群成员，否则返回 `400 not a group member`；群不存在/已解散返回 `group not found` 或 `group dismissed`。

---

#### 4.14.3 修改群资料

```http
PATCH /api/groups/{groupId}
```

请求体：

```json
{
  "name": "新群名",
  "announcement": "周五例会",
  "intro": "",
  "avatarUrl": ""
}
```

- 只带要改的字段；空串表示不改该字段。
- 所有字段都为空时返回 `400 no fields to update`。
- 仅群主/管理员可操作。
- 字段上限：`name<=128`、`announcement<=512`、`intro<=255`、`avatarUrl<=255`。
- 成功返回 `data.group`；全群收到 `GroupInfoChanged(142)`。
- 失败：`field too long`、`no fields to update`、`not a group member or dismissed`、`no permission`。

---

#### 4.14.4 邀请 / 移出成员

```http
POST   /api/groups/{groupId}/members
DELETE /api/groups/{groupId}/members/{userId}
```

邀请请求体：

```json
{ "memberIds": [1004] }
```

- 邀请仅群主/管理员可用。
- 已在群里的 ID 静默跳过（幂等）；重复邀请导致无新增时返回 `400 all invitees already in group`。
- 人数受 `memberLimit` 限制，超限返回 `400 member limit exceeded`。
- 被邀请人此前对该群的待处理申请会自动置为“已通过”。
- 成功返回群详情：`data.group + data.members[]`。
- 全群收到每个新增成员的 `GroupMemberChanged(140)`，`action="added"`，`role=3`。

移出：

- 仅群主/管理员可用；管理员只能移出普通成员，不能移出群主或其他管理员。
- 成功后剩余成员收到 `GroupMemberChanged(140)`，`action="removed"`；被移出者本人也会单独收到同一条事件。
- 被移出后立即失去发言/拉历史权限（服务端按成员表校验）。
- 成功返回 `{ "code": 0, "message": "ok" }`。

失败：`not a group owner/admin`、`no permission`、`use quit to leave group`、`target is not a group member`、`only owner can remove admin`、`cannot remove group owner`。

---

#### 4.14.5 任命 / 取消管理员

```http
POST /api/groups/{groupId}/admins
```

请求体：

```json
{ "userId": 1002, "admin": true }
```

- 仅群主可操作。
- `admin=true` 升为管理员，`false` 降为普通成员。
- 全群收到 `GroupMemberChanged(140)`，`action="roleChanged"`，`role` 为 2 或 3。
- 成功返回 `{ "code": 0, "message": "ok" }`。

失败：`invalid userId`、`only owner can set admin`、`cannot change your own role`、`target is not a group member`、`cannot change owner role`、`target already admin`、`target is not admin`。

---

#### 4.14.6 转让群主 / 退群 / 解散

```http
POST /api/groups/{groupId}/transfer
POST /api/groups/{groupId}/quit
POST /api/groups/{groupId}/dismiss
```

转让请求体：

```json
{ "toUserId": 1002 }
```

- 仅群主可转让，目标必须是本群成员且不是自己。
- 旧群主自动降为管理员。
- 全群收到 `GroupMemberChanged(140)`：
  - `action="transferred"`，`user_id=新群主`，`role=1`
  - `action="roleChanged"`，`user_id=旧群主`，`role=2`
- 成功返回 `data.group`。

退群：

- 群主必须先转让，否则 `400 owner must transfer first`。
- 剩余成员收到 `GroupMemberChanged(140)`，`action="quit"`，`user_id=退群者`，`role=0`。
- 成功返回 `{ "code": 0, "message": "ok" }`。

解散：

- 仅群主可操作。
- 群被软删除（`status=1`），清理成员、游标、离线队列。
- 解散前成员收到 `GroupDismissed(141)`。
- 此后任何人再收发消息/拉历史均被拒绝。
- 成功返回 `{ "code": 0, "message": "ok" }`。

失败：`group not found or dismissed`、`only owner can transfer`、`cannot transfer to yourself`、`target is not a group member`、`not a group member`、`owner must transfer first`。

---

#### 4.14.7 入群申请与审批

```http
POST /api/groups/requests
GET  /api/groups/requests?direction=incoming|outgoing|all
POST /api/groups/requests/{requestId}/accept
POST /api/groups/requests/{requestId}/reject
```

申请入群请求体：

```json
{ "groupId": 1, "message": "拉我一把" }
```

- `message` 可选，最大 255 字节。
- `direction` 默认 `incoming`；`incoming` 表示需要我审批的申请（我是群主/管理员的群），`outgoing` 表示我发出的申请。
- 申请成功后实时推 `GroupRequest(143)` 给群主；离线群主/管理员在 TCP 登录时补推。
- 审批后推 `GroupRequestResult(144)` 给申请人（离线则登录补推）；通过时全群再收 `GroupMemberChanged(140)`，`action="added"`。

入群申请对象字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `requestId` | uint64 | 申请 ID |
| `groupId` | uint64 | 群 ID |
| `groupName` | string | 群名 |
| `ownerId` | uint64 | 群主 userId |
| `requesterId` | uint64 | 申请人 userId |
| `requesterUsername` | string | 申请人用户名 |
| `requesterNickname` | string | 申请人昵称 |
| `handlerId` | uint64 | 审批人；0=未处理 |
| `message` | string | 附言 |
| `status` | int | 0/1/2/3 |
| `createdTs` | uint64 | 申请时间（毫秒） |

失败：`missing groupId`、`invalid groupId`、`message too long`、`group not found or dismissed`、`already in group`、`request already pending`、`request not found or already handled`、`no permission to handle`、`member limit exceeded`。

---

### 4.15 文件模块：图片/文件七牛云直传

单聊/群聊的图片（`type=2`）与文件（`type=4`）功能。数据面与控制面彻底分离：
**文件二进制由客户端直传七牛云对象存储（Kodo），不经过 IM 服务器、不落服务器
磁盘**；IM 服务器只做控制面——签凭证、传后核验登记、发送时权属校验与元数据
重建、下载时签发短时效 URL。

```text
客户端                    IM 服务器                     七牛云
   │ 1 POST /api/files/upload-token ─►│ 白名单/大小/配额校验，
   │ ◄── fileId/key/uploadUrl/uptoken/ticket          （本地 HMAC 签发）
   │ 2 直传字节（uptoken）──────────────────────────────►│（数据面，服务器不经手）
   │   = 分片上传 V2（推荐，见 4.15.6）或表单直传
   │ 3 POST /api/files/commit ───────►│ GET /stat 核验对象存在
   │ ◄── fileId/url/size/qetag         │ INSERT files（size/qetag 以 stat 为准）
   │ 4 TCP type=2/4 {"file_id":…} ───►│ 权属校验+元数据重建后入库/投递
   │ 5 GET /api/files/{uuid} ────────►│ 现场签发短时效 URL
   │ ◄── 302 Location ─────────────────────────────────►│（带签名与 deadline）
```

**直传方式对上层完全透明**：第 2 步选分片 V2 还是表单直传，控制面（1/3/4/5）
一字不变，服务器只在 commit 时用 `stat` 核验结果。所以客户端可以按文件大小
自行选择（小图走表单更省一次往返，视频/大文档走分片 V2），互操作口径见 4.15.6。

#### 4.15.1 POST /api/files/upload-token（Bearer）

请求体：`{"name":"照片.png","mime":"image/png","size":4096}`
（`mime` 可选；闸门是 `name` 的扩展名白名单：图片 jpg/jpeg/png/gif/webp/bmp，
文档 pdf/txt/md/doc/docx/xls/xlsx/ppt/pptx，压缩包 zip/rar/7z，音视频
mp3/mp4/wav；其余一律 400。可执行类型不在白名单内即被拒。）

成功 `data`：

| 字段 | 说明 |
| --- | --- |
| `fileId` | 服务端生成的随机 uuid v4（对外文件标识） |
| `key` | 七牛对象名 `im/<yyyy-mm>/<uuid>.<ext>`，服务端生成、客户端不可选 |
| `uploadUrl` | 直传目标基址（真实模式=七牛 uphost，如 `https://up-z2.qiniup.com`；mock 模式=`/api/files/mock-put`）。分片 V2 在它后面拼 `/buckets/<bucket>/objects/<b64url(key)>/...`，表单直传拼 `/<key>` |
| `uptoken` | 七牛上传凭证 `AK:sign:encodedPolicy`，policy 字段：`scope="<bucket>:<key>"`（锁死单对象）、`deadline`（绝对 Unix 秒，**签发 + 3600**）、`fsizeLimit`（申报大小，由七牛端强制执行）、`insertOnly=1`（禁覆盖已有对象）。只授权写这一个对象，不含任何密钥 |
| `expiresIn` | 凭证有效期秒数（3600）。**注意**：`bucket` 名也在这份 policy 里（`scope` 冒号前段），所以客户端做分片 V2 时**不需要任何额外配置**，从 `uptoken` 第三段解出 bucket 即可（与官方 SDK `Qiniu_Utils_Extract_Bucket` 同做法） |
| `ticket` | 无状态"签发记录"（HMAC-SHA256 自包含票据，**2h** 时效）：commit/mock-put 必带，证明"这个 file_id/key/大小/名字是你本人刚申请的"。`name/mime` 也签进票据——落库的就是申请时过了白名单的那份，客户端没有"换个名字再 commit"的机会 |

失败：`missing name` / `missing or invalid size` / `file too large (limit NMB)` /
`file type not allowed` / `invalid name` / `daily upload quota exceeded`（429）。
本接口**不写 files 表**——没有 commit 的文件在业务上不存在。

#### 4.15.2 POST /api/files/commit（Bearer）

请求体：`{"file_id":"<uuid>","ticket":"<upload-token 返回的 ticket>"}`

服务端先验 ticket（归属+时效+字段完整性），再调存储 `stat` 核验对象**真实
存在**——没直传就 commit 会被拒（防虚报）。成功后以 stat 返回的权威
`size/qetag` 登记 files 表。

成功 `data`：`{"fileId":…,"url":"http://<FILE_BASE_URL>/api/files/<uuid>",
"size":…,"qetag":…}`

失败：`missing file_id/ticket` / `invalid or expired upload ticket`（403）/
`object not uploaded`（400）/ `object exceeds size limit`（400）/
`storage unavailable`（503，存储端不可达；不影响文本链路）。

#### 4.15.3 GET /api/files/{uuid}：下载换取（302）

双鉴权：带 `Authorization: Bearer`，或免 Bearer 的签名参数
`?e=<deadline>&vtoken=<hmac>`（`<img>` 标签跟不了请求头，302 能）。
`files.status=1` 时返回 `302` + `Location: <短时效签名直链>`；否则 404。
**消息体里存的一直是这个换取入口**（稳定 URL），真正的七牛签名地址每次
现场生成、`FILE_URL_TTL_SEC`（默认 600s）后过期——历史消息不会因签名过期而坏。

MVP 权限口径：登录用户（或持有效 vtoken）拿到 uuid 即可换取一次；
限制到"引用过该文件的会话成员"为后续项。

#### 4.15.4 DELETE /api/files/{uuid}（Bearer）

仅上传者本人：先标记 `files.status=2` 再物理删除存储对象（顺序刻意：
业务侧先不可见）。非本人/已删除/不存在统一 `404 not found`（防探测）。

#### 4.15.5 mock 端点（仅 `QINIU_ENABLED=0`）

- `POST /api/files/mock-put`（Bearer，`{"file_id","ticket"}`）：登记对象元数据
  （模拟"直传成功"）；真实模式 404。
- `GET /api/files/mock-object/{key}?e=&token=`：模拟私有空间签名下载，返回
  合成字节 + `X-Mock-Size` / `X-Mock-Qetag` 响应头供核对。
- mock 只模拟七牛的**协议语义**（存在性/时效/签名/防篡改），不搬运真实字节；
  真实数据面只在 `QINIU_ENABLED=1` 时存在。
- ⚠ 但 mock **盖不住"与外部系统对齐口径"**：它自己签自己验，错口径也自洽
  （下载凭证、管理签名两处都出现过"mock/单测全绿、真机 401"）。凡涉及七牛
  的签名/域名/区域，定案只能靠真机验收（见 4.15.9）。

#### 4.15.6 客户端直传的两种方式（数据面协议）

两种都是"客户端把字节直接推给七牛 `uploadUrl`"，服务器不经手；鉴权都用
`Authorization: UpToken <uptoken>`（**表单直传例外**：它把 token 作为表单字段传）。

**A. 分片上传 V2（推荐；官方 C SDK `Qiniu_Multipart_PutFile` 同构）**

| 步 | 方法与路径 | 请求头 | 响应 |
| --- | --- | --- | --- |
| 1 init | `POST {uploadUrl}/buckets/{bucket}/objects/{b64url(key)}/uploads` | `Authorization: UpToken …`、`Content-Type: application/octet-stream`（空 body） | `{"uploadId":"…","expireAt":…}` |
| 2 每片 | `PUT {uploadUrl}/buckets/{bucket}/objects/{b64url(key)}/uploads/{uploadId}/{partNumber}` | 同上；body = 该片的**原始字节** | `{"etag":"…","md5":"…"}` |
| 3 complete | `POST {uploadUrl}/buckets/{bucket}/objects/{b64url(key)}/uploads/{uploadId}` | `Authorization: UpToken …`、`Content-Type: application/json`；body `{"parts":[{"etag":"…","partNumber":1},…],"mimeType":"…","fname":"…"}` | `{"key":"…","hash":"<qetag>"}` |

- `partNumber` 从 **1** 起；片大小合法区间 **1MB ~ 1GB**（官方 SDK 默认 4MB），
  总片数 ≤ 10000；0 字节也要 1 片。
- `bucket` 取自 `uptoken` 的 `policy.scope` 冒号前段；`key` 用 **urlsafe Base64**
  （保留 `=` 填充，与服务端 `entryBase64` 同一口径）编码后放进路径。
- 第 3 步返回的 `hash` 就是七牛 `qetag`；随后 commit 时服务器用 `stat` 独立取回的
  `qetag` 必须与它**逐字节相同**——验收里"控制面与数据面口径一致"的判据就是这一条。
- 失败定位：init 401 ⇒ 凭证/scope；part 401 而 init 通过 ⇒ `UpToken` 头没带上；
  complete 400 ⇒ `parts` 里的 `etag/partNumber` 与实发分片对不上（片大小/顺序）。

**B. 表单直传（小文件省一次往返；官方 `Qiniu_Io_PutFile` 同构）**

`POST {uploadUrl}/{key}`，`multipart/form-data` 三段：`token=<uptoken>`、
`key=<object key>`、`file=<二进制, filename=<名字>>` + 该片 `Content-Type`。
成功 200，body `{"key":"…","hash":"<qetag>"}`。

#### 4.15.7 七牛侧三处签名口径（实现者必读，均已真机定案）

| 用途 | 待签串 dataToSign | 结果 | 出处 |
| --- | --- | --- | --- |
| 上传凭证 uptoken | `encodedPolicy`（**只有它**，不含 URL path） | `AK:urlsafe_b64(hmac_sha1(SK,dataToSign)):encodedPolicy` | 官方「上传凭证」文档 + C SDK `Qiniu_Mac_SignToken` |
| 私有空间下载 | **整条绝对下载 URL**（含 `scheme://host/path`）+ `"?e=" + deadline` | URL 追加 `&token=AK:urlsafe_b64(hmac_sha1(SK,dataToSign))` | C SDK `Qiniu_RS_GetPolicy_MakeRequest` |
| 管理 API（stat/delete） | `"<METHOD> <Path>[?Query]\nHost: <Host>\n[Content-Type: …\n][X-Qiniu-Date: …\n]\n[Body]"` | 请求头 `Authorization: Qiniu AK:urlsafe_b64(hmac_sha1(SK,dataToSign))` | C SDK `Qiniu_Mac_AuthV2` + `Qiniu_Mac_HmacV2` |

四条容易写歪、且**自洽的单测挡不住**的要点：

1. **有效期字段必须叫 `deadline`**（绝对 Unix 秒）。写成 `expires` 会被七牛整字段
   忽略 ⇒ 截止时间按 0 处理 ⇒ 恒定 401 `expired token`，而签名本身是好的。
2. **下载凭证签的是整条 URL，scheme 也在里面**：`QiniuConfig.json` 的 `domain`
   写 `https://` 就必须用 https 请求，写 `http://` 就必须用 http，换协议必然验签失败。
3. **管理签名里 method 与 path 之间是空格**，`Host:`/`Content-Type:` 是"键冒号空格值"
   的头形式，且头与 body 之间**必有一个空行**；"发出去的头"与"签进去的头"必须成对
   （本项目先后用过 q-sign v2 与 `METHOD\nHost\nPath` 两种错口径，两次都是单测全绿、
   真机 401 `BadToken`）。
4. **管理 API 的 entry 是 `urlsafe_b64("<bucket>:<key>")`**（bucket 在前）。写反不会
   报鉴权错——entry 整段都在签名里，两边自洽——七牛只会回"查无此对象"（612），
   极易被误判成"客户端根本没传上去"。

#### 4.15.8 区域、下载域名与 CDN 缓存约束

- **区域三件套必须同区**：`zone`、`up_host`（如 z2 华南 = `https://up-z2.qiniup.com`）、
  下载 `domain`。上传按 `up_host` 走、下载按 `domain` 走，配错区域的表现是
  "直传 200、commit 通过、下载 403/404"。
- **一级测试域名（`*-bkt.clouddn.com`）只能当临时测试用**：只在 **http** 上服务
  （https 会被边缘节点判 `421 Misdirected Request` 的 HTML 页）、有 QPS/带宽限制
  （自动化反复拉取会吃 **429**）、约 30 天回收。生产必须绑**已备案自有域名**并在
  七牛配好 HTTPS 证书。
- **CDN"忽略查询参数"缓存会绕过私有直链的时效性**：对象一旦被带 `token` 成功取回，
  同一 path 不带 token 也可能命中缓存直接 200。生产必须给下载域名配置**保留查询
  参数**（或开启私有空间回源鉴权），否则 `FILE_URL_TTL_SEC` 形同虚设。
- **SNI 必须发送**（上传/下载/管理三类连接都是）：这些端点多租户共用接入 IP，
  ClientHello 里没有 `server_name` 会落到默认站点，症状是"握手成功但证书/响应不对"。

#### 4.15.9 错误语义与排障（真机）

| 出现位置 | 响应 | 含义 / 第一动作 |
| --- | --- | --- |
| 直传 | 401 `bad token` / `error_code=BadToken` | 先怀疑**签名口径**（见 4.15.7 第 1 点），再怀疑 AK/SK 不配对、AK 无该 bucket 上传权限、`up_host` 区域不符 |
| 直传 | 401 `expired token` | 先查 policy 用的是 `deadline` 还是 `expires`；字段名没错才去校时 |
| commit | 503 `qiniu stat http=401 … BadToken` | 管理签名口径（4.15.7 第 3 点）；服务端会把七牛原话与 `X-Reqid` 一起回显 |
| commit | 503 `qiniu stat http=612` | 签名/权限都对，七牛说"查无此 entry"：先确认真传成功过，再查 entry 顺序 `bucket:key` |
| commit | 400 `object not uploaded` | 正确的负向行为（申请了凭证但没直传） |
| 下载 | 401 `download token not specified` | 请求确实没带 token（私有空间的正常拒绝），说明域名与 Kodo 是通的 |
| 下载 | 401 `download token auth failed` | 带 token 但验签不过 ⇒ 待签串口径错（少签了 scheme/host 是本项目踩过的）|
| 下载 | 421 / 403 / 429 且响应体是 **HTML** | 边缘节点拒的，**与签名无关**：域名没 HTTPS 站点 / 未绑定该 bucket / 测试域名被回收限流 |
| 本服务器 | 429 `daily upload quota exceeded` | 超过 `FILE_USER_DAILY_QUOTA_MB`（带 `Retry-After`），与七牛限流无关，看响应体即可分辨 |

排查工具：`step3_verify/qiniu_token_probe.py`（上传凭证口径）、
`step3_verify/qiniu_manage_probe.py`（管理 API 的 host × 口径矩阵）、
`RUN_QINIU_REAL=1 bash step3_verify/verify_Qiniu_file.sh`（端到端真机验收）。

---

## 5. TCP 协议

### 5.1 分帧

TCP 是字节流，协议采用固定长度头分帧：

```text
+----------------------+-----------------------------+
| 4 字节长度 N（大端） | N 字节 JSON body（UTF-8）   |
+----------------------+-----------------------------+
```

- 长度字段：`uint32`，**网络字节序（大端）**，只表示 body 字节数，不含 4 字节头。
- body：`Message` 对象的 JSON 序列化。
- **客户端上行单帧 body 上限：4096 字节**（`frame.h` 的 `MAX_BODY`）。
- 客户端发送的 `N == 0` 或 `N > 4096`：服务端**直接关闭连接**，不会返回错误帧。
- 恶意长度/超大帧不会分配大内存；客户端必须保证**自己发送的**单帧不超过 4096 字节。
- 图片/文件不要把二进制塞进 `content`：先直传对象存储拿 `file_id`，`content` 只放服务端重建的媒体元数据（见 4.15 / 5.8）。

> 4096 是**入站（客户端 → 服务端）**限制。服务端下行帧没有这个 4096 限制，
> 例如一次返回 100 条消息的 `HistoryResp` 可能明显超过 4096 字节。
> 客户端接收时必须严格按“长度头指定的 N”读取，**不要**用 4096 去限制下行帧。
> 服务端下行写队列一次 `async_write` 最多合并 32 帧 / 64KB，但每帧仍是独立的
> 4 字节长度头 + JSON body，客户端按帧解析即可。

### 5.2 Message JSON 结构

```json
{
  "from": 1001,
  "to": 1002,
  "type": 1,
  "msg_id": 123,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "hello",
  "is_group": false
}
```

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `from` | uint64 | **是** | 发送者 userId。客户端发送时可为 0，服务端下行以会话身份为准 |
| `to` | uint64 | 否 | 接收者 userId；群聊时为 `groupId`；登录/心跳可为 0 |
| `type` | int | **是** | 消息类型，必须是 JSON 整数 |
| `msg_id` | uint64 | 否 | 客户端发送时为临时消息号；服务端下行时为服务端消息 ID；事件中复用为 `request_id` |
| `ts` | uint64 | 否 | 毫秒时间戳；服务端消息由服务端生成 |
| `conversation_id` | uint64 | 否 | 会话 ID |
| `seq` | uint64 | 否 | 会话内单调序号；历史请求时表示 `before_seq` |
| `content` | string | 视类型 | 文本、URL、token 或 JSON 字符串 |
| `is_group` | bool | 否 | `true` 表示 `to` 为 `groupId`，走群聊逻辑；缺省/`false`=单聊 |

解析约束（客户端发送时必须满足）：

- `from` 必须存在且为 uint64。
- `type` 必须存在且为 JSON int。
- 未知 `type` 会被服务端判定为非法消息并**直接关闭连接**。
- `Text(1)` / `Image(2)` 必须带 `content`（字符串）。
- `HistoryResp(111)` 必须带 `content`。
- 其他类型 `content` 可选。
- `is_group` 接受 bool 或 int（非 0 为 true）。
- 服务端下行序列化时**始终写出** `is_group`（`true/false`）。

### 5.3 消息类型

| 值 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `1` | Text | 双向 | 文本消息 |
| `2` | Image | 双向 | 图片消息，`content` 为媒体元数据 JSON（见 5.8） |
| `3` | Heartbeat | 客户端 → 服务端 | 心跳，刷新空闲计时 |
| `4` | File | 双向 | 文件消息，`content` 为媒体元数据 JSON（见 5.8；⚠ 客户端须先支持再上线服务端） |
| `100` | Login | 客户端 → 服务端 | 登录，`content` 放 JWT |
| `101` | LoginAck | 服务端 → 客户端 | 登录成功 |
| `102` | Error | 服务端 → 客户端 | 错误通知 |
| `110` | HistoryReq | 客户端 → 服务端 | 历史消息请求 |
| `111` | HistoryResp | 服务端 → 客户端 | 历史消息响应 |
| `112` | Ack | 客户端 → 服务端 | 投递/已读回执 |
| `113` | SendAck | 服务端 → 客户端 | 发送成功确认 |
| `130` | FriendRequest | 服务端 → 客户端 | 收到好友申请 / 登录补推 |
| `131` | FriendRequestResult | 服务端 → 客户端 | 好友申请通过或拒绝 |
| `132` | FriendListChanged | 服务端 → 客户端 | 好友列表变化 |
| `140` | GroupMemberChanged | 服务端 → 客户端 | 群成员变化 |
| `141` | GroupDismissed | 服务端 → 客户端 | 群被解散 |
| `142` | GroupInfoChanged | 服务端 → 客户端 | 群资料变化 |
| `143` | GroupRequest | 服务端 → 客户端 | 收到入群申请 / 登录补推 |
| `144` | GroupRequestResult | 服务端 → 客户端 | 入群申请处理结果 |

### 5.4 连接生命周期

1. 客户端建立 TCP 连接。
2. 发送 `type=100` 登录帧，`content` 为 access token。
3. 服务端验签成功返回 `type=101`；失败返回 `type=102`。
4. 登录成功后服务端异步补推离线消息与未处理事件。
5. 客户端定期发送 `type=3` 心跳。
6. 同一账号重复登录会踢掉旧连接；旧连接被关闭。
7. 空闲超过 `IDLE_TIMEOUT_SEC`（默认 60s）被服务端断开。
8. 服务端连接数超过 `TCP_MAX_SESSIONS` 时，新连接被立即关闭。

> 心跳建议 **30 秒**，必须显著小于空闲超时。服务端**不回复 pong**，只刷新空闲计时。
> 心跳帧可以只带 `{"from":0,"to":0,"type":3}`，因为 `from` 和 `type` 是必填字段。

### 5.5 TCP 登录

请求：

```json
{
  "from": 0,
  "to": 0,
  "type": 100,
  "msg_id": 0,
  "ts": 0,
  "conversation_id": 0,
  "seq": 0,
  "content": "<access_token>"
}
```

成功：

```json
{
  "from": 0,
  "to": 1001,
  "type": 101,
  "msg_id": 0,
  "ts": 0,
  "conversation_id": 0,
  "seq": 0,
  "content": "welcome",
  "is_group": false
}
```

失败：

```json
{
  "from": 0,
  "to": 0,
  "type": 102,
  "content": "invalid or expired token"
}
```

补充：

- 服务端只信任 JWT 的 `sub`，忽略客户端自报的 `from`。
- 重复发送登录帧（已登录/正在登录）返回 `type=102`，`content="already logged in"`。
- 刷新 token 不能用于 TCP 登录，会被拒绝。

### 5.6 心跳

```json
{ "from": 0, "to": 0, "type": 3 }
```

- 服务端不回复。
- 任何合法帧都会刷新空闲计时。
- 建议 30s 一次；不要用缩短心跳来做死链检测，可依赖 TCP `SO_KEEPALIVE` 或应用层探测。

### 5.7 发送文本消息

客户端发送：

```json
{
  "from": 0,
  "to": 1002,
  "type": 1,
  "msg_id": 10001,
  "content": "hello B"
}
```

- `from` 可填 0，服务端以登录身份为准。
- `msg_id` 非 0 时作为去重键：同一会话、同一发送者、同一 `msg_id` 不会产生两条消息。
- 重试必须保持同一 `msg_id`。
- `msg_id=0` 表示不做去重。

发送方收到 `SendAck(113)`：

```json
{
  "from": 0,
  "to": 1001,
  "type": 113,
  "msg_id": 88,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "sent",
  "is_group": false
}
```

- `content="sent"`：新消息已落库。
- `content="duplicate"`：命中去重，返回已存在消息的 `msg_id/seq/ts`。

接收方在线时收到：

```json
{
  "from": 1001,
  "to": 1002,
  "type": 1,
  "msg_id": 88,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "hello B",
  "is_group": false
}
```

失败：

- 未登录：`login first`
- 接收者非法：`bad recipient`
- 非好友：`not friends`
- 被对方拉黑：`blocked by recipient`
- 过载：`server busy`
- 其他落库错误：`send failed` 或具体错误文本

### 5.8 发送图片 / 文件消息（媒体元数据）

`type=2`（图片）与 `type=4`（文件）的 `content` 是一段媒体元数据 JSON。
发送时客户端只需带 `file_id`（上传 commit 后获得，见 4.15）：

```json
{
  "from": 0,
  "to": 1002,
  "type": 2,
  "msg_id": 10002,
  "content": "{\"file_id\":\"cc50e658-fc97-4f61-b6b5-7976ccbbf375\"}"
}
```

群媒体：`to` 填 groupId、`"is_group": true`、`type` 用 2 或 4。

服务端校验链（全部通过才入库/投递）：content 是合法 JSON → file_id 在 files
表存在且未删除 → **上传者 = 发送者本人**。通过后 `content` 被服务端用数据库
行**整体重建**，接收端收到的是权威元数据：

```json
{
  "file_id": "cc50e658-fc97-4f61-b6b5-7976ccbbf375",
  "url": "http://<FILE_BASE_URL>/api/files/cc50e658-…",
  "name": "照片.png",
  "size": 4096,
  "mime": "image/png",
  "qetag": "28f0…"
}
```

- `url` 恒为**服务器换取入口**（GET 302 → 短时效签名直链，见 4.15.3）；
  客户端自报的 url 一律被替换——消息内容不可能诱导接收端打开外部链接。
- 校验失败返回 `type=102` Error 帧（不断连接）：`bad media metadata: …`、
  `file not found or deleted`、`file not uploaded by sender`。
- 去重沿用 `msg_id`（client_msg_id）现有机制；离线补推/历史/回执对媒体
  消息与普通文本完全同构（type 原样透传）。
- 禁止：把二进制（含 base64）塞进 `content`（上行帧 4096 上限）；把七牛
  签名直链（带 deadline）写进消息——会随过期腐化历史消息。
- `file_id` 从哪来不影响本节的线格式：直传用分片 V2 还是表单（4.15.6）、文件在
  哪个区域哪个 bucket（4.15.8），对 TCP 消息都不可见——消息里只有一个 uuid 和
  服务端重建的元数据。这也是"数据面换存储/换上传方式不用发客户端"的根据。
- **兼容红线**：未知 type 的旧客户端会判非法并断线；上线 `type=4` 前必须
  先发布能识别 type=4 的客户端。

### 5.9 历史消息

请求：

```json
{
  "from": 0,
  "to": 1002,
  "type": 110,
  "seq": 0,
  "content": "20"
}
```

| 字段 | 说明 |
| --- | --- |
| `to` | 对方 userId（单聊）或 `groupId`（群聊，配合 `is_group=true`） |
| `seq` | `before_seq`，0 表示从最新开始 |
| `content` | 本次拉取条数，字符串形式 |

- `content` 缺省时默认 20。
- 接入层把条数钳制在 `1 ~ 200`，DB 层再钳制到最多 `100`，因此**有效上限为 100 条/次**。
- 响应 `type=111`，`content` 是一段 **JSON 数组文本**，客户端需要二次解析：

```json
{
  "from": 0,
  "to": 1001,
  "type": 111,
  "content": "[{\"msg_id\":88,\"conversation_id\":5,\"seq\":7,\"from\":1001,\"type\":1,\"ts\":1760000000000,\"content\":\"hello B\",\"is_group\":false}]",
  "is_group": false
}
```

数组元素字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `msg_id` | uint64 | 服务端消息 ID |
| `conversation_id` | uint64 | 会话 ID |
| `seq` | uint64 | 会话内序号 |
| `from` | uint64 | 发送者 userId |
| `type` | int | 消息类型（1/2） |
| `ts` | uint64 | 毫秒时间戳 |
| `content` | string | 消息内容 |
| `is_group` | bool | 是否群消息 |

失败：`login first`、`bad peer`、`history failed`、`server busy`；群历史非成员会返回 `not a group member` 等错误。

### 5.10 投递 / 已读回执

只确认投递：

```json
{
  "from": 0,
  "to": 0,
  "type": 112,
  "conversation_id": 5,
  "seq": 7,
  "content": "delivered"
}
```

确认已读（`content` 不是 `"delivered"` 即视为已读）：

```json
{
  "from": 0,
  "to": 0,
  "type": 112,
  "conversation_id": 5,
  "seq": 7,
  "content": ""
}
```

- `conversation_id == 0` 或 `seq == 0` 时服务端静默忽略。
- 服务端更新 `last_delivered_seq` / `last_read_seq`（只增不减）。
- 在行级离线队列模式下，会删除已投递的离线消息。
- 服务端不回复 ACK。
- 过载时 ACK 可能被静默丢弃（幂等，下一条更大的 `seq` 会补上）。

### 5.11 群消息发送 / 历史 / 回执

群消息复用 `type=1/2`，把 `to` 填成 `groupId` 并带 `is_group=true`。

发送：

```json
{
  "from": 0,
  "to": 1,
  "type": 1,
  "is_group": true,
  "msg_id": 10001,
  "content": "大家好"
}
```

服务端在同一事务内：

1. 校验发送者是群成员且群未解散；
2. 原子分配 `seq`；
3. 写 `messages`；
4. 给除发送者外全体成员写离线队列（写扩散）。

发送方收到 `SendAck(113)`：

```json
{
  "from": 0,
  "to": 1001,
  "type": 113,
  "msg_id": 88,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "sent",
  "is_group": true
}
```

- 命中去重时 `content="duplicate"`，不再向群成员重复投递。

在线成员收到：

```json
{
  "from": 1001,
  "to": 1,
  "type": 1,
  "msg_id": 88,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "大家好",
  "is_group": true
}
```

- 发送者自己不会收到这条下行消息，只收到 `SendAck`。
- 离线成员登录时由离线队列/游标补推。

群历史请求：

```json
{
  "from": 0,
  "to": 1,
  "type": 110,
  "is_group": true,
  "seq": 0,
  "content": "20"
}
```

- 响应 `type=111`，`is_group=true`，`content` 为数组文本，每条含 `is_group`。
- 有效条数上限与单聊一致：接入层 `<=200`，DB 层实际最多 `100`。
- 非成员拉群历史会被拒绝（`not a group member` 等）。

群回执：`type=112` 与单聊一致，带 `conversation_id + seq`，更新该成员在群会话中的游标并删除已投递离线消息。

失败：`login first`、`bad group id`、`server busy`、`group send failed`、`not a group member or group dismissed`。

### 5.12 好友事件（130 / 131 / 132）

事件帧的公共信封：

- `type` 为事件类型；
- `content` 是 **JSON 文本字符串**，客户端需要二次解析；
- `msg_id` 复用为 `request_id`（130/131）；
- `conversation_id=0`、`seq=0`；
- `is_group=false`。

#### FriendRequest（130）

方向：申请人 → 被申请人。新申请实时推送；离线申请在登录时以 `event="pending"` 补推。

```json
{
  "from": 1001,
  "to": 1002,
  "type": 130,
  "msg_id": 10,
  "ts": 1760000000000,
  "content": "{\"event\":\"new\",\"request_id\":10,\"requester_id\":1001,\"target_id\":1002,\"requester_username\":\"alice\",\"requester_nickname\":\"Alice\",\"message\":\"你好，我是 Alice\",\"status\":0,\"ts\":1760000000000}"
}
```

`content` 字段：

| 字段 | 说明 |
| --- | --- |
| `event` | `new`=新申请；`pending`=登录补推的待处理申请 |
| `request_id` | 申请 ID；与信封 `msg_id` 相同 |
| `requester_id` | 申请人 userId |
| `target_id` | 被申请人 userId |
| `requester_username` / `requester_nickname` | 申请人展示信息 |
| `message` | 申请附言 |
| `status` | 申请状态（0） |
| `ts` | 申请创建时间（毫秒） |

客户端处理申请后调用 HTTP `accept` / `reject`。

#### FriendRequestResult（131）

方向：被申请人（处理者） → 申请人。申请人离线时登录补推。

```json
{
  "from": 1002,
  "to": 1001,
  "type": 131,
  "msg_id": 10,
  "ts": 1760000000000,
  "content": "{\"event\":\"accepted\",\"request_id\":10,\"requester_id\":1001,\"target_id\":1002,\"target_username\":\"bob\",\"target_nickname\":\"Bob\",\"message\":\"你好，我是 Alice\",\"status\":1,\"ts\":1760000000000}"
}
```

- `event`：`accepted` / `rejected`
- `status`：`1` / `2`
- 若申请人在线且已实时推送，服务端会标记“已通知”，避免下次登录重复推送。

#### FriendListChanged（132）

方向：操作者 → 自己或对方。

```json
{
  "from": 1001,
  "to": 1002,
  "type": 132,
  "msg_id": 0,
  "ts": 1760000000000,
  "content": "{\"event\":\"friend_list_changed\",\"action\":\"added\",\"friend_id\":1001,\"remark\":\"\",\"status\":1}"
}
```

`content` 字段：

| 字段 | 说明 |
| --- | --- |
| `event` | 固定 `friend_list_changed` |
| `action` | `added` / `deleted` / `updated` / `blocked` / `unblocked` |
| `friend_id` | 相关好友 userId |
| `remark` | 备注（`updated` 时有意义） |
| `status` | `1`=好友，`2`=拉黑，`0`=删除/取消拉黑 |

`action` 触发场景：

| action | 触发场景 |
| --- | --- |
| `added` | 好友申请被通过 |
| `deleted` | 删除好友 |
| `updated` | 修改备注 |
| `blocked` | 拉黑 |
| `unblocked` | 取消拉黑 |

该事件**不持久化**。离线或多端场景下，客户端登录后应调用 `GET /api/friends` 拉全量列表对齐。

### 5.13 群事件（140 ~ 144）

群操作走 HTTP，事件由服务端主动下行；`content` 是 JSON 文本。

#### GroupMemberChanged（140）

触发：建群/邀请（`added`）、移出（`removed`）、退群（`quit`）、任命/取消管理员（`roleChanged`）、转让（`transferred`）。

```json
{
  "from": 1001,
  "to": 1002,
  "type": 140,
  "content": "{\"event\":\"group_member_changed\",\"action\":\"added\",\"group_id\":1,\"user_id\":1003,\"role\":3,\"operator_id\":1001}"
}
```

| 字段 | 说明 |
| --- | --- |
| `event` | 固定 `group_member_changed` |
| `action` | `added` / `removed` / `quit` / `roleChanged` / `transferred` |
| `group_id` | 群 ID |
| `user_id` | 本次被操作的成员 |
| `role` | 操作后的角色；`removed`/`quit` 为 0 |
| `operator_id` | 操作者 userId |

#### GroupDismissed（141）

```json
{
  "from": 1001,
  "to": 1002,
  "type": 141,
  "content": "{\"event\":\"group_dismissed\",\"group_id\":1,\"operator_id\":1001,\"name\":\"项目讨论组\"}"
}
```

#### GroupInfoChanged（142）

```json
{
  "from": 1001,
  "to": 1002,
  "type": 142,
  "content": "{\"event\":\"group_info_changed\",\"group_id\":1,\"operator_id\":1001,\"name\":\"新群名\"}"
}
```

客户端收到后可重新调用 `GET /api/groups/{groupId}` 拉取最新资料。

#### GroupRequest（143）

方向：申请人 → 群主（在线直推；离线登录补推）。`msg_id` 复用为 `request_id`。

```json
{
  "from": 1003,
  "to": 1001,
  "type": 143,
  "msg_id": 20,
  "content": "{\"event\":\"new\",\"requestId\":20,\"groupId\":1,\"groupName\":\"项目讨论组\",\"ownerId\":1001,\"requesterId\":1003,\"requesterUsername\":\"carol\",\"requesterNickname\":\"Carol\",\"handlerId\":0,\"message\":\"拉我一把\",\"status\":0,\"createdTs\":1760000000000}"
}
```

- `event`：`new`=新申请；`pending`=登录补推的待处理申请。
- 客户端处理后调用 HTTP `accept` / `reject`。

#### GroupRequestResult（144）

方向：审批人 → 申请人；离线时登录补推。

```json
{
  "from": 1001,
  "to": 1003,
  "type": 144,
  "msg_id": 20,
  "content": "{\"event\":\"accepted\",\"requestId\":20,\"groupId\":1,\"groupName\":\"项目讨论组\",\"ownerId\":1001,\"requesterId\":1003,\"requesterUsername\":\"carol\",\"requesterNickname\":\"Carol\",\"handlerId\":1001,\"message\":\"拉我一把\",\"status\":1,\"createdTs\":1760000000000}"
}
```

- `event`：`accepted` / `rejected`
- `status`：`1` / `2`

### 5.14 登录补推顺序

TCP 登录成功后，服务端先发 `LoginAck(101)`，然后按以下顺序补推：

1. 离线消息（单聊与群聊混合，最多 `kLoginOfflineBatch=100` 条）
2. 待我处理的好友申请（`FriendRequest(130)`，`event="pending"`）
3. 我发出的好友申请结果（`FriendRequestResult(131)`）
4. 待我审批的入群申请（`GroupRequest(143)`，`event="pending"`）
5. 我发出的入群申请结果（`GroupRequestResult(144)`）

离线消息超过 100 条时，`offlineMore` 目前**只写服务端日志，没有下行“续拉”信令**；剩余部分需等客户端下一次登录继续补推，或由客户端通过历史接口主动对齐。

### 5.15 离线与事件补偿矩阵

| 数据/事件 | 是否持久化 | 离线补偿方式 |
| --- | --- | --- |
| 单聊/群聊消息 | 是（`messages`） | 登录补推（每次最多 100 条）或历史接口 |
| 好友申请 `FriendRequest(130)` | 是（`friend_requests`） | 登录补推 `event="pending"` |
| 好友申请结果 `FriendRequestResult(131)` | 是（`result_notified`） | 登录补推未通知结果 |
| 好友列表变化 `FriendListChanged(132)` | 否 | 登录后 `GET /api/friends` 全量对齐 |
| 群成员变化 `GroupMemberChanged(140)` | 否 | 登录后 `GET /api/groups` / `GET /api/groups/{id}` 对齐 |
| 群解散 `GroupDismissed(141)` | 群状态持久化 | 登录后群列表不再返回该群 |
| 群资料变化 `GroupInfoChanged(142)` | 是（`groups`） | 登录后拉群详情对齐 |
| 入群申请 `GroupRequest(143)` | 是（`group_requests`） | 登录补推 `event="pending"` |
| 入群申请结果 `GroupRequestResult(144)` | 是（`result_notified`） | 登录补推未通知结果 |

---

## 6. 错误处理与重试

### 6.1 TCP 错误帧

格式：

```json
{ "from": 0, "to": <userId>, "type": 102, "content": "错误原因" }
```

常见 `content`：

| 错误文本 | 触发场景 |
| --- | --- |
| `invalid or expired token` | TCP 登录 JWT 无效/过期/类型错误 |
| `already logged in` | 重复发送登录帧 |
| `login first` | 未登录就发消息/拉历史 |
| `bad recipient` | 单聊接收者为 0 |
| `bad peer` | 单聊历史接收者为 0 |
| `bad group id` | 群消息/群历史 groupId 为 0 |
| `not friends` | 单聊发送者与接收者不是好友 |
| `blocked by recipient` | 被接收者拉黑 |
| `server busy` | 服务端过载/单连接在途任务超限 |
| `send failed` / `group send failed` | 落库失败等 |
| `history failed` | 拉历史失败 |
| 数据库业务错误 | 如群不存在、非群成员、无权限、成员已满等 |

### 6.2 重试原则

1. **HTTP 429/503**：按 `Retry-After` 等待 + 抖动；不要立即重试，不要把这类响应计入业务失败率。
2. **TCP `server busy`**：降速，并用**同一 `msg_id`** 重试；服务端按 `client_msg_id` 去重。
3. **连接被关闭**：抖动退避后重连；重连前先 `POST /api/token/refresh`，不要直接密码登录。
4. **消息是否已发送成功不确定**：用同一 `msg_id` 重发；若返回 `SendAck` 且 `content="duplicate"`，说明服务端已有该消息。
5. **登录后对齐**：好友/群成员等非持久化事件丢失时，主动拉全量列表。

### 6.3 客户端必须处理的协议边界

| 边界 | 客户端行为 |
| --- | --- |
| 单帧 body > 4096 字节 | 禁止发送；图片/文件先上传再发 URL |
| 未知 `type` | 禁止发送；服务端会关闭连接 |
| `Text/Image` 缺 `content` | 禁止发送；服务端判定非法并关闭连接 |
| `from`/`type` 缺失或类型错误 | 禁止发送；服务端关闭连接 |
| `HistoryResp.content` | 是字符串里的 JSON 数组，需要二次解析 |
| 事件 `content` | 是字符串里的 JSON 对象，需要二次解析 |
| `msg_id=0` | 不做去重；需要幂等重试时必须使用非 0 值 |
| 收到 `429/503` | 按 `Retry-After` 退避，不要计入业务失败率 |

---

## 7. 数据模型与枚举

### 7.1 数据库表

| 表 | 说明 |
| --- | --- |
| `users` | 用户账号、密码哈希、昵称、头像、状态 |
| `friends` | 好友关系（双向各一行，`status=1` 好友，`status=2` 拉黑） |
| `friend_requests` | 好友申请（单向，0=待处理，1=通过，2=拒绝，3=取消） |
| `conversations` | 单聊/群聊会话，`last_seq` 为会话内序号生成器 |
| `conversation_members` | 会话成员与投递/已读游标 |
| `messages` | 消息历史，服务端分配 `id/seq/created_at` |
| `offline_messages` | 离线消息队列（仅 `OFFLINE_CURSOR=0` 默认模式使用） |
| `groups` | 群资料，关联一个 `type=2` 会话 |
| `group_members` | 群成员与角色、群内昵称 |
| `group_requests` | 入群申请（带状态机与 `result_notified` 离线补偿） |
| `files` | 媒体文件登记（七牛直传的**控制面记录**，二进制不入库）：`uuid` 对外标识、`uploader_id` 权属、`object_key=bucket/zone` 存储定位、`size/qetag` **以七牛 stat 权威值登记**、`status` 1=正常 2=已删除 |

关键字段约束：

| 字段 | 约束 |
| --- | --- |
| `users.username` | `VARCHAR(64)`，全局唯一 |
| `users.status` | `1`=正常，`0`=禁用 |
| `friends.remark` | `VARCHAR(64)` |
| `friend_requests.message` | `VARCHAR(255)` |
| `messages.client_msg_id` | `VARCHAR(64)`，由客户端 `msg_id` 十进制字符串写入；`msg_id=0` 时不写 |
| `groups.name` | `VARCHAR(128)` |
| `groups.announcement` | `VARCHAR(512)` |
| `groups.intro` / `groups.avatar_url` | `VARCHAR(255)` |
| `groups.member_limit` | 默认 `200` |
| `group_members.role` | `1`=群主，`2`=管理员，`3`=普通成员 |
| `group_requests.message` | `VARCHAR(255)` |
| `files.uuid` | `CHAR(36)`，服务端生成的随机 v4，全局唯一（`uk_files_uuid`） |
| `files.object_key` | `VARCHAR(255)`，形如 `im/<yyyy-mm>/<uuid>.<ext>`；服务端生成并锁进上传凭证 `policy.scope`，客户端不可选 |
| `files.name` | `VARCHAR(255)`，仅展示用；消毒后入库（去控制字符 + UTF-8 边界安全截断） |
| `files.size` | 字节数，**取七牛 `stat` 返回值**登记（客户端申报值只用于签凭证前的闸门） |
| `files.qetag` | `CHAR(32)` 七牛校验和（urlsafe base64 形态）；与标准 sha256 不同算法，**不可互换核验** |
| `files.zone` | 七牛区域：0=z0 华东，1=z1 华北，2=z2 华南，3=na0 北美，4=as0 东南亚 |
| `files.status` | `1`=正常，`2`=已删除（软删：先标库、再物理删七牛对象，顺序刻意） |

### 7.2 业务枚举

好友关系 `friends.status`：

| 值 | 含义 |
| --- | --- |
| `0` | 保留 |
| `1` | 好友 |
| `2` | 拉黑 |

好友申请 / 入群申请 `status`：

| 值 | 含义 |
| --- | --- |
| `0` | 待处理 |
| `1` | 已通过 |
| `2` | 已拒绝 |
| `3` | 已取消（保留） |

会话 `conversations.type`：

| 值 | 含义 |
| --- | --- |
| `1` | 单聊 |
| `2` | 群聊 |

群状态 `groups.status`：

| 值 | 含义 |
| --- | --- |
| `0` | 正常 |
| `1` | 已解散 |

---

## 8. 限制与容量

### 8.1 协议层限制

| 项目 | 限制 |
| --- | --- |
| TCP 客户端上行单帧 body | `1 ~ 4096` 字节；0 或超限服务端直接断连 |
| TCP 服务端下行单帧 body | 不限制 4096；客户端必须按长度头读取（如 `HistoryResp` 可能较大） |
| HTTP 请求体 | 默认 `64KB`；超限 `413` 并关闭连接 |
| HTTP 单连接请求数 | 最多 `256` 个 |
| HTTP keep-alive 空闲 | `60s` |
| TCP 登录补推离线消息 | 单次最多 `100` 条 |
| 历史消息条数 | 接入层 `1 ~ 200`，DB 层实际最多 `100`；默认 20 |
| 好友/入群申请附言 | `<= 255` 字节 |
| 好友备注 | `<= 64` 字节 |
| 批量好友操作 | 单次最多 `200` 个目标 |
| 用户名 | `1 ~ 64` 字节 |
| 密码 | `6 ~ 72` 字节 |
| 群名 | `1 ~ 128` 字节 |
| 群公告 | `<= 512` 字节 |
| 群简介 / 群头像 URL | `<= 255` 字节 |
| 群初始成员 | 至少 1 个，含群主最多 200 个 |

### 8.2 服务端运行参数

| 环境变量 | 默认 | 说明 |
| --- | --- | --- |
| `DB_CONFIG` | `DBConfig.ini` | 数据库配置路径 |
| `JWT_CONFIG` | `JWTConfig.json` | JWT 配置路径 |
| `TCP_PORT` / `HTTP_PORT` | `8888` / `8080` | 监听端口 |
| `BCRYPT_COST` | `10` | bcrypt 成本因子 |
| `DB_WORKER_COUNT` | 核数×2（上限 8） | SQL worker 线程数 |
| `DB_POOL_SIZE` | 与 `DB_WORKER_COUNT` 相同 | 连接池上限 |
| `DB_HASH_WORKER_COUNT` | 核数 | bcrypt worker 线程数 |
| `DB_QUEUE_LIMIT` | `2000` | 单任务队列深度上限 |
| `RELATION_CACHE_TTL` | `10000` | 关系缓存毫秒数，0=关闭 |
| `RELATION_CACHE_MAX` | `20000` | 关系缓存条目上限 |
| `OFFLINE_CURSOR` | `0` | 1=使用游标补推离线消息 |
| `HTTP_MAX_INFLIGHT` | `512` | HTTP 在途响应上限 |
| `HTTP_BODY_MAX` | `65536` | HTTP 请求体上限 |
| `AUTH_RATE_PER_SEC` | `20` | 全局认证令牌速率 |
| `AUTH_RATE_BURST` | `40` | 全局令牌桶容量 |
| `AUTH_IP_PER_SEC` | `30` | 单 IP 令牌速率 |
| `TCP_MAX_SESSIONS` | `2000` | TCP 并发连接上限 |
| `TCP_MAX_OUTSTANDING` | `64` | 单连接在途 DB 任务上限 |
| `IDLE_TIMEOUT_SEC` | `60` | TCP 空闲踢人秒数，钳制 15~3600 |
| `QUIET_LOG` | 未设置 | 1=丢弃 stdout/stderr |
| `QINIU_ENABLED` | `0` | 文件模块：0=Mock 存储（不发外网），1=真实七牛（见 4.15） |
| `QINIU_CONFIG` | `QiniuConfig.json` | 七牛配置路径（AK/SK、bucket/zone、up_host/domain、rs_host、ca_bundle） |
| `FILE_MAX_SIZE_MB` | `20` | 单文件上限；同时写进上传凭证 `policy.fsizeLimit` 由七牛强制执行 |
| `FILE_URL_TTL_SEC` | `600` | 302 下载的签名直链时效（钳制 30~86400） |
| `FILE_USER_DAILY_QUOTA_MB` | `500` | 每人每日上传配额（按已 commit 字节累计），超限回 429 |
| `FILE_TYPE_STRICT` | `1` | 图片/非图片的 mime 与扩展名一致性校验 |
| `FILE_BASE_URL` | 跟随 `HTTP_PORT` 回环 | 消息元数据里换取入口的前缀（必须是客户端可达地址） |

### 8.3 写队列预算

- 单连接写队列上限：`512KB`。
- 进程级写队列总预算：`64MB`。
- 超过预算时服务端主动断开“当前要写入”的连接。
- 一次 `async_write` 最多合并 `32` 帧 / `64KB`，帧顺序不变。

> 注：README §11.5 曾写“单连接写队列超 4MB”，当前代码实际是
> **单连接 512KB + 进程级 64MB**（见 `session.h`）。

---

## 9. 客户端实现建议

### 9.1 推荐状态机

```text
未连接
  └─ TCP connect
       └─ 发送 Login(100) + access token
            ├─ 收到 LoginAck(101) → 已登录
            └─ 收到 Error(102)   → 若 token 过期，先 refresh，再重连
已登录
  ├─ 每 30s 发送 Heartbeat(3)
  ├─ 收发 Chat(1/2)、History(110/111)、Ack(112)、SendAck(113)
  ├─ 处理事件 130~144
  └─ 连接断开 → 抖动退避 → refresh token → 重新连接
```

### 9.2 必做事项

1. TCP 读取必须按“4 字节大端长度 + body”精确读取，处理粘包/半包。
2. 发送前检查单帧 body 不超过 4096 字节。
3. 处理 `HistoryResp` 与事件时，对 `content` 做二次 JSON 解析。
4. 为每条待发消息生成稳定、非 0 的 `msg_id`，重试时保持不变。
5. 处理 `429/503` 的 `Retry-After`，使用抖动退避。
6. TCP 登录使用 access token；重连优先 refresh，而不是密码登录。
7. 登录后主动拉取好友列表、群列表，弥补不持久化事件的缺失。
8. 收到 `server busy` 时降速，不要无限并发重试。

### 9.3 分帧示例（Python）

```python
import json
import socket
import struct

MAX_BODY = 4096


def send_frame(sock, obj):
    body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    if len(body) > MAX_BODY:
        raise ValueError("frame body exceeds 4096 bytes")
    sock.sendall(struct.pack("!I", len(body)) + body)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def recv_frame(sock):
    # 注意：4096 只约束客户端上行；服务端下行（如 HistoryResp）可能更大，
    # 这里按长度头读取，可自行设置一个防御性的宽松上限（例如 16MB）。
    length = struct.unpack("!I", recv_exact(sock, 4))[0]
    if length == 0 or length > 16 * 1024 * 1024:
        raise ValueError("invalid frame length")
    return json.loads(recv_exact(sock, length))
```

### 9.4 典型调用顺序

```text
1. POST /api/register                  （如无账号）
2. POST /api/login                     → token + refreshToken
3. TCP connect + Login(100, token)      → LoginAck(101)
4. 登录补推：离线消息 / 待处理申请 / 未通知结果
5. 心跳 Heartbeat(3)
6. 发送消息 Text(1) / Image(2)          → SendAck(113)
7. 历史 HistoryReq(110)                → HistoryResp(111)
8. 回执 Ack(112)
9. 断线：POST /api/token/refresh       → 新 token + 新 refreshToken
10. 重连并重复 3~8
```

---

## 10. 已知限制与注意事项

1. **refresh 无撤销表**：改密码/封号后 refresh 不能立即失效；需要后续引入 `jti` 黑名单或 `token_version`。
2. **登录补推上限 100 条**：超出部分没有下行“还有更多”信令，只能等下次登录或客户端主动拉历史。
3. **关系缓存是进程内缓存**：多实例部署时，实例之间的缓存失效不会广播，窗口最长等于 `RELATION_CACHE_TTL`。
4. **群成员/资料/解散事件不持久化**：客户端重连后必须拉群列表/群详情对齐。
5. **好友列表变化事件不持久化**：客户端重连后必须拉好友列表对齐。
6. **多端登录**：同一账号重复登录会踢掉旧 TCP 连接；离线游标模式（`OFFLINE_CURSOR=1`）下，游标是账号级的，一端 ACK 会影响另一端未读。
7. **生产部署**：必须在 HTTP 前置 HTTPS、TCP 前置 TLS，并补充限流、防重放、日志脱敏等能力。
8. **消息正文不要放二进制**：图片/文件先直传对象存储拿 `file_id`，`content` 只放媒体元数据（见 4.15/5.8）。

---

## 11. 代码索引

| 主题 | 文件 |
| --- | --- |
| TCP 分帧 | `step1_tcp_echo/frame.h` |
| 消息类型与 Message 结构 | `step1_tcp_echo/message.h` |
| JSON 编解码、历史序列化 | `step1_tcp_echo/message_codec.h` |
| 好友事件 JSON | `step1_tcp_echo/friend_types.h` |
| 群事件 JSON | `step1_tcp_echo/group_types.h` |
| TCP 连接、登录、消息、补推、回执 | `step1_tcp_echo/session.cpp` / `session.h` |
| HTTP 路由与响应构造 | `step1_tcp_echo/http_server.cpp` |
| JWT 签发/校验 | `step1_tcp_echo/token_service.cpp` / `token_service.h` |
| TCP 连接数上限 | `step1_tcp_echo/server.cpp` |
| 数据库 DAO 与业务错误文本 | `step1_tcp_echo/db.cpp` / `db.h` |
| 建表语句与字段约束 | `db/schema.sql` |
| 原始说明与性能章节 | `readme.md` |
