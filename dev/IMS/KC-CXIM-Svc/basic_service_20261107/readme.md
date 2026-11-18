# KC-CXIM IMS 服务端（basic_service_20261107）

基于 C++17 + Boost.Asio / Boost.Beast + MariaDB 的即时通讯服务端示例。

- **HTTP 短连接**：注册、登录、用户搜索、好友关系管理等请求-响应型业务。
- **TCP 长连接**：登录鉴权、心跳、单聊消息、历史消息、回执、好友事件实时推送。
- **数据库**：MariaDB，表结构见 [`db/schema.sql`](db/schema.sql)。

> 设计原则：好友关系的事实变更走 HTTP，变更后的实时事件走 TCP 推送。
> HTTP 负责“写事实”，TCP 负责“推事件”。

---

## 1. 目录结构

```text
basic_service_20261107/
├── step1_tcp_echo/       # 服务端：TCP 长连接 + HTTP 短连接 + DB 访问层
│   ├── main.cpp          # 程序入口：加载配置、启动 TCP/HTTP 监听
│   ├── server.*          # TCP 监听与 accept
│   ├── session.*         # 单条 TCP 连接：分帧、解析、业务、事件推送
│   ├── session_manager.* # 在线用户表 userId -> session
│   ├── http_server.*     # HTTP API：注册/登录/好友模块
│   ├── db.*              # 连接池 + DB worker + DAO
│   ├── message.h         # TCP 消息类型与 Message 结构体
│   ├── message_codec.h   # Message <-> JSON 编解码
│   ├── friend_types.h    # 好友领域模型与事件 JSON
│   ├── group_types.h     # 群聊领域模型与事件 JSON（建群/成员/申请）
│   ├── token_service.*   # JWT（HS256）签发/校验
│   └── frame.h           # 4 字节大端长度头分帧
├── step2_test_client/    # 测试客户端
│   ├── test_client.cpp           # 基础聊天/离线/历史/重连验收
│   ├── test_friend_client.cpp    # 好友模块 HTTP + TCP 事件验收
│   └── test_group_client.cpp     # 群聊模块 HTTP + TCP 事件/消息验收
├── step3_verify/         # 验证脚本与测试数据
│   ├── verify_tcp.sh
│   ├── verify_tcp_http.sh
│   ├── verify_friend.sh
│   ├── verify_group.sh
│   └── seed_test_data.sql
├── db/schema.sql         # 数据库建表语句
├── DBConfig.ini          # 数据库连接配置
├── JWTConfig.json        # JWT 密钥/签发者/有效期
└── build.sh         # 构建脚本
```

---

## 2. 构建与运行

### 2.1 构建

Windows（MSYS/MinGW）或 Linux：

```bash
cd dev/IMS/KC-CXIM-Svc/basic_service_20261107
bash build.sh
```

构建类型（性能优化 P0-5）：`build.sh` 现在默认 **Release**。

- 以前顶层 `CMakeLists.txt` 完全没有处理 `CMAKE_BUILD_TYPE`，Linux（Unix Makefiles，单配置）会拿到空的 `CXX_FLAGS`，等于 **-O0 跑在生产机上**；Windows（Visual Studio，多配置）`cmake --build build` 不带 `--config` 时默认编 **Debug(/Od)**。
- 现在 `build.sh` 传 `-DCMAKE_BUILD_TYPE=Release`，Windows 侧再显式 `--config Release`；`CMakeLists.txt` 里也加了"未指定时默认 Release"的兜底。
- 要调试照常覆盖：`CMAKE_BUILD_TYPE=Debug bash build.sh`（或 `cmake --build build --config Debug`）。
- `third-party/libbcrypt` 子工程自带 `-O3`，不受本次改动影响。

构建产物：

```text
build/bin/Release/cxim-server.exe   # Windows（默认 Release；Debug 仍在 build/bin/Debug/）
build/bin/cxim-server               # Linux（单配置，直接落在 bin/）
```

> 压测请用 Release 产物：`build/bin/Release/test_stress_03.exe`。
> 用 Debug 版客户端测服务端会同时污染两端——MSVC Debug 的 `/Od` + 安全迭代器会让
> 客户端自己那部分耗时膨胀几十倍，而延迟是从客户端计时算出来的。

### 2.2 启动

```bash
DB_CONFIG=./DBConfig.ini JWT_CONFIG=./JWTConfig.json \
HTTP_PORT=8080 TCP_PORT=8888 \
DB_WORKER_COUNT=4 DB_POOL_SIZE=4 DB_HASH_WORKER_COUNT=2 BCRYPT_COST=10 \
QUIET_LOG=0 ./build/bin/Release/cxim-server
```

全部可调环境变量（默认值按 **2 核 / 2GB / 3Mbps + 同机 mariadbd** 这一档给，见 §9）：

| 变量 | 默认 | 含义 |
|------|------|------|
| `DB_CONFIG` / `JWT_CONFIG` | `DBConfig.ini` / `JWTConfig.json` | 配置文件路径 |
| `TCP_PORT` / `HTTP_PORT` | `8888` / `8080` | 监听端口 |
| `BCRYPT_COST` | `10` | bcrypt 成本因子（P0-1）。每 +1 算力翻倍；旧哈希串自带 cost 前缀，改这个值**不影响老用户校验** |
| `DB_WORKER_COUNT` | `核数*2`（上限 8） | SQL worker 线程数（P0-2） |
| `DB_POOL_SIZE` | `= DB_WORKER_COUNT` | 连接池上限。并发连接数不可能超过 SQL worker，配大了只是误导（P0-7） |
| `DB_HASH_WORKER_COUNT` | `核数` | bcrypt worker 线程数（P0-1/P0-2）。超订不提高吞吐，只会放大所有接口的延迟 |
| `DB_QUEUE_LIMIT` | `2000` | 单条任务队列深度上限（P0-4） |
| `RELATION_CACHE_TTL` | `10000` | 发消息前置关系校验的缓存毫秒数，`0`=每次都查库（P1-1） |
| `RELATION_CACHE_MAX` | `20000` | 关系缓存条目上限（LRU 淘汰） |
| `OFFLINE_CURSOR` | `0` | `1`=用游标补推离线消息，不再写 `offline_messages` 行级队列（P1-4，语义差异见 §9.4） |
| `HTTP_MAX_INFLIGHT` | `512` | 同时在途 HTTP 请求上限，超过回 `503 + Retry-After`（P0-4） |
| `HTTP_BODY_MAX` | `65536` | 请求体字节上限，超过回 `413`（P0-4） |
| `AUTH_RATE_PER_SEC` | `20` | 注册/登录/刷新的全局令牌补充速率（P0-4）。这是**机器容量闸门** |
| `AUTH_RATE_BURST` | `40` | 上述全局桶容量（允许的突发） |
| `AUTH_IP_PER_SEC` | `30` | 单 IP 每秒令牌（反滥用）。刻意**宽于**全局值：反过来配会让共享出口 IP 的真实用户与单机压测在服务器空闲时被大面积 429 |
| `AUTH_IP_PER_SEC` | `5` | 单 IP 每秒令牌，挡住单机刷注册/密码爆破 |
| `TCP_MAX_SESSIONS` | `2000` | TCP 并发连接上限，超过直接关闭新连接（P0-4） |
| `TCP_MAX_OUTSTANDING` | `64` | 单条 TCP 连接未完成的 DB 任务上限（P0-4） |
| `IDLE_TIMEOUT_SEC` | `60` | 长连接空闲踢人秒数，**必须大于客户端心跳间隔** |
| `QUIET_LOG` | 未设置=打印 | `1` 丢弃 stdout/stderr（生产建议开；每条消息都 `std::endl` 刷 journald 会拖慢 io 线程） |

启动成功后会看到类似输出：

```text
[db] connecting to 127.0.0.1:3306/CXIMDB as cximUser
[db] pool max=4, sqlWorkers=4, hashWorkers=2, bcryptCost=10, queueLimit=2000,
     offlineCursor=0, relationCacheTtlMs=10000 (connections are created lazily)
[server] listening on port 8888
[http] listening on port 8080
[server] io threads=2
```

### 2.3 初始化数据库

```bash
# 使用项目自带的 seed_db（推荐，不依赖本机 mysql 客户端）
./build/bin/Release/seed_db.exe DBConfig.ini db/schema.sql     # Windows

# 或者
mysql -h <host> -P <port> -u <user> -p <dbname> < db/schema.sql
```

---

## 3. 协议总览

### 3.1 HTTP 连接（P1-6：支持 keep-alive）

- 默认端口：`8080`
- 请求/响应体：UTF-8 JSON
- `Content-Type: application/json`
- **默认复用连接**：客户端 HTTP/1.1 且未声明 `Connection: close` 时，服务端写完响应会继续
  在同一连接上读下一个请求；空闲 `60s` 或单连接处理满 `256` 个请求后关闭。
  显式发 `Connection: close` 的行为与以前一致（一问一答即关）。
  为什么要改：原来每请求一个连接且由服务端主动 close，44 req/s 的稳态就能堆出 ~2600 个
  `TIME_WAIT`，fd/端口持续 churn；跨公网还要每个请求重新握手 + 慢启动。
- 请求体上限 `64KB`（`HTTP_BODY_MAX`），超过返回 `413`；在途请求超 `HTTP_MAX_INFLIGHT`
  返回 `503 + Retry-After`（P0-4）。
- 除注册/登录/刷新外，其余接口都需要：

```http
Authorization: Bearer <JWT>
```

