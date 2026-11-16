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
bash build_step.sh
```

构建产物：

```text
build/bin/Debug/cxim-server.exe   # Windows
build/bin/cxim-server             # Linux
```

### 2.2 启动

```bash
DB_CONFIG=./DBConfig.ini JWT_CONFIG=./JWTConfig.json \
HTTP_PORT=8080 TCP_PORT=8888 ./build/bin/Debug/cxim-server.exe 
```

启动成功后会看到类似输出：

```text
[db] connecting to 127.0.0.1:3306/CXIMDB as cximUser
[server] listening on port 8888
[http] listening on port 8080
```

### 2.3 初始化数据库

```bash
# 使用项目自带的 seed_db（推荐，不依赖本机 mysql 客户端）
./build/bin/Debug/seed_db.exe DBConfig.ini db/schema.sql

# 或者
mysql -h <host> -P <port> -u <user> -p <dbname> < db/schema.sql
```

---

## 3. 协议总览

### 3.1 HTTP 短连接

- 默认端口：`8080`
- 请求/响应体：UTF-8 JSON
- `Content-Type: application/json`
- 每个连接处理一个请求后关闭（`Connection: close`）
- 除注册/登录外，其余接口都需要：

```http
Authorization: Bearer <JWT>
```

JWT 通过 `POST /api/login` 获取。

### 3.2 TCP 长连接

- 默认端口：`8888`
- 每一帧：`4 字节大端长度（网络序） + JSON body`
- body 是 `Message` 的 JSON 序列化结果
- 空闲超过 60 秒会被服务端踢掉
- 同一账号重复登录会踢掉旧连接

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

响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "userId": 1001,
    "username": "alice",
    "token": "eyJhbGciOiJIUzI1NiIs..."
  }
}
```

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
| 401 | 未带 token / token 无效或过期 |
| 404 | 路径不存在 |
| 405 | HTTP 方法不允许 |

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

响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "userId": 1001,
    "username": "alice",
    "token": "eyJhbGciOiJIUzI1NiIs..."
  }
}
```

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

服务端不回复 pong，只刷新空闲计时。超过 60 秒没有任何消息会被断开。
客户端应定期发送心跳并自行实现断线重连。

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
| `offline_messages` | 离线消息队列，登录补推后按 ACK 删除 |
| `groups` | 群资料：群名/公告/简介/头像/群主/状态/人数上限，关联一个 `type=2` 会话 |
| `group_members` | 群成员与角色（1=群主，2=管理员，3=成员）、群内昵称；`(group_id,user_id)` 主键 |
| `group_requests` | 入群申请（单向，带状态机与 `result_notified` 离线补偿），风格同 `friend_requests` |

完整建表语句与字段注释见 [`db/schema.sql`](db/schema.sql)。

---

## 10. 注意事项

- HTTP 好友接口必须带 `Authorization: Bearer <JWT>`。
- TCP 登录只信任 JWT 的 `sub`，不要依赖客户端自报的 `from`。
- TCP 空闲 60 秒会被服务端断开，客户端需要心跳与断线重连。
- 好友申请与申请结果优先通过 TCP 实时推送；离线期间的新申请/处理结果会在下次 TCP 登录时补推。列表变化事件不持久化，离线或多端场景建议重新调用 `GET /api/friends` 拉取全量列表。
- 消息正文不要放二进制；图片/文件请先上传对象存储，再把 URL 放进 `content`。
- 生产环境必须在 HTTP 前置 HTTPS、在 TCP 前置 TLS，并补充限流、防重放、日志脱敏等能力。
