# KC-CXIM-IMS 通信选型设计说明

> 文档性质：架构选型分析（ADR）
> 适用工程：`dev/IMS/KC-CXIM-Svc/basic_service_20261107`
> 主题：即时消息（IM）通道为何直接使用 **TCP 长连接 + 自定义分帧**，而不是 WebSocket
> 说明：仓库中并未留下明确的选型记录，本文依据现有代码结构、协议实现与客户端形态推导整理。

---

## 一、结论摘要

**WebSocket 本质上是"TCP + HTTP Upgrade 握手 + RFC 6455 帧封装"的应用层协议，它与裸 TCP 并非互斥的传输层方案。**
本项目两端（服务端 C++ / 客户端 C++ 桌面端）均由自己掌控，且客户端不是浏览器，因此直接使用裸 TCP 并在其上自定义轻量分帧，是更简单、开销更低、可控性更强的选择。WebSocket 的核心价值（让浏览器获得全双工能力、复用 HTTP 端口穿透代理）在本项目场景中收益很小，反而会带来额外的握手、帧头、掩码与依赖成本。

一句话：**不是"TCP vs WebSocket"的传输层之争，而是"是否需要为浏览器/HTTP 生态支付标准化外壳的成本"的取舍。**

---

## 二、当前架构现状

### 2.1 组件与职责

| 组件 | 文件 | 职责 |
|---|---|---|
| TCP 长连接服务器 | `step1_tcp_echo/server.cpp` | 监听端口、`async_accept`，每条连接创建 `session` |
| 会话（连接生命周期） | `step1_tcp_echo/session.cpp` | 读写分帧、消息分发、登录鉴权、发消息、离线补推、历史、回执、空闲踢人 |
| 应用层分帧 | `step1_tcp_echo/frame.h` | 4 字节大端长度头 + body，处理粘包/半包 |
| 消息模型 | `step1_tcp_echo/message.h` | `Message` / `MsgType` / `StoredMessage` |
| JSON 编解码 | `step1_tcp_echo/message_codec.h` | RapidJSON 序列化 / 反序列化 |
| HTTP 短连接服务器 | `step1_tcp_echo/http_server.cpp` | 仅提供 `/api/register`、`/api/login`，签发 JWT |
| 在线表 | `step1_tcp_echo/session_manager.*` | `userId -> session` 路由 |
| 数据库层 | `step1_tcp_echo/db.cpp` | 连接池 + worker 线程，消息落库与离线队列 |
| 入口 | `step1_tcp_echo/main.cpp` | 单个 `io_context` 同时挂 TCP 与 HTTP 两个 acceptor |

### 2.2 连接模型

```
桌面客户端 (C++)
   │
   ├── HTTP 短连接 (8080)  ──> /api/register、/api/login ──> 返回 JWT
   │
   └── TCP 长连接 (8888)   ──> 登录(携带 JWT) / 心跳 / 发消息 / 历史 / 回执
                                │
                                └── SessionManager 按 userId 路由 ──> 对端 session
```

- 注册/登录这类**一次性、无状态**操作走 HTTP 短连接（`http_server.cpp:108-111`），回完即关（`http_server.cpp:183`）。
- 实时消息、在线状态、离线补推等**有状态、双向、长时间存活**的通道走 TCP 长连接（`main.cpp:66-68`）。
- 鉴权统一为无状态 JWT：HTTP 登录时签发（`http_server.cpp:170`），TCP 登录时验签（`session.cpp:127-141`），TCP 侧不再查库比对密码。

### 2.3 应用层协议

```
+------------------+--------------------------------+
| 4 字节长度 len   |  len 字节的 JSON 消息体         |
| （网络字节序/大端）|                                |
+------------------+--------------------------------+
```

- 编码/解码：`frame.h:50`（`htonl`）、`frame.h:62`（`ntohl`）。
- body 上限 `frame::MAX_BODY = 4096`（`frame.h:43`），用于防御超大长度导致的内存耗尽攻击。
- 业务消息为 JSON（RapidJSON），字段见 `message_codec.h:115-131`。
- 保活：应用层 `Heartbeat` 消息（`message.h:27`）+ 60 秒空闲超时踢人（`session.h:38`、`session.cpp:75-88`）。
- 写入串行化：`std::deque` 写队列 + `async_write` 逐条发送（`session.cpp:351-376`）。
- 读写解耦：读完一条立刻继续读下一条（`session.cpp:67-68`）。

---

## 三、为什么选择 TCP 长连接而非 WebSocket

### 3.1 客户端是原生 C++，不是浏览器

WebSocket 诞生的首要动机是**让浏览器里的 JavaScript 获得全双工通信能力**。浏览器的 JS 运行时无法直接创建裸 TCP 连接，只能使用 HTTP/WebSocket/WebRTC 等被允许的 API。

本项目的客户端是**自研 C++ 桌面客户端**（见 `step2_test_client/*` 与仓库中的 Windows 客户端提交），可以自由使用 `socket` / Boost.Asio，不存在浏览器限制。此时 WebSocket 的"浏览器兼容"这一最大价值不再成立。