JWT 通过 `POST /api/login` 或 `POST /api/token/refresh` 获取（后者不校验密码，见 §4.3）。

### 3.2 TCP 长连接

- 默认端口：`8888`
- 每一帧：`4 字节大端长度（网络序） + JSON body`
- body 是 `Message` 的 JSON 序列化结果
- 空闲超过 `IDLE_TIMEOUT_SEC`（默认 60）秒会被服务端踢掉，**客户端心跳必须显著小于它**
- 同一账号重复登录会踢掉旧连接
- 连接上已设置 `TCP_NODELAY`（P1-5）与 `SO_KEEPALIVE`；下行帧在同一个 `async_write` 里
  最多合并 32 帧 / 64KB（P1-8），帧顺序不变
- 并发连接数超 `TCP_MAX_SESSIONS`（默认 2000）时，新连接被立刻关闭（P0-4）
- 单条连接在途 DB 任务超 `TCP_MAX_OUTSTANDING`（默认 64）时，该请求回
  `Error("server busy")`，客户端应降速重试（P0-4）

```text
+----------------+-----------------------------+
| 4 字节长度 N   | N 字节 JSON body            |
| big-endian     | Message 序列化              |
+----------------+-----------------------------+
```

---

## 4. 鉴权流程

### 4.1 HTTP 登录获取 JWT

```bash
curl -X POST http://127.0.0.1:8080/api/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'
```

响应（P0-3：登录同时签发 access + refresh 两个 token）：

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

- `token` 就是原来的字段，行为不变；老客户端忽略新增字段即可，接口向后兼容。
- `refreshToken` **只能**用来调 `POST /api/token/refresh`；拿它当 Bearer 或 TCP 登录凭证会被拒绝。
- 为什么要加：`/api/login` 每次都要做一次 bcrypt 校验（本机 cost=12 实测 ~138ms CPU/次），
  2 核机器上 100 个并发登录排空要 14 秒。重连改走 refresh 后服务端只做 HMAC 验签（微秒级），
  登录风暴的成本几乎归零。详见 §4.3 与 §9.1。

后续 HTTP 请求带：

```http
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...
```

### 4.2 TCP 登录

建立 TCP 连接后，先发送 `type=100` 的登录消息，`content` 放 JWT：

```json
{
  "from": 0,
  "to": 0,
  "type": 100,
  "msg_id": 0,
  "ts": 0,
  "conversation_id": 0,
  "seq": 0,
  "content": "eyJhbGciOiJIUzI1NiIs..."
}
```

服务端以 JWT 的 `sub` 作为真实 userId，**不信任客户端传来的 `from`**。

登录成功：

```json
{
  "from": 0,
  "to": 1001,
  "type": 101,
  "msg_id": 0,
  "ts": 0,
  "conversation_id": 0,
  "seq": 0,
  "content": "welcome"
}
```

登录失败：

```json
{
  "from": 0,
  "to": 0,
  "type": 102,
  "msg_id": 0,
  "ts": 0,
  "conversation_id": 0,
  "seq": 0,
  "content": "invalid or expired token"
}
```

### 4.3 断线重连：刷新 access token（P0-3）

TCP 登录本身**不查库、不算哈希**，只验 JWT 签名；但 JWT 只能由 `/api/login` 签发，
而 `/api/login` 每次都要做一次 bcrypt 校验密码。于是"网络抖动后大家一起重连"就会
退变成"大家一起做重 CPU 哈希"，把整台 2 核机器占满——这正是压测里登录 13 秒的根因。

正确的重连姿势是用 refresh token 换 access token：

```bash
curl -X POST http://127.0.0.1:8080/api/token/refresh \
  -H "Content-Type: application/json" \
  -d '{"refreshToken":"eyJhbGciOiJIUzI1NiIs..."}'
```

响应与 `/api/login` 同构（`token` + 轮换后的新 `refreshToken` + `expiresIn`），
成本是**一次 HMAC 验签 + 两次 HMAC 签发**：不查库、不算 bcrypt、不占用 Db 的任何 worker。

推荐客户端策略：

1. 保存 `token` 与 `refreshToken`，在 `token` 到期前（或收到 TCP `invalid or expired token` 时）
   先调 `/api/token/refresh`；
2. 只有 refresh 也失效（过期/被撤销）才回落到 `/api/login` 用密码重新登录；
3. 重连一律带**抖动退避**（如 `min(60s, 1s × 2^n) + rand(0,1s)`），把"同时重连"打散成"错峰重连"；
4. 收到 `429`（限流）或 `503`（过载）时按 `Retry-After` 等待，不要立刻重试。

**安全边界（务必知晓）**：refresh token 是无状态 JWT，服务端**没有撤销表**，
所以"改密码/封号后立即让所有 refresh 失效"目前做不到；有效期默认 30 天，
且每次刷新都轮换（旧 refresh 在自己的 exp 前仍可用）。要闭合这个缺口，需要加一张
`jti` 黑名单/版本号表（`users.token_version` 或独立表）并在 `verifyRefresh` 里比对——
这是刻意留下的、需要产品与安全一起决策的一项，不是遗漏。

---

## 5. HTTP API

### 5.0 通用约定

成功响应：

```json
{ "code": 0, "message": "ok", "data": {} }
```

失败响应：

```json
{ "code": 1, "message": "错误原因" }
```

常见 HTTP 状态码：

| 状态码 | 含义 |
| --- | --- |
| 200 | 成功 |
| 400 | 参数错误 / 业务规则拒绝 |
| 401 | 未带 token / token 无效或过期 / 账号密码错误 |
| 404 | 路径不存在 |
| 405 | HTTP 方法不允许 |
| 413 | 请求体超过 `HTTP_BODY_MAX`（默认 64KB），随后连接被关闭（P0-4） |
| 429 | 注册/登录/刷新超过限流速率，带 `Retry-After`，客户端必须退避（P0-4） |
| 503 | 服务端过载（在途请求或任务队列超阈值），带 `Retry-After`（P0-4） |

过载语义是这次新增的约定，值得单独说明：

- `message` 为 `"server busy"` 时，HTTP 一定回 `503 + Retry-After`；
  TCP 侧则回一条 `type=102` 的 `Error` 帧，`content` 同样是 `"server busy"`。
- `HTTP_MAX_INFLIGHT` 数的是**正在处理的请求**（以响应为单位：进 `send_json` +1、写完 -1），
  不是"打开着的连接数"。这个区别很关键：连接寿命由 keep-alive 空闲计时器决定（最长 60s），
  拿它当在途量等于把"最近 60 秒内开过的所有连接"都算进去——注册风暴里几百个一次性连接会
  在服务器其实空闲时把计数顶过阈值，然后**每个新连接在解析第一个请求之前就被 503 关掉**，
  自己造出自己的 503 风暴。
- HTTP 准入同时看 SQL 队列深度（`Db::sqlQueueDepth()`）：仍有一批 DAO 走"软上限、不丢任务"
  的投递口（原因见 §11.5），只看响应数挡不住它们把队列堆深。
- 设计取舍是"过载时拒掉一部分、让客户端退避"，而不是"全部收下然后一起慢到超时"。
  后者在 2 核机器上的表现就是压测里看到的：一个注册风暴让所有在线用户的
  发消息、查好友列表全部涨到秒级。
- 因此客户端要正确处理 `429/503`：按 `Retry-After` 等待 + 抖动退避，
  并把"这类响应不计入业务失败率"（否则限流会被误报成服务异常）。

---

### 5.1 注册

```http
POST /api/register
```

请求体：

```json
{
  "username": "alice",
  "password": "secret123"
}
```

约束：

- `username` 非空，长度 <= 64
- `password` 长度 6 ~ 72
- 用户名全局唯一

示例：

```bash
curl -X POST http://127.0.0.1:8080/api/register \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'
```

响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": { "userId": 1001 }
}
```

---

### 5.2 登录

```http
POST /api/login
```

请求体：

```json
{
  "username": "alice",
  "password": "secret123"
}
```

示例：

```bash
curl -X POST http://127.0.0.1:8080/api/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'
```

响应（P0-3：登录同时签发 access + refresh 两个 token）：

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

- `token` 就是原来的字段，行为不变；老客户端忽略新增字段即可，接口向后兼容。
- `refreshToken` **只能**用来调 `POST /api/token/refresh`；拿它当 Bearer 或 TCP 登录凭证会被拒绝。
- 为什么要加：`/api/login` 每次都要做一次 bcrypt 校验（本机 cost=12 实测 ~138ms CPU/次），
  2 核机器上 100 个并发登录排空要 14 秒。重连改走 refresh 后服务端只做 HMAC 验签（微秒级），
  登录风暴的成本几乎归零。详见 §4.3 与 §9.1。

---

### 5.3 搜索用户（BE-FRIEND-01）

```http
GET /api/users/search?keyword=<关键字>&limit=<条数>
```

需要鉴权。

参数：

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `keyword` | 是 | 匹配用户名、昵称，或精确用户 ID |
| `limit` | 否 | 默认 20，最大 50 |

示例：

```bash
curl "http://127.0.0.1:8080/api/users/search?keyword=bob&limit=20" \
  -H "Authorization: Bearer $TOKEN"
```

响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "users": [
      {
        "userId": 1002,
        "username": "bob",
        "nickname": "Bob",
        "avatarUrl": ""
      }
    ]
  }
}
```

说明：

- 只返回公开字段，不会返回 `password_hash`、token 等敏感信息。
- 搜索结果会排除自己。