### 3.2 需要自定义分帧，且这正是项目的设计目标

`frame.h` 的注释大段解释了 TCP 粘包/半包问题以及为何要自己定义消息边界（`frame.h:4-22`）。结合工程目录 `step1_tcp_echo` / `step2_test_client` / `step3_verify`（含 `verify_tcp.sh` 粘包/半包验证），可以看出这是一个**分步推进、带教学与验证性质**的工程：自研分帧、心跳、重连本身就是设计与练习目标之一。

如果改用 WebSocket，分帧、掩码、心跳控制帧这些工作会被协议栈"藏起来"，也就失去了对协议细节的掌控与验证价值。

### 3.3 更低的每消息开销与更直接的延迟

| 项目 | 本项目（裸 TCP） | WebSocket |
|---|---|---|
| 应用层帧头 | 固定 4 字节 | 2–14 字节（取决于长度与扩展） |
| 客户端到服务端掩码 | 无 | 每帧必须做 XOR 掩码 |
| 建连握手 | 无额外握手，连上即可发 | HTTP Upgrade 握手 |
| 协议协商 | 无 | Sec-WebSocket-Key / Accept 等 |

对 IM 这类消息频繁、单条较小的场景，帧头与掩码的差异会被放大。裸 TCP 的 `4 字节长度 + JSON` 已经足够表达消息边界，没有引入额外标准头。

### 3.4 依赖与实现复杂度更低

服务端已经使用 Boost.Asio（`session.h:2`）。裸 TCP 直接基于 `ip::tcp::socket` 即可，无需再引入 WebSocket 协议实现。虽然项目已用 Boost.Beast 做 HTTP（`http_server.cpp:6-8`），Beast 也支持 WebSocket，但启用它意味着：

- 额外的握手/升级状态机与 header 校验；
- 帧层（分片、控制帧、Close 码、掩码）处理；
- 与现有"4 字节长度头"自定义协议不兼容，需要另起一套编解码分支。

对当前单线程 `io_context` 的简洁模型（`main.cpp:54`、`main.cpp:71`）而言，裸 TCP 的代码路径更短、更易读。

### 3.5 HTTP 与长连接职责清晰分离

项目把"认证类短交互"和"消息类长连接"分开：

- HTTP 短连接：注册、登录、返回 JWT（`http_server.cpp`）。
- TCP 长连接：登录后的一切实时行为（`session.cpp`）。

这已经获得了 WebSocket 常见的"复用 HTTP 端口"之外的好处，同时又避免了把两类流量耦合到同一协议栈上。认证令牌通过 HTTP 获取后再在 TCP 首包携带（`session.cpp:127`），流程自然。

### 3.6 认证方式与协议无关

JWT 是无状态的，放在 HTTP 响应体里返回（`http_server.cpp:64`），再在 TCP 的 `Login` 消息 `content` 中提交（`message.h:28`、`session.cpp:138`）。这一方式与传输协议解耦，换 WebSocket 也不会更简单，因此不构成选择 WebSocket 的理由。

---

## 四、两种方案对比总览

| 维度 | 裸 TCP + 自定义分帧（现状） | WebSocket |
|---|---|---|
| 浏览器原生支持 | 否 | 是 |
| 原生客户端支持 | 是 | 是 |
| 全双工 | 是（TCP 本身） | 是 |
| 消息边界 | 需自研（已有 `frame.h`） | 协议内置 |
| 每帧额外开销 | 低（4 字节头） | 中（2–14 字节头 + 掩码） |
| 建连成本 | 低 | 中（HTTP Upgrade） |
| 穿透仅放行 80/443 的代理 | 依赖端口/网络策略 | 较好 |
| 复用 HTTP 基础设施（CDN/七层 LB/WSS） | 需自行适配 | 天然契合 |
| 标准生态与调试工具 | 需自建 | 成熟（浏览器 DevTools 等） |
| 实现复杂度（本项目语境） | 低 | 中 |
| 协议可控性 | 高 | 受 RFC 6455 约束 |

**本项目在"浏览器支持、代理穿透、HTTP 基础设施复用"三项上均无强需求，而在"低开销、低复杂度、高可控性"三项上有明确收益，因此选择裸 TCP。**

---

## 五、选择裸 TCP 需要自行承担的代价

选择裸 TCP 并非"免费"，以下工作必须自己做，项目目前的对应实现如下：

| 自研项 | 说明 | 本项目实现 |
|---|---|---|
| 粘包/半包处理 | 必须定义消息边界 | 4 字节长度头（`frame.h`） |
| 心跳保活 | 检测半开连接 | `Heartbeat` 消息 + 60s 空闲超时（`session.cpp:75`） |
| 断线重连 | 客户端侧 | 客户端需求（见 `IMS_llm_prompt_word_info.txt` 第 18 行） |
| 鉴权 | 连接级身份 | JWT 验签（`session.cpp:127`） |
| 背压/写队列 | 避免并发写乱序 | `write_queue_` + 串行 `async_write`（`session.cpp:351`） |
| 加密 | 明文 TCP 不安全 | 目前未实现，后续需 TLS 或应用层加密 |