---

### 5.4 好友列表（BE-FRIEND-05）

```http
GET /api/friends
```

需要鉴权。

示例：

```bash
curl http://127.0.0.1:8080/api/friends \
  -H "Authorization: Bearer $TOKEN"
```

响应：

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
| `status` | 1=好友（2=拉黑不会出现在列表中） |

---

### 5.5 好友申请列表（BE-FRIEND-05）

```http
GET /api/friends/requests?direction=incoming|outgoing|all
```

需要鉴权。

| direction | 说明 |
| --- | --- |
| `incoming` | 我收到的申请（默认） |
| `outgoing` | 我发出的申请 |
| `all` | 收发的申请都返回 |

示例：

```bash
curl "http://127.0.0.1:8080/api/friends/requests?direction=incoming" \
  -H "Authorization: Bearer $TOKEN"
```

响应：

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
| 0 | 待处理 |
| 1 | 已通过 |
| 2 | 已拒绝 |
| 3 | 已取消（保留） |

---

### 5.6 发送好友申请（BE-FRIEND-03）

```http
POST /api/friends/requests
```

需要鉴权。

请求体：

```json
{
  "toUserId": 1002,
  "message": "你好，我是 Alice"
}
```

`message` 可选，最大 255 字节。

示例：

```bash
curl -X POST http://127.0.0.1:8080/api/friends/requests \
  -H "Authorization: Bearer $TOKEN_A" \
  -H "Content-Type: application/json" \
  -d '{"toUserId":1002,"message":"你好，我是 Alice"}'
```

响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "request": {
      "requestId": 10,
      "requesterId": 1001,
      "targetId": 1002,
      "requesterUsername": "alice",
      "requesterNickname": "Alice",
      "targetUsername": "bob",
      "targetNickname": "Bob",
      "message": "你好，我是 Alice",
      "status": 0,
      "createdTs": 1760000000000
    }
  }
}
```

业务校验：

- 不能加自己
- 目标用户必须存在且正常
- 不能重复申请
- 已是好友 / 被对方拉黑 / 对方已向你发送待处理申请时会拒绝

如果对方在线，会通过 TCP 实时收到 `FriendRequest=130` 事件。

---

### 5.7 通过好友申请（BE-FRIEND-04）

```http
POST /api/friends/requests/{requestId}/accept
```

需要鉴权，且只能由被申请人本人操作。

示例：

```bash
curl -X POST http://127.0.0.1:8080/api/friends/requests/10/accept \
  -H "Authorization: Bearer $TOKEN_B"
```

响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "request": {
      "requestId": 10,
      "requesterId": 1001,
      "targetId": 1002,
      "requesterUsername": "alice",
      "requesterNickname": "Alice",
      "targetUsername": "bob",
      "targetNickname": "Bob",
      "message": "你好，我是 Alice",
      "status": 1,
      "createdTs": 1760000000000
    }
  }
}
```

通过后：

- 数据库写入双向好友关系 `(A,B)` 与 `(B,A)`
- 申请人收到 `FriendRequestResult=131`，`event="accepted"`
- 双方在线时收到 `FriendListChanged=132`，`action="added"`

---

### 5.8 拒绝好友申请（BE-FRIEND-04）

```http
POST /api/friends/requests/{requestId}/reject
```

示例：

```bash
curl -X POST http://127.0.0.1:8080/api/friends/requests/10/reject \
  -H "Authorization: Bearer $TOKEN_B"
```

响应结构同“通过申请”，`status` 为 `2`。

申请人会收到 `FriendRequestResult=131`，`event="rejected"`。

---

### 5.9 修改好友备注（BE-FRIEND-05）

```http
PATCH /api/friends/{friendId}
```

需要鉴权。

请求体：

```json
{ "remark": "B-remark" }
```

`remark` 最大 64 字节。

示例：

```bash
curl -X PATCH http://127.0.0.1:8080/api/friends/1002 \
  -H "Authorization: Bearer $TOKEN_A" \
  -H "Content-Type: application/json" \
  -d '{"remark":"B-remark"}'
```

响应：

```json
{ "code": 0, "message": "ok" }
```

备注是私有的，只影响自己的好友列表。

---

### 5.10 删除好友（BE-FRIEND-05）

```http
DELETE /api/friends/{friendId}
```

需要鉴权。

示例：

```bash
curl -X DELETE http://127.0.0.1:8080/api/friends/1002 \
  -H "Authorization: Bearer $TOKEN_A"
```

响应：

```json
{ "code": 0, "message": "ok" }
```

删除会同时删除双向关系，并清理双方待处理申请。
双方在线时会收到 `FriendListChanged=132`，`action="deleted"`。

---

### 5.11 拉黑好友（BE-FRIEND-05）

```http
POST /api/friends/{friendId}/block
```

示例：

```bash
curl -X POST http://127.0.0.1:8080/api/friends/1002/block \
  -H "Authorization: Bearer $TOKEN_A"
```

响应：

```json
{ "code": 0, "message": "ok" }
```

拉黑后：

- 删除双方好友关系
- 写入 `(A,B,status=2)` 的单向拉黑行
- 清理双方好友申请

---

### 5.12 取消拉黑（BE-FRIEND-05）

```http
POST /api/friends/{friendId}/unblock
```

示例：

```bash
curl -X POST http://127.0.0.1:8080/api/friends/1002/unblock \
  -H "Authorization: Bearer $TOKEN_A"
```

取消拉黑只删除自己的拉黑记录，不会自动恢复好友关系；需要重新申请。

---

### 5.13 群聊模块（BE-GROUP-01 ~ 08）

> 群聊沿用“HTTP 写事实、TCP 推事件”的原则：建群/邀请/移出/任命/转让/解散/改资料/审批入群
> 都走 HTTP；变更后的实时事件走 TCP（type=140~144，见 §6.12）。
> 群消息本体走 TCP（复用文本/图片类型 + `is_group=true`，见 §6.13）。

角色定义：`role` 1=群主，2=管理员，3=普通成员。权限矩阵：

| 操作 | 群主(1) | 管理员(2) | 成员(3) |
| --- | --- | --- | --- |
| 发言 / 拉历史 / 回执 | ✓ | ✓ | ✓ |
| 改群资料 | ✓ | ✓ | ✗ |
| 邀请成员 | ✓ | ✓ | ✗ |
| 移出普通成员 | ✓ | ✓（仅移成员） | ✗ |
| 任命/取消管理员 | ✓ | ✗ | ✗ |
| 转让群主 / 解散 | ✓ | ✗ | ✗ |
| 退群 | 需先转让 | ✓ | ✓ |

所有接口需 `Authorization: Bearer <JWT>`；`groupId`/`userId` 均为数字。

#### 5.13.1 建群

```http
POST /api/groups
```

请求体：

```json
{ "name": "项目讨论组", "memberIds": [1002, 1003] }
```

- `name` 非空且 ≤128 字节；`memberIds` 至少 1 个。
- MVP 约束：初始成员必须是发起人（自动成为群主）的**好友**。

```bash
curl -s -X POST http://127.0.0.1:8080/api/groups \
  -H "Authorization: Bearer $TOKEN_A" -H "Content-Type: application/json" \
  -d '{"name":"项目讨论组","memberIds":[1002,1003]}'
```

响应：

```json
{ "code": 0, "message": "ok",
  "data": { "group": { "groupId": 1, "conversationId": 5, "name": "项目讨论组",
    "ownerId": 1001, "status": 0, "memberLimit": 200, "memberCount": 3,
    "myRole": 1, "createdTs": 1760000000000 } } }
```

建群成功后，除发起人外的每个新成员都会收到 TCP `GroupMemberChanged(added)`。

#### 5.13.2 我的群列表 / 群详情

```http
GET  /api/groups
GET  /api/groups/{groupId}
```

- 列表只返回 `status=0`（未解散）且我是成员的群。
- 详情返回 `group` + `members[]`，必须是成员，否则 400 `not a group member`。

```bash
curl -s http://127.0.0.1:8080/api/groups -H "Authorization: Bearer $TOKEN_A"
curl -s http://127.0.0.1:8080/api/groups/1 -H "Authorization: Bearer $TOKEN_A"
```

`members[]` 每项：`userId / username / nickname / avatarUrl / groupNickname / role / joinedTs`。

#### 5.13.3 修改群资料

```http
PATCH /api/groups/{groupId}
```

请求体（只带要改的字段，空串=不改该字段；仅群主/管理员）：

```json
{ "name": "新群名", "announcement": "周五例会", "intro": "", "avatarUrl": "" }
```

成功后全群收到 TCP `GroupInfoChanged(142)`。

#### 5.13.4 邀请 / 移出成员

```http
POST   /api/groups/{groupId}/members           # 邀请（body: {"memberIds":[1004]}）
DELETE /api/groups/{groupId}/members/{userId}  # 移出（仅群主/管理员，且只能移普通成员）
```

- 邀请：已在群里的 id 静默跳过（幂等）；人数受 `memberLimit` 限制；被邀请人此前对该群的待处理申请会自动置为“已通过”。
- 移出后即时生效：被移者收 TCP `GroupMemberChanged(removed)`，且**再发群消息/拉历史会被拒**（服务端按成员表校验，成员行已删）。

#### 5.13.5 任命 / 取消管理员

```http
POST /api/groups/{groupId}/admins
```

```json
{ "userId": 1002, "admin": true }
```

仅群主可操作；`admin=true` 把普通成员升为管理员，`false` 降回。全群收到 `GroupMemberChanged(roleChanged)`。

#### 5.13.6 转让群主 / 退群 / 解散

```http
POST /api/groups/{groupId}/transfer   # body: {"toUserId":1002}，仅群主；旧群主降为管理员
POST /api/groups/{groupId}/quit        # 退群；群主须先转让，否则 400
POST /api/groups/{groupId}/dismiss     # 解散，仅群主；软删 status=1，清成员/游标/离线队列
```

- 转让：全群收到 `GroupMemberChanged(transferred)`（新群主）+ `roleChanged`（旧群主→管理员）。
- 退群：剩余成员收到 `GroupMemberChanged(quit)`。
- 解散：解散前成员收到 `GroupDismissed(141)`；此后该群任何人再收发/拉历史均被拒。

#### 5.13.7 入群申请与审批

```http
POST /api/groups/requests                              # 申请入群 body: {"groupId":1,"message":"拉我一把"}
GET  /api/groups/requests?direction=incoming|outgoing|all
POST /api/groups/requests/{requestId}/accept           # 群主/管理员通过
POST /api/groups/requests/{requestId}/reject           # 群主/管理员拒绝
```

- `incoming`=需要我审批的（我是群主/管理员的群的待处理申请）；`outgoing`=我发出的。
- 申请成功后实时推 `GroupRequest(143)` 给群主；离线群主/管理员在 TCP 登录时补推（见 §6.12）。
- 审批后推 `GroupRequestResult(144)` 给申请人（离线则登录补推）；通过时全群再收 `GroupMemberChanged(added)`。

---

### 5.14 刷新 access token（性能优化 P0-3）

```http
POST /api/token/refresh
```

请求体：

```json
{ "refreshToken": "eyJhbGciOiJIUzI1NiIs..." }
```

响应：与 `POST /api/login` 同构（`token` + 轮换后的新 `refreshToken` + `expiresIn`）。

- 无需 `Authorization`；但**只接受 `typ=refresh` 的 token**，拿 access token 来刷新会回 401。
- 服务端成本：一次 HMAC-SHA256 验签 + 两次 HMAC 签发。**不查库、不做 bcrypt、不占用 Db worker**，
  因此这是重连路径唯一应该使用的接口（详见 §4.3 的客户端策略）。
- 失败：`400 missing refreshToken` / `401 invalid or expired refresh token` / `429`（限流）。

### 5.15 批量发送好友申请（性能优化 P1-7）

```http
POST /api/friends/requests/batch
```

请求体（单次最多 200 个目标）：

```json
{ "toUserIds": [1002, 1003, 1004], "message": "我是小李" }
```