> 注意：这些能力 WebSocket 也只是"部分内置"（如帧、控制帧、Close），鉴权、重连、业务心跳、背压仍需应用层实现。因此换成 WebSocket 并不会消除大部分工作。

---

## 六、何时应当引入 WebSocket

出现以下需求时，应重新评估并优先考虑 WebSocket：

1. **需要 Web 前端 / 小程序 / 网页端直接接入消息**——浏览器无法使用裸 TCP，这是硬性约束。
2. **需要在严格的企业代理/防火墙环境下通信**——通常只放行 80/443 的 HTTP(S)，WebSocket 更容易穿行。
3. **希望复用现有 HTTP 基础设施**——CDN、七层负载均衡、WSS/TLS 卸载、基于 Host/Path 的路由。
4. **希望使用成熟的标准化工具链**——浏览器 DevTools、通用 WS 客户端、标准化的 ping/pong 与关闭码。
5. **多语言/多端接入且不想让每端都实现同一套私有分帧**。

---

## 七、演进建议：TCP 与 WebSocket 并存

若未来需要支持 Web 端，**不建议替换现有 TCP 长连接**，而是新增一个 WebSocket 入口，复用既有业务层：

```
                    ┌────────────────────────────┐
桌面客户端 ──TCP──> │                            │
                    │  SessionManager / Db / JWT │
Web/小程序 ──WS──>  │  （共用业务与会话路由）     │
                    └────────────────────────────┘
```

要点：

1. **抽象出传输无关的会话接口**：把 `session` 的读写与业务分发解耦，定义如 `IMessageChannel`（`send(Message)` / 回调 `onMessage`），TCP 与 WebSocket 各自实现。
2. **复用消息模型与编解码**：`Message` / `message_codec.h` 不变，WebSocket 直接承载 JSON 文本帧即可（不再需要 4 字节长度头，因为 WS 自带帧边界）。
3. **统一鉴权与在线表**：两种入口都走同一套 JWT 验签与 `SessionManager`，保证同一用户跨端登录/踢人的语义一致。
4. **统一心跳与超时策略**：WS 可用协议级 ping/pong，应用层 `Heartbeat` 仍保留以兼容两类客户端。
5. **部署层**：TCP 与 WS 可分端口，也可由反向代理在 443 上按路径分流到 WS 入口。

---

## 八、决策记录（ADR 简表）

| 项 | 内容 |
|---|---|
| 决策 | 实时消息通道采用 TCP 长连接 + 4 字节长度头 + JSON 自定义协议 |
| 状态 | 已采用（当前实现） |
| 背景 | 自研原生 C++ 客户端；需处理粘包/半包；追求低开销与高可控性；无浏览器接入需求 |
| 备选方案 | WebSocket（Boost.Beast）；HTTP 长轮询；MQTT 等 |
| 选择理由 | 无浏览器/代理穿透/HTTP 基建复用需求；裸 TCP 开销更低、依赖更少、协议完全可控；分帧自研是项目目标之一 |
| 代价 | 需自研分帧、心跳、重连、背压、加密等 |
| 触发重评估条件 | 需要 Web/小程序接入；需要穿行严格代理；需要复用 HTTP 生态基础设施 |
| 演进策略 | 新增 WebSocket 入口与 TCP 并存，复用业务层，而非替换 |

---

## 九、代码位置索引

| 主题 | 位置 |
|---|---|
| TCP 监听与接入 | `step1_tcp_echo/server.cpp:14-41` |
| 长度头协议说明 | `step1_tcp_echo/frame.h:4-43` |
| 长度编解码 | `step1_tcp_echo/frame.h:50-66` |
| 读头 / 读体 | `step1_tcp_echo/session.cpp:32-70` |
| 消息分发 | `step1_tcp_echo/session.cpp:93-120` |
| JWT 登录验签 | `step1_tcp_echo/session.cpp:127-168` |
| 发消息落库与路由 | `step1_tcp_echo/session.cpp:173-232` |
| 空闲超时 | `step1_tcp_echo/session.cpp:75-88` |
| 写队列串行化 | `step1_tcp_echo/session.cpp:351-376` |
| HTTP 注册/登录 | `step1_tcp_echo/http_server.cpp:108-174` |
| 消息类型定义 | `step1_tcp_echo/message.h:24-37` |
| JSON 编解码 | `step1_tcp_echo/message_codec.h:32-180` |
| 单 `io_context` 装配 | `step1_tcp_echo/main.cpp:54-71` |

---

## 十、结语

本项目选择裸 TCP 长连接，是在"客户端自控、无浏览器需求、追求轻量与可控"的前提下做出的合理工程取舍。它把复杂度集中在自己实现的少量、可验证的协议逻辑上（`frame.h` + `session`），换来了更低的每消息开销与更清晰的 HTTP/长连接职责划分。若未来接入 Web 端或多端生态，应以**并存演进**的方式引入 WebSocket，复用已经稳定的业务层，而不是推翻现有通道。