响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "requests": [
      { "requestId": 88, "requesterId": 1001, "targetId": 1003,
        "targetUsername": "carol", "targetNickname": "Carol",
        "message": "我是小李", "status": 0, "createdAt": 1730000000000 }
    ],
    "skipped": [
      { "toUserId": 1002, "reason": "already friends" },
      { "toUserId": 1004, "reason": "blocked by target user" }
    ]
  }
}
```

- 为什么要有批量接口：单条接口发 100 个申请 = 100 个事务 + 100 次连接借还，而且这些事务
  全都在同一个 `requester_id` 的索引区间上抢锁（`fixme.md` 分析过的 gap lock 风暴就是这个形状）。
  批量把它压成**一个事务 + 一次多行 INSERT**，并把加锁顺序固定为按目标 id 升序。
- 与单条接口的语义差异：批量是**尽力而为**——不满足条件的目标进 `skipped`（原因文案与单条接口
  完全一致），不会让整批失败。需要"要么全成要么报错"的调用方请继续用 §5.6。
- 事件推送与单条一致：每个成功项实时推 `FriendRequest(130)` 给被申请人，离线者登录时补推。

### 5.16 批量删除好友（性能优化 P1-7）

```http
POST /api/friends/batch-delete
```

请求体：

```json
{ "friendIds": [1002, 1003, 1004] }
```

响应：

```json
{ "code": 0, "message": "ok", "data": { "requested": 3, "deleted": 3 } }
```

- 一个事务内两条多行 `DELETE`（`friends` + `friend_requests`），语义与 §5.10 单条一致：双向删除、
  顺带清理双方待处理申请。
- 单条删除的实现没变；批量接口只是把 N 次事务压成 1 次，因此 **P1-1 的关系缓存会逐条失效**。

---

## 6. TCP 接口

### 6.1 Message 字段

每条 TCP 消息都是一个 JSON 对象：

```json
{
  "from": 1001,
  "to": 1002,
  "type": 1,
  "msg_id": 123,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "hello"
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `from` | uint64 | 发送者 userId；服务端下行时以会话身份为准 |
| `to` | uint64 | 接收者 userId |
| `type` | int | 消息类型，见下表 |
| `msg_id` | uint64 | 客户端发送时是临时消息号，服务端下行时是服务端消息 ID |
| `ts` | uint64 | 毫秒时间戳，服务端消息由服务端生成 |
| `conversation_id` | uint64 | 会话 ID |
| `seq` | uint64 | 会话内单调序号；历史请求时表示 `before_seq` |
| `content` | string | 文本、URL、token 或 JSON 字符串 |
| `is_group` | bool | /*************群聊功能***************/：`true` 表示 `to` 为 groupId、走群聊逻辑；缺省/`false`=单聊（旧客户端不发送该字段即按单聊处理，向后兼容）|

### 6.2 消息类型

| 值 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| 1 | Text | 双向 | 文本消息 |
| 2 | Image | 双向 | 图片消息，`content` 建议放 URL |
| 3 | Heartbeat | 客户端 -> 服务端 | 心跳，刷新空闲计时 |
| 100 | Login | 客户端 -> 服务端 | 登录，`content` 放 JWT |
| 101 | LoginAck | 服务端 -> 客户端 | 登录成功 |
| 102 | Error | 服务端 -> 客户端 | 错误通知 |
| 110 | HistoryReq | 客户端 -> 服务端 | 历史消息请求 |
| 111 | HistoryResp | 服务端 -> 客户端 | 历史消息响应 |
| 112 | Ack | 客户端 -> 服务端 | 投递/已读回执 |
| 113 | SendAck | 服务端 -> 客户端 | 发送成功确认 |
| 130 | FriendRequest | 服务端 -> 客户端 | 收到好友申请 / 登录补推 |
| 131 | FriendRequestResult | 服务端 -> 客户端 | 申请通过或拒绝 |
| 132 | FriendListChanged | 服务端 -> 客户端 | 好友列表变化 |
| 140 | GroupMemberChanged | 服务端 -> 客户端 | 群成员变化：added/removed/quit/roleChanged/transferred |
| 141 | GroupDismissed | 服务端 -> 客户端 | 群被解散 |
| 142 | GroupInfoChanged | 服务端 -> 客户端 | 群资料变化 |
| 143 | GroupRequest | 服务端 -> 客户端 | 收到入群申请（发群主）/ 登录补推 |
| 144 | GroupRequestResult | 服务端 -> 客户端 | 入群申请处理结果 |

---

### 6.3 登录

```json
{
  "from": 0,
  "to": 0,
  "type": 100,
  "content": "<JWT>"
}
```

成功收到 `type=101`：

```json
{
  "from": 0,
  "to": 1001,
  "type": 101,
  "content": "welcome"
}
```

失败收到 `type=102`：

```json
{
  "from": 0,
  "to": 0,
  "type": 102,
  "content": "invalid or expired token"
}
```

---

### 6.4 心跳

```json
{
  "from": 0,
  "to": 0,
  "type": 3
}
```

服务端不回复 pong，只刷新空闲计时。超过 `IDLE_TIMEOUT_SEC`（默认 60）秒没有任何消息会被断开。
客户端应定期发送心跳并自行实现断线重连。

心跳间隔怎么选（P0-4）：

- 建议 **30 秒**，必须显著小于空闲超时（留一倍余量，避免一次丢包就被踢）。
  压测客户端默认值已从 5s 调整为 30s。
- 5s 心跳在 500 条连接上是 100 帧/秒的纯无效唤醒与 8KB/s 的下行开销；
  而空闲超时本来就是 60s，把心跳降到 30s 不损失任何检测能力。
  需要更快的死链检测请依赖 TCP 层（连接已开 `SO_KEEPALIVE`）或应用层主动探测，
  不要用缩短心跳来做。
- 重连必须带**抖动退避**，并在重连前先调 `POST /api/token/refresh` 换 access token
  （见 §4.3）。否则一次网络抖动就会变成一次全量 bcrypt 登录风暴。

---

### 6.5 发送文本消息

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

- `from` 可以填 0，服务端以登录身份为准。
- `msg_id` 是客户端临时消息号，用于去重；同一会话、同一发送者重复的 `msg_id` 不会产生两条消息。
  重试同一条消息请**保持 `msg_id` 不变**，这是 `429/503/busy` 之后安全重发的前提。

服务端内部路径（P1-1/P1-2/P0-6/P1-4 都改在这里，客户端语义不变，但值得知晓）：

- 关系校验（是否好友 / 是否被拉黑）与消息落库现在是**一个 DB 任务、一个事务**，
  不再是"先查关系、再排队落库"两次；这消除了中间被删好友的 TOCTOU 窗口。
- 关系判定默认走进程内缓存（`RELATION_CACHE_TTL`，默认 10s；加好友/删好友/拉黑时立刻失效），
  命中时整条链路只剩落库事务本身。设成 `0` 可退回"每条消息都查库"的强一致行为。
- 落库事务的 SQL 往返从 10 次降到 7~8 次；`OFFLINE_CURSOR=1` 时不再写 `offline_messages`
  行级队列（语义差异见 §9.4）。
- 单条连接在途任务超过 `TCP_MAX_OUTSTANDING`（默认 64）时，这条消息直接回
  `type=102` 的 `Error("server busy")`，客户端应降速并按同一 `msg_id` 重试。

服务端回 `SendAck=113`：

```json
{
  "from": 0,
  "to": 1001,
  "type": 113,
  "msg_id": 88,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "sent"
}
```

如果命中去重，`content` 为 `"duplicate"`，返回的是已存在消息的 `msg_id/seq/ts`。

接收方会收到：

```json
{
  "from": 1001,
  "to": 1002,
  "type": 1,
  "msg_id": 88,
  "ts": 1760000000000,
  "conversation_id": 5,
  "seq": 7,
  "content": "hello B"
}
```

---

### 6.6 发送图片消息

`type=2`，`content` 建议存图片 URL，不要把二进制塞进消息体：

```json
{
  "from": 0,
  "to": 1002,
  "type": 2,
  "msg_id": 10002,
  "content": "https://example.com/images/a.png"
}
```

---

### 6.7 历史消息

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

- `to`：对方 userId（单聊由双方 ID 推导会话）
- `seq`：`before_seq`，0 表示从最新开始
- `content`：本次拉取条数，字符串形式

响应 `type=111`，`content` 是一段 JSON 数组文本：

```json
{
  "from": 0,
  "to": 1001,
  "type": 111,
  "content": "[{\"msg_id\":88,\"conversation_id\":5,\"seq\":7,\"from\":1001,\"type\":1,\"ts\":1760000000000,\"content\":\"hello B\"}]"
}
```

注意：`content` 本身是字符串，里面才是数组，客户端需要二次解析。

---

### 6.8 投递 / 已读回执

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

确认已读（不带 `"delivered"` 视为已读）：

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

服务端会更新 `last_delivered_seq` / `last_read_seq`，并删除已投递的离线消息。

---

### 6.9 好友申请事件（type=130）

发送方向：申请人 -> 被申请人。

新申请或登录补推时，接收方收到：

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

- `event`：`"new"` 表示刚收到的新申请，`"pending"` 表示登录补推的历史待处理申请。
- `msg_id` 复用为 `request_id`，方便客户端直接调用通过/拒绝接口。
- 客户端处理完申请后，调用 HTTP 的 `accept` / `reject` 接口。

---

### 6.10 好友申请结果事件（type=131）

发送方向：被申请人（处理者） -> 申请人。

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

`event` 取值：

- `"accepted"`：申请已通过（`status=1`）
- `"rejected"`：申请已拒绝（`status=2`）

如果申请人当时离线，服务端会在其下次 TCP 登录时补推。

---

### 6.11 好友列表变化事件（type=132）

发送方向：操作者 -> 自己或对方。

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

`action` 取值：

| action | 触发场景 |
| --- | --- |
| `added` | 好友申请被通过 |
| `deleted` | 删除好友 |
| `updated` | 修改备注 |
| `blocked` | 拉黑 |
| `unblocked` | 取消拉黑 |

收到事件后，客户端可以增量更新本地好友列表，也可以重新调用 `GET /api/friends` 拉取全量列表。

---

### 6.12 群事件（type=140~144）

群操作走 HTTP，本节事件均由服务端主动下行，`content` 是 JSON 文本（字段见 `group_types.h`）。

**GroupMemberChanged（140）**：邀请/建群(added)、移出(removed)、退群(quit)、任命或取消管理员(roleChanged)、转让(transferred)。

```json
{ "from": 1001, "to": 1002, "type": 140,
  "content": "{\"event\":\"group_member_changed\",\"action\":\"added\",\"group_id\":1,\"user_id\":1003,\"role\":3,\"operator_id\":1001}" }
```

- `action`：`added`/`removed`/`quit`/`roleChanged`/`transferred`。
- `user_id`：本次被操作的成员；`role`：其操作后的角色（removed/quit 填 0）。

**GroupDismissed（141）**：`{"event":"group_dismissed","group_id":1,"operator_id":1001,"name":"..."}`。

**GroupInfoChanged（142）**：`{"event":"group_info_changed","group_id":1,"operator_id":1001,"name":"新群名"}`，客户端收到后可重新拉群详情。

**GroupRequest（143）**：入群申请推给群主（`msg_id` 复用为 `request_id`）。`event`：`new`=新申请，`pending`=登录补推的待处理申请。客户端处理后调用 HTTP `accept`/`reject`。

**GroupRequestResult（144）**：审批结果推给申请人，`event`：`accepted`/`rejected`；离线时登录补推（依据 `group_requests.result_notified`）。

> 离线取舍：入群申请与结果**持久化**（`group_requests` 状态字段即“事件队列”），登录补推不丢；
> 而成员/资料/解散这类事件**不持久化**，客户端应在 TCP 登录后调用 `GET /api/groups` 对齐全量群列表（与好友列表变化事件 132 的处理一致）。

### 6.13 群消息发送 / 历史 / 回执

群消息复用 `type=1/2`，只需把 `to` 填成 `groupId` 并带上 `is_group=true`。

发送：

```json
{ "from": 0, "to": 1, "type": 1, "is_group": true, "msg_id": 10001, "content": "大家好" }
```

- 服务端在同一事务内：校验“发送者是群成员且群未解散” → 原子分配 `seq` → 写 `messages` → 给除发送者外全体成员写 `offline_messages`（写扩散）。
- 发送方收到 `SendAck=113`（`is_group=true`，带 `conversation_id/seq/msg_id`）；命中去重则 `content="duplicate"`。
- 在线成员直接收到 `type=1,is_group=true,from=<发送者>,to=<groupId>` 的消息；离线成员登录时由离线队列补推。

历史（`to=groupId, is_group=true`）：

```json
{ "from": 0, "to": 1, "type": 110, "is_group": true, "seq": 0, "content": "20" }
```

响应 `type=111`，`content` 为数组文本，每条含 `is_group` 字段。非成员拉群历史会被拒（`not a group member`）。

回执（`type=112`）与单聊一致：带 `conversation_id + seq`，更新该成员在群会话里的 `last_delivered_seq/last_read_seq`（GREATEST 只增不减）并删除已投递的离线消息。

---

## 7. 完整示例

### 7.1 HTTP 完整流程（curl）

```bash
BASE=http://127.0.0.1:8080

# 1) 注册
curl -s -X POST $BASE/api/register \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'

curl -s -X POST $BASE/api/register \
  -H "Content-Type: application/json" \
  -d '{"username":"bob","password":"secret123"}'

# 2) 登录
TOKEN_A=$(curl -s -X POST $BASE/api/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}' \
  | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')

TOKEN_B=$(curl -s -X POST $BASE/api/login \
  -H "Content-Type: application/json" \
  -d '{"username":"bob","password":"secret123"}' \
  | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')

# 3) A 搜索 B
curl -s "$BASE/api/users/search?keyword=bob" \
  -H "Authorization: Bearer $TOKEN_A"

# 4) A 向 B 发申请（假设 B 的 userId=1002）
curl -s -X POST $BASE/api/friends/requests \
  -H "Authorization: Bearer $TOKEN_A" \
  -H "Content-Type: application/json" \
  -d '{"toUserId":1002,"message":"hi"}'

# 5) B 查看收到的申请
curl -s "$BASE/api/friends/requests?direction=incoming" \
  -H "Authorization: Bearer $TOKEN_B"

# 6) B 通过（假设 requestId=10）
curl -s -X POST $BASE/api/friends/requests/10/accept \
  -H "Authorization: Bearer $TOKEN_B"

# 7) A 查看好友列表
curl -s $BASE/api/friends \
  -H "Authorization: Bearer $TOKEN_A"

# 8) A 修改备注
curl -s -X PATCH $BASE/api/friends/1002 \
  -H "Authorization: Bearer $TOKEN_A" \
  -H "Content-Type: application/json" \
  -d '{"remark":"B-remark"}'

# 9) A 删除好友
curl -s -X DELETE $BASE/api/friends/1002 \
  -H "Authorization: Bearer $TOKEN_A"
```

### 7.2 Python TCP 客户端示例

```python
import json
import socket
import struct

TCP_HOST = "127.0.0.1"
TCP_PORT = 8888
TOKEN = "<从 HTTP /api/login 获取的 JWT>"


def send_frame(sock, obj):
    """发送一帧：4 字节大端长度 + JSON body。"""
    body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
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
    """接收一帧并解析成 dict。"""
    length = struct.unpack("!I", recv_exact(sock, 4))[0]
    return json.loads(recv_exact(sock, length))


def main():
    sock = socket.create_connection((TCP_HOST, TCP_PORT), timeout=10)

    # 1) TCP 登录
    send_frame(sock, {"from": 0, "to": 0, "type": 100, "content": TOKEN})
    resp = recv_frame(sock)
    if resp["type"] != 101:
        raise RuntimeError("login failed: %r" % resp)
    print("login ok:", resp["content"])

    # 2) 发文本给 userId=1002
    send_frame(sock, {
        "from": 0,
        "to": 1002,
        "type": 1,
        "msg_id": 1,
        "content": "hello from python",
    })

    # 3) 等 SendAck，同时打印收到的聊天消息/好友事件
    while True:
        msg = recv_frame(sock)
        print("recv:", msg)
        if msg["type"] == 113:          # SendAck
            break
        if msg["type"] == 102:          # Error
            raise RuntimeError(msg["content"])

    # 4) 拉取历史消息
    send_frame(sock, {
        "from": 0,
        "to": 1002,
        "type": 110,
        "seq": 0,
        "content": "20",
    })
    while True:
        msg = recv_frame(sock)
        if msg["type"] == 111:
            history = json.loads(msg["content"])
            print("history:", history)
            break

    sock.close()


if __name__ == "__main__":
    main()
```

### 7.3 C++ 分帧示例

```cpp
#include <boost/asio.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using boost::asio::ip::tcp;

static void send_frame(tcp::socket& s, const std::string& json) {
    const std::uint32_t net_len =
        htonl(static_cast<std::uint32_t>(json.size()));
    std::vector<char> out(4 + json.size());
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, json.data(), json.size());
    boost::asio::write(s, boost::asio::buffer(out));
}

static std::string recv_frame(tcp::socket& s) {
    char header[4];
    boost::asio::read(s, boost::asio::buffer(header, 4));
    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, 4);
    const std::uint32_t len = ntohl(net_len);

    std::vector<char> body(len);
    boost::asio::read(s, boost::asio::buffer(body));
    return std::string(body.begin(), body.end());
}
```

完整客户端可参考：

- `step2_test_client/test_client.cpp`
- `step2_test_client/test_friend_client.cpp`

---

## 8. 验收脚本

```bash
# 基础聊天 + 离线补推 + 历史 + 重连（HTTP 登录 + TCP 长连接）
bash step3_verify/verify_tcp_http.sh 8888 "$(pwd)/build" 8080

# 好友模块：搜索 / 申请 / 通过 / 拒绝 / 列表 / 备注 / 删除 / 拉黑 + TCP 事件
bash step3_verify/verify_friend.sh 8888 "$(pwd)/build" 8080

# 群聊模块：建群 / 邀请 / 任命 / 移出 / 转让 / 解散 / 入群申请 + 群消息在线投递
#           / 离线写扩散补推 / client_msg_id 去重 / 群历史分页 + TCP 群事件
bash step3_verify/verify_group.sh 8888 "$(pwd)/build" 8080
```

`verify_friend.sh` 会自动应用最新 `db/schema.sql`，并运行
`step2_test_client/test_friend_client.cpp`。

`verify_group.sh` 同样先应用 `db/schema.sql`（含 `groups/group_members/group_requests`），
再运行 `step2_test_client/test_group_client.cpp`；测试账号使用 `grpA_/grpB_/grpC_/grpD_`
随机后缀，不会污染固定账号。

验收脚本的三条前置约定（`verify_group.sh` 已实现，另外两个脚本仍待对齐）：

1. **两个端口必须是空着的**，脚本会先探测；被占用就直接失败并给出查占用者的命令。
   原因很实际：脚本用 `/dev/tcp` 探端口来判断"自己起的服务就绪"，如果端口被
   上一个实例（手工起的、或上次没退干净的）占着，那么本脚本的服务 bind 失败退出，
   而探端口立刻成功——于是验收的是**一个你并不知道存在的旧二进制实例**，
   失败信息会指向完全错误的方向。
2. **就绪判定同时盯着自己那个进程的存活状态**：进程一死（端口冲突、DB 连不上、
   配置读错）就立即失败并打出服务日志，而不是继续等陌生端口。
3. **验收开头会打印服务端自述的运行期参数**：
   `[db] pool max / sqlWorkers / hashWorkers / bcryptCost / queueLimit / offlineCursor /
   relationCacheTtlMs`、`[server] io threads`、两个监听端口。
   这批优化全部由环境变量控制，参数没生效时表现为"改了但曲线没动"，
   所以先自述、再验收（对应 §11.6 检查清单第 3 条）。

失败时脚本还会按 §11.5 的过载响应表给三条排查提示，其中最关键的一条：
**客户端收到 429/503 是服务端在正确地拒绝过载，不是功能坏了**——不过验收只建几个账号，
正常不会触发；真触发了就该检查 `AUTH_RATE_PER_SEC` / `AUTH_IP_PER_SEC` /
`HTTP_MAX_INFLIGHT` 是不是被调得过低了。

---

## 9. 数据库表

| 表 | 说明 |
| --- | --- |
| `users` | 用户账号、密码哈希、昵称、头像、状态 |
| `friends` | 好友关系（双向各一行，status=1 好友，status=2 拉黑） |
| `friend_requests` | 好友申请（单向，0=待处理，1=通过，2=拒绝，3=取消） |
| `conversations` | 单聊/群聊会话，`last_seq` 为会话内序号生成器 |
| `conversation_members` | 会话成员与投递/已读游标 |
| `messages` | 消息历史，服务端分配 `id/seq/created_at` |
| `offline_messages` | 离线消息队列，登录补推后按 ACK 删除（仅 `OFFLINE_CURSOR=0` 默认模式使用，见 §11.4） |
| `groups` | 群资料：群名/公告/简介/头像/群主/状态/人数上限，关联一个 `type=2` 会话 |
| `group_members` | 群成员与角色（1=群主，2=管理员，3=成员）、群内昵称；`(group_id,user_id)` 主键 |
| `group_requests` | 入群申请（单向，带状态机与 `result_notified` 离线补偿），风格同 `friend_requests` |

完整建表语句与字段注释见 [`db/schema.sql`](db/schema.sql)。

---

## 10. 注意事项

- HTTP 好友接口必须带 `Authorization: Bearer <JWT>`。
- TCP 登录只信任 JWT 的 `sub`，不要依赖客户端自报的 `from`。
- TCP 空闲 `IDLE_TIMEOUT_SEC`（默认 60）秒会被服务端断开，客户端需要心跳与断线重连；
  心跳建议 30s，重连前先走 `/api/token/refresh` 而不是密码登录（§4.3）。
- 好友申请与申请结果优先通过 TCP 实时推送；离线期间的新申请/处理结果会在下次 TCP 登录时补推。列表变化事件不持久化，离线或多端场景建议重新调用 `GET /api/friends` 拉取全量列表。
- 消息正文不要放二进制；图片/文件请先上传对象存储，再把 URL 放进 `content`。
- 生产环境必须在 HTTP 前置 HTTPS、在 TCP 前置 TLS，并补充限流、防重放、日志脱敏等能力。


---

## 11. 性能与容量（2 核 / 2GB / 3Mbps 受限服务器）

> 本节记录 `docs/IMS_llm_Optimize_Solution.md` 里 P0-1 ~ P1-8 这批优化在本仓库的落地方式、
> 需要运维/客户端配合的地方，以及**怎么用压测验证它真的生效**。
> 文中标"实测"的来自 `stress_report/` 的数据；标"推算/预期"的是模型结果，必须复压才能确认。

### 11.1 瓶颈是什么：CPU 被 bcrypt 占满

`test_stress_03` 的登录延迟与并发好友数呈**严格线性**关系。取每轮 `login` 的最大值
（= 最后一个排完队的用户，等价于整个哈希风暴的排空时间）做差分：

```text
(14384 - 3384) / (100 - 20)  = 137.5 ms / 用户
(28198 - 14384) / (200 - 100) = 138.1 ms / 用户      → 斜率 ≈ 138ms/用户，截距 ≈ 0.6s
```

每个用户在这段窗口里正好提出 **2 次 bcrypt**（注册的 `generateHash` + 登录的
`validatePassword`）。在 C 个核上 `每用户墙钟 = 2 × t_hash / C`，代回 C=2 得
**t_hash ≈ 138ms（本机 cost=12 的真实单次耗时）**。于是：

```text
CPU 供给     = 2 核 = 2000 ms CPU/s
每用户需求   = 2 × 138 = 276 ms CPU
注册+登录容量 = 2000 / 276 ≈ 7.2 用户/s
```

用这个容量反推排空时间，与三轮实测吻合：

| N | 预测 N/7.2 | 实测 login max |
| --- | --- | --- |
| 20 | 2.8 s | 3.38 s |
| 100 | 13.9 s | 14.38 s |
| 200 | 27.8 s | 28.20 s |
| 500 | 69 s | 超时中止（240s 内没跑完 accept 阶段） |

三条关键推论，直接决定"该改什么、不该改什么"：

1. **加连接池 / 加线程没用**。11-12 与 11-17 两轮数据几乎一致，那时已经把连接池扩到 50、
   DB worker 扩到 8、io 线程改成多线程 `run()`——因为限制是"CPU 秒数"，不是"并发度"。
2. **它会连坐所有接口**。同机 `mariadbd` 抢不到核，所以完全不碰 bcrypt 的
   `create request`（126ms → 1043ms）、`list friends`（103ms → 477ms）在 N 从 20 涨到 100 时一起涨。
3. **真实风险场景不是"100 个人同时注册"，而是"重连风暴"**：服务端重启或网络抖动后，
   所有客户端同时回落到密码登录，就会重现这条曲线。所以 P0-3（免密刷新）比 P0-1（降成本）更治本。

对照实验（去掉 bcrypt 之后还剩什么）：`test_stress_02_20261117` 用**已存在**的用户做全链路
200 线程 × 100s，得到 `44.5 iter/s`、`tcp p50 342ms`、`http p50 1207ms`。
说明第二瓶颈真实存在，但量级小一个数量级——**先修 P0，否则 SQL 层的改动在曲线上看不出来**。

### 11.2 本仓库落地清单

| 项 | 改法 | 代码位置 |
| --- | --- | --- |
| P0-1 bcrypt 成本 | 硬编码 `cost=12` → `DbRuntime::bcryptCost`（默认 10，`BCRYPT_COST` 可覆盖）；哈希 worker 数 = 核数 | `db.h:DbRuntime`、`db.cpp:DbRuntime::fromEnv`、`registerUser` |
| P0-2 双线程池 | `Db` 拆成 `sqlQueue_/sqlWorkers_` 与 `hashQueue_/hashWorkers_`；注册=「hash 任务→SQL 任务」，登录=「SQL 任务→hash 任务」，**哈希期间不再持有 DB 连接** | `db.h` 私有成员、`db.cpp:Db::Db/workerLoop/submitSql/submitHash` |
| P0-3 免密刷新 | access/refresh 两类 JWT（payload `typ` 区分）+ `POST /api/token/refresh`（只验签，不查库不算哈希）；登录响应带 `refreshToken/expiresIn` | `token_service.{h,cpp}`、`jwt_config.{h,cpp}`、`http_server.cpp:do_refresh_token` |
| P0-4 过载保护 | 认证接口全局 + 每 IP 令牌桶（429 + `Retry-After`）；HTTP 在途**响应**上限 + SQL 队列深度准入（503）；请求体上限 64KB（413）；热路径队列满回 `"server busy"`；TCP 连接数上限、单连接在途任务上限；写队列 4MB 高水位踢连接；**压测客户端已按 `Retry-After` 退避重试 429/503**（否则服务端正确的拒绝会被记成业务失败） | `http_server.cpp`（`OverloadConfig/RateLimiter/inflightCount`）、`db.cpp:submitSql/submitAlways`、`server.cpp:maxSessions`、`session.cpp:reserveJob/send_message`、`step2_test_client/test_stress_0{2,3}.cpp` |
| P0-5 构建类型 | 默认 Release（Linux `-O2`，Windows `--config Release`）；产物路径与压测口径写进本节 | `CMakeLists.txt`、`build.sh` |
| P0-6 DB 往返 | 注册去掉前置查重 `SELECT`（靠 1062 兜底）；池借还不再无条件 `isValid()` ping（空闲 30s 才 ping；只有开过事务的连接才 RESET）；消息"取 msg_id + 回读整行"合并为 `SELECT ... WHERE id=LAST_INSERT_ID()`；关系校验成立时跳过接收者存在性查询 | `db.cpp:ConnectionPool::acquire/release`、`PooledConn`、`sendSingleTxn`、`registerUser` |
| P0-7 参数对齐 | `DB_POOL_SIZE` 默认 = `DB_WORKER_COUNT`；worker 按核数给（不再 8 worker 抢 2 核）；systemd 加 `LimitNOFILE=65535`、`LimitCORE=0`；io 线程数改为按核数（不再"至少 4"） | `build.sh`、`main.cpp` |
| P1-1 关系缓存 | 进程内 LRU+TTL（key=(小uid,大uid)，存双向 status）；通过申请/删除/拉黑（含批量）处显式失效；`RELATION_CACHE_TTL=0` 可关 | `db.cpp:relationCache*`、`sendChecked`、`handleFriendRequest`、`deleteFriend`、`deleteFriendsBatch`、`setFriendBlocked` |
| P1-2 发送合并 | `areFriends` + `sendMessage` 两段回调 → 单个 `Db::sendChecked`（一个任务、关系与落库同事务）；`do_send` 少一层回调与两次大对象拷贝 | `db.cpp:sendChecked`、`session.cpp:do_send` |
| P1-3 登录补推合并 | 5~7 个 DB 任务 → `Db::fetchLoginSync` 1 个任务（一条连接顺序取完，无群用户跳过群查询）；单次离线消息 200 → 100 条 + `offlineMore`（限制见 §11.4） | `db.cpp:fetchLoginSync`、`session.cpp:push_login_sync` |
| P1-4 游标离线 | `OFFLINE_CURSOR=1` 时不写/不删 `offline_messages`，补推按 `messages.seq > conversation_members.last_delivered_seq` 计算（单聊与群聊都生效） | `db.cpp:offlineFetchSql/sendSingleTxn/sendGroupMessage/updateReceipt` |
| P1-5 TCP_NODELAY | TCP 与 HTTP accept 后都设 `no_delay`；TCP 侧另开 `SO_KEEPALIVE` | `session.cpp:start`、`http_server.cpp:run` |
| P1-6 keep-alive | HTTP 从"一问一答即关"改为按客户端意愿复用（空闲 60s / 单连接 256 请求后关闭），并加请求体上限 | `http_server.cpp:http_session` |
| P1-7 批量写 | `POST /api/friends/requests/batch`、`POST /api/friends/batch-delete`：一个事务、一次多行语句、按 id 升序固定加锁顺序。同时要求与单条接口**行为对齐**：逐个推 `FriendListChanged`/`FriendRequest` 事件；附言超长像单条一样回 400（不能 `resize(255)` 按字节切断中文，否则整批 INSERT 失败）；`INSERT IGNORE`/UPDATE 守卫"什么都没做成"的目标补进 `skipped`；`deleted` 用"真正从我的好友列表消失的人数"而不是 DELETE 的 affected rows | `db.cpp:createFriendRequestsBatch/deleteFriendsBatch`、`http_server.cpp` 路由与响应 |
| P1-8 合并写 | 一次 `async_write` 最多合并 32 帧 / 64KB，帧顺序不变，减少系统调用与碎包 | `session.cpp:do_write` |
| 心跳 | 压测客户端默认 5s → 30s | `step2_test_client/test_stress_0{2,3}.cpp` |

删除的旧入口：`Db::areFriends()`——同一段判定 SQL 留两份实现迟早漂移，判定已并入 `sendChecked`。
保留但当前无生产调用的只读接口（`fetchPendingIncoming / fetchUnnotifiedResults / sendMessage /
fetchOffline` 等）的理由写在 `db.h` 对应位置：它们是纯读、不参与缓存失效，且排查脚本会用到。

### 11.3 线程模型（为什么"加线程"不再是要事）

```text
io 线程 × 核数(2~8) ：epoll 收发 + JSON 编解码 + JWT 验签 + 写队列，不做任何阻塞调用
        │
        ├─ sqlQueue_  → SQL worker × min(核数*2, 8) ：借连接执行 SQL（阻塞 IO）
        └─ hashQueue_ → hash worker × 核数          ：只算 bcrypt，不持有连接
                             │ postToIo
io 线程回调里再 post 到「本连接的 strand」→ 才允许碰 session 状态
```

三条纪律：

1. **CPU 型与阻塞型任务不共用一条队列**，否则注册风暴会把不相干的发消息/查列表一起拖死
   （这就是 P0-2 的全部动机）。
2. **线程数按核数收敛**。2 核机器上 8 个 worker 跑 bcrypt 不会提高吞吐，只会让
   所有线程（含 `mariadbd`）的延迟一起变差。
3. `session` 的状态只允许在本连接的 strand 上改。新增的 `outstanding_`（在途任务配额）与
   `write_queue_bytes_` 都遵守这条：归还配额的 `releaseJob()` 一律放在 strand 内，
   **不能**在 DB 回调线程里直接改。

### 11.4 两个需要人来定的开关

这两个不是"调大就更快"的参数，而是**语义选择**，上线前请产品/运维明确表态。

**`OFFLINE_CURSOR`（P1-4）**

| | `=0` 行级队列（默认） | `=1` 游标 |
| --- | --- | --- |
| 每条消息写侧 | 多 1 条 `INSERT offline_messages` | 不写 |
| ACK | `UPDATE` 游标 + `DELETE` 队列行 | 只有 `UPDATE` |
| 群消息 | 按成员数线性写行（100 人群 = 100 行/条消息） | 与成员数无关 |
| 多端登录 | 每台设备各自确认，互不影响 | **游标是账号级**：先 ACK 的那台会把另一台的未读一起推进 |
| 表膨胀 | 随消息量增长，靠 ACK 清理 | 不增长 |

结论：单端为主、想省写放大 → `=1`；需要"手机看过的不该影响桌面端未读"这类多端语义 → 保持 `=0`。
两种模式**可以在线切换（不删数据），但两个方向都有必须知道的边界**：

| 切换方向 | 后果 | 处置 |
| --- | --- | --- |
| `=1` → `=0` | 游标模式期间产生的消息没有 `offline_messages` 行，切回后这部分**不会补推**（消息本身仍在 `messages`，历史接口能查到） | 低峰切换，或切换后让客户端做一次全量历史对齐 |
| `=0` → `=1` | 队列里尚未 ACK 的 `offline_messages` 行不再被删除，表停止收敛（不丢消息：游标覆盖同样的范围，因为游标推进与删除用的是同一次 ACK） | 切换前把队列排空，或保留一次性清理任务 |

另外一条与本次改动相关的已知限制，别当成已解决：单次登录补推上限从 200 降到 100 之后，
`offlineMore` 目前**只写日志、没有下行信令**——超过上限的离线消息要等下一次登录才会继续补。
（"把 200 降到 100"只是把这个既有边界提前，不是新引入的问题类；老实现同样在 200 条截断。）
不直接加一个新下行帧类型的原因：老客户端的 `parse_message` 对未知 type 会判为非法并断开连接，
协议兼容性比这一条信令值钱。真正的解法是把 ACK 与补推打通（`updateReceipt` 返回"是否还有未推完"，
有就立刻继续推），那需要改回执的返回语义，属于后续项。

**`RELATION_CACHE_TTL`（P1-1）**

- 默认 10s：发消息时先查进程内缓存，命中就不碰数据库；通过申请/删除/拉黑时**立刻显式失效**，
  所以单进程部署下几乎不存在窗口。
- `0` = 关闭缓存，每条消息都在事务内实时查关系（强一致，代价是每条消息多一条 SQL）。
- 已知边界：缓存是**进程内**的。将来多实例部署时，实例 A 的失效不会通知实例 B，
  窗口最长等于 TTL——那时必须换成带广播失效的集中式缓存（Redis pub/sub 或版本号）。
  这条约束写在 `db.h:sendChecked` 的注释里，扩容时不要漏。

### 11.5 过载时服务端会怎么回应

| 触发 | 表现 | 客户端应该做 |
| --- | --- | --- |
| 认证速率超 `AUTH_RATE_PER_SEC` / `AUTH_IP_PER_SEC` | HTTP `429` + `Retry-After` | 按 `Retry-After` 退避重试，不计入业务失败率 |
| 在途响应超 `HTTP_MAX_INFLIGHT` **或** SQL 队列深度超同一阈值 | HTTP `503` + `Retry-After`，随后关连接（新连接在解析第一个请求前就被拒） | 退避后重连再试 |
| DB 队列超 `DB_QUEUE_LIMIT`（已改造的热路径接口） | `message="server busy"` + HTTP `503` | 同上 |

队列上限有软硬两级，这点容易被误解，单独说明：

- **硬**（回 `kBusyError` → 429/503/Error 帧）：注册、登录、发消息（单聊/群聊）、历史翻页、
  好友申请与删除（含批量）、登录补推——这些回调签名明确，能立刻给出错误响应。
- **软**（只告警，任务照常排队）：其余仍走 `Db::submit()` 的 DAO（群资料/成员管理等）。
  为什么不能丢：这些 DAO 的回调签名各不相同，投递口无法统一回一个错误；一旦丢任务，
  客户端会干等到超时，而 TCP 侧那条连接已经预占的在途配额（`reserveJob`）
  只能在回调里归还——**丢任务会让这条连接永久报 "server busy"**。
  所以过载的真正闸门放在入口层（HTTP 在途上限 + 认证限流、TCP 连接数上限 + 每连接在途上限），
  软上限只是"该调阈值了"的信号（`[db] sql queue over soft limit`，每 100 次打一条）。
| 请求体超 `HTTP_BODY_MAX` | HTTP `413`，随后关连接 | 修正请求，不要重试 |
| TCP 并发连接超 `TCP_MAX_SESSIONS` | 新连接被立刻关闭 | 抖动退避后重连 |
| 单连接在途任务超 `TCP_MAX_OUTSTANDING` | TCP `Error("server busy")` | 降速，**用同一 `msg_id` 重试**（服务端按 `client_msg_id` 去重） |
| 单连接写队列超 4MB（对端读得太慢） | 服务端主动断开该连接 | 重连后走登录补推 / 历史接口对齐（消息已落库，不会丢） |

### 11.6 部署检查清单（2C2G 这一档）

```bash
# 1) 构建：确认不是 -O0 / Debug
CMAKE_BUILD_TYPE=Release bash build.sh

# 2) 安装 .deb 后确认 fd 上限真的生效（不要相信发行版默认值）
systemctl show CXIM -p LimitNOFILE
cat /proc/$(pgrep -x cxim-server)/limits | grep "open files"

# 3) 启动日志里核对线程/成本参数（这是最快的"配置有没有生效"检查）
journalctl -u CXIM -b | grep -E "pool max|io threads"
#   期望：pool max = sqlWorkers、hashWorkers = 核数、bcryptCost = 10、io threads = 核数

# 4) mariadbd 侧余量：cxim 的连接数不能超过 max_connections
mysql -e "SHOW VARIABLES LIKE 'max_connections'"
mysql -e "SELECT COUNT(*) FROM information_schema.PROCESSLIST"
```

同机资源分配：`prepare_env_V0.sh` 给的是 `innodb_buffer_pool_size=384M`、`max_connections=100`、
`performance_schema=OFF`、`innodb_flush_log_at_trx_commit=1`。最后一条是刻意的
（"IM 消息优先不丢"），**不要为了压测数字改它**——省 fsync 的收益远不如
"少写一条 `offline_messages`"来得正当。

### 11.7 怎么验证优化生效

**验收口径不是"p50 小了多少"，而是"登录延迟对 N 的斜率"**——斜率直接等于
`2 × t_hash / 核数`，是最不容易被其它噪声掩盖的信号。

| 指标 | 优化前（实测） | 目标 |
| --- | --- | --- |
| `login` 斜率 | 138 ms/用户 | < 5 ms/用户（重连走 refresh 后应 ≈ 0） |
| `login` max，N=100 | 14.38 s | < 1 s |
| `create request` p50，N=100 | 1043 ms | < 200 ms（与登录风暴解耦） |
| `tcp send` p50，N=100 | 500 ms | < 150 ms |
| 全生命周期 elapsed，N=100 | 50.4 s | < 10 s |
| N=500 | 超时中止 | 跑通（部分请求 429/503 属预期降级） |

复压步骤：

```bash
# 服务端先按 §11.6 确认参数生效，再：
./build/bin/Release/test_stress_03.exe --host <ip> --http-port 10002 --tcp-port 10001 \
    --friends 20 --timeout 120        # 快循环，先确认功能没坏
./build/bin/Release/test_stress_03.exe --host <ip> --http-port 10002 --tcp-port 10001 \
    --friends 100 --timeout 240
./build/bin/Release/test_stress_03.exe --host <ip> --http-port 10002 --tcp-port 10001 \
    --friends 200 --timeout 240
```

压测期间同时在服务器上看"谁在吃核"——这是判断"是否还有第二个 CPU 型瓶颈"的唯一可靠办法：

```bash
perf top -p $(pgrep -d, -x cxim-server)   # bcrypt 修好后 blowfish/E_* 系列符号应大幅退场
mpstat -P ALL 1                            # 两个核不应再长时间 100%
vmstat 1                                   # r（运行队列长度）应从 6~10 回到 2~3
ss -s ; netstat -s | grep -i retrans       # 验证那一簇 ~3.1s 长尾是否为 RTO 重传
./stress_report/show_svc_monitor.sh        # 现成的服务资源面板
```

先看一个容易误读的点：**压测客户端已经会按 `Retry-After` 退避重试 429/503**
（`http_call` 最多 4 次，`http_call_once` 才是单次）。所以
- 数据里的 `register/login` 延迟**包含退避等待**——这是对的，客户端真实感受到的延迟就该算进去；
- 但也别把"服务端在限流"读成"服务端变慢了"：如果某一轮看到延迟明显高于 §11.8 的预测，
  先在服务端确认是不是限流命中（日志里 `server busy` / 客户端收到 429），
  再决定是调 `AUTH_RATE_PER_SEC` 还是继续优化。把 429/503 记成业务失败的时代已经过去了。

另外三点测量学提醒（否则会把客户端的毛病算成服务端的）：

1. **必须用 Release 客户端**。原报告里的样本全部来自 `build/bin/Debug/test_stress_03.exe`，
   MSVC Debug 的 `/Od` + 安全迭代器会把客户端自己的耗时放大几十倍，而延迟是从客户端计时的。
2. **`tcp reply` 有相当比例是脚本自己的串行模型**：主用户先收满 N 条，再**逐条**回复并等
   `SendAck`，于是第 i 个好友的"收回复延迟" ≈ i × 单条往返，是 O(N²) 的测量伪影。
   由它反推的"单条消息端到端 ≈ 50ms"才是服务端消息路径的真实基线。
   要测消息路径吞吐，请改脚本为并发回复，或直接看 `tcp send`（SendAck 延迟）。
3. N=500 这一档建议**分布式压测**：当前脚本每个 friend 起 3 个线程（worker + recv + 心跳），
   500 并发就是 1500+ 线程，Windows 压测机自己会先到极限，`connect` 失败未必是服务端的错。

### 11.8 预期收益（推算，未复压）

以本机实测 `t_hash(cost=12) ≈ 138ms` 为基准，bcrypt 成本每 -1 耗时减半：

| 配置组合 | 注册+登录容量 | N=100 登录排空（含 0.6s 截距） |
| --- | --- | --- |
| 现状 cost=12 | 7.2 用户/s | 14.4 s |
| P0-1（cost=10） | ≈ 29 用户/s | ≈ 4.1 s |
| P0-1 + P0-2（解队头阻塞） | ≈ 29 用户/s | ≈ 4.1 s，且非认证接口不再连坐 |
| **P0-3 重连走 refresh** | 密码登录不再参与重连 | **≈ 0.1 s（只剩一次 HMAC）** |
| P0-5 / P0-6 / P1-2 / P1-3 / P1-5 | 改善常数项与第二瓶颈 | 稳态吞吐提升，需实测 |

安全性的诚实说明：把 cost 从 12 降到 10 会**削弱离线爆破的抵抗强度**（每 -1 强度减半），
这是为了在 2 核机器上换取可接受的登录延迟而做的明确取舍，补偿手段是 P0-4 的登录限流
（全局 + 每 IP）与口令长度/弱口令校验。如果这条取舍不被接受，那么**唯一不动安全基线的解法
是把哈希算力外置**（P3-1：独立 auth 进程/实例，或升 4 核——容量与核数成正比）。
