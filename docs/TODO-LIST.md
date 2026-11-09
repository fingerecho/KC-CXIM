# KC-CXIM 项目完善 TODO List

> 适用范围：`dev/IMS`（自研 IM 服务端）、`dev/Admin`（Web 管理后台）、`dev/Desktop`（Windows 桌面端）、`dev/Mobile`（移动端）。
>
> 参考文档：
> - [`docs/KC-CXIM-IMS-DESIGN.md`](KC-CXIM-IMS-DESIGN.md)：TCP 长连接与 WebSocket 选型说明。
> - [`docs/total_comment.txt`](total_comment.txt) 第 15–80 行：现有后端的 P0/P1/P2 待办与上线路径。
> - [`docs/TIM_USAGE.md`](TIM_USAGE.md)：腾讯云 IM Windows SDK 能力清单。
> - [`readme.md`](../readme.md)：项目定位、架构与快速开始。

## 0. 标记与优先级

- `[x]` 已完成
- `[ ]` 待完成
- `[!]` 阻塞中
- **P0**：上线阻断项，没有就不能对外提供服务
- **P1**：正式上线前强烈建议完成
- **P2**：上线后迭代 / 体验优化
- **P3**：长期演进

任务编号规则：`BE` 后端、`ADM` 管理后台、`DESK` 桌面端、`MOB` 移动端、`OPS` 运维与跨端。

---

## 1. 现状基线

### 1.1 已完成

- [x] TCP 长连接：4 字节大端长度头 + JSON body，处理粘包/半包。
- [x] HTTP 短连接：`/api/register`、`/api/login`，Boost.Beast 实现。
- [x] 密码安全：bcrypt 哈希存储，登录时校验。
- [x] 无状态鉴权：HTTP 登录签发 JWT（HS256），TCP 登录验签。
- [x] MariaDB 持久化：`users`、`friends`、`conversations`、`conversation_members`、`messages`、`offline_messages`。
- [x] 单聊：会话去重键、服务端分配 `msg_id/seq/ts`、消息落库。
- [x] 在线投递与离线补推：`SessionManager` + `offline_messages`。
- [x] 历史消息：按 `conversation_id + seq` keyset 分页。
- [x] 回执：`last_delivered_seq`、`last_read_seq` 游标。
- [x] 消息去重：`(conversation_id, sender_id, client_msg_id)` 唯一键。
- [x] 基础可靠性：心跳保活、60 秒空闲超时、重复登录踢旧连接、写队列串行化。
- [x] DB 访问层：连接池 + Worker 线程，阻塞 SQL 不占用 io 线程。
- [x] 验收脚本：`verify_tcp.sh`、`verify_tcp_http.sh`。
- [x] Windows 桌面端：duilib + C++17，接入腾讯云 IM SDK。
- [x] 架构文档、README、现有协议与表结构说明。

### 1.2 核心缺口

- [ ] 生产级安全：TLS、限流、防重放、权限校验、日志脱敏。
- [ ] 多实例与水平扩展：在线状态、消息路由、跨节点投递。
- [ ] 群聊、频道、万人群。
- [ ] 富媒体消息：语音、视频、文件、自定义消息。
- [ ] 音视频通话与 WebRTC 信令。
- [ ] Web 管理后台。
- [ ] Electron 桌面端迁移。
- [ ] 移动端。
- [ ] 可观测性、CI/CD、压测与容量基线。

### 1.3 建议目标架构

```text
客户端（Electron / Flutter / Web）
        │  HTTP(S) + WebSocket(TLS)
        ▼
接入层：API 网关 + WS/TCP 网关 + 鉴权限流
        │  gRPC / 内部 HTTP
        ▼
业务层：用户 / 好友 / 会话 / 消息 / 群组 / 信令 / 推送
        │
        ├── Redis：在线状态、缓存、限流、分布式锁
        ├── Kafka/RocketMQ：异步持久化、扩散、推送
        ├── MariaDB：用户、关系、会话、消息、离线队列
        └── 对象存储 + CDN：图片、语音、视频、文件
```

---

## 2. 总体里程碑

| 阶段 | 目标 | 关键交付物 |
| --- | --- | --- |
| M0 基线加固 | 现有单聊服务可安全试运行 | TLS、限流、日志脱敏、多线程 io、CI、压测基线 |
| M1 自研 MVP | 单聊 + 文本 + 离线 + 已读 + WebSocket | WS 网关、Electron 客户端 beta、后台基础登录 |
| M2 群聊与富媒体 | 群聊、图片/语音/视频/文件、管理后台 | 群表与 API、上传服务、管理后台 |
| M3 多端与实时增强 | 多设备、推送、正在输入、撤回、音视频信令 | 设备表、推送、信令服务、WebRTC |
| M4 移动端与规模化 | Flutter 客户端、多实例、容量扩展 | 移动 App、Redis/Kafka、灰度与监控 |

---

## 3. 后端服务 TODO

> 本节重点参考 `docs/total_comment.txt` 第 15–80 行的 P0/P1/P2 清单，并补充消息类型、会话形态、实时能力与音视频能力。

### 3.0 与 total_comment.txt 15–80 行的对应关系

| total_comment.txt 主题 | 本清单对应章节 |
| --- | --- |
| 身份认证与授权 | 3.1 BE-AUTH-01 ~ BE-AUTH-08 |
| 持久化与存储 | 3.9、3.2 BE-REL-07 ~ BE-REL-10 |
| 传输安全 | 3.1 BE-SEC-01 ~ BE-SEC-06 |
| 协议与输入健壮性 | 3.1 BE-PROTO-01 ~ BE-PROTO-07 |
| 可靠性与并发正确性 | 3.2 BE-REL-01 ~ BE-REL-10 |
| 可观测性与运维 | 3.3 BE-OPS-01 ~ BE-OPS-09 |
| 扩展性 / 多实例 | 3.4 BE-SCALE-01 ~ BE-SCALE-07 |
| 功能完整度 | 3.5 消息类型、3.6 会话形态、3.7 实时能力、3.8 音视频 |
| 测试与质量 | 3.10、8 测试与质量门禁 |
| 安全加固 | 3.1、3.10 BE-SEC-07 ~ BE-SEC-08 |
| P2 上线后迭代 | 3.7、3.9、9 建议执行顺序 |
| 最小可上线路径（MVP） | 3.11、9 建议执行顺序 |

### 3.1 P0：认证、安全与协议

- [x] **BE-AUTH-01** HTTP 注册/登录 + bcrypt + JWT（已完成）。
- [x] **BE-AUTH-02** TCP 登录验签，身份以 JWT `sub` 为准（已完成）。
- [ ] **BE-AUTH-03** Token 刷新与吊销：短期 access token + refresh token，支持单设备/全设备登出。
- [ ] **BE-AUTH-04** 设备绑定与登录策略：设备表、设备指纹、同端互踢/多端共存策略。
- [ ] **BE-AUTH-05** 账号状态：封禁、解禁、注销、密码重置、登录失败次数限制。
- [ ] **BE-AUTH-06** 未登录连接超时：连接后限定时间内必须完成登录，不能靠心跳永久挂住。
- [ ] **BE-AUTH-07** 登录验证码/风控：异常 IP、异常频率触发验证码或二次验证。
- [ ] **BE-AUTH-08** 权限校验：只能给好友/群成员发消息，禁止越权拉取历史和会话。
- [ ] **BE-SEC-01** TCP 全链路 TLS：`asio::ssl::stream`，证书加载、SNI、证书轮换。
- [ ] **BE-SEC-02** WebSocket 使用 WSS；HTTP 管理端启用 HTTPS。
- [ ] **BE-SEC-03** 日志脱敏：禁止把消息正文、密码、token 打进日志。
- [ ] **BE-SEC-04** 敏感字段加密存储：手机号、邮箱、第三方账号等按需加密。
- [ ] **BE-SEC-05** 防重放：nonce + 时间窗 + 签名，重点覆盖登录与高风险操作。
- [ ] **BE-SEC-06** 限流与防刷：按 IP、用户、连接、消息类型、会话维度限流。
- [ ] **BE-PROTO-01** 协议版本字段：`version`，定义向后兼容与灰度策略。
- [ ] **BE-PROTO-02** 消息信封 v2：统一 `version/client_msg_id/type/ts/seq/attrs` 等字段。
- [ ] **BE-PROTO-03** 合理设置 body 上限：文本与富媒体分离，图片/文件只传 URL。
- [ ] **BE-PROTO-04** 完整字段校验：`to==0`、自己发自己、content 长度、非法枚举、未知类型。
- [ ] **BE-PROTO-05** 统一错误码与错误回执：`code`、`message`、`request_id`、可重试标记。
- [ ] **BE-PROTO-06** 协议文档：字段、时序图、错误码、版本变更记录。
- [ ] **BE-PROTO-07** 协议 fuzz 测试：畸形帧、超长 body、非法 JSON、深嵌套。

### 3.2 P0：可靠性与并发正确性

- [ ] **BE-REL-01** 多线程 `io_context`：线程池 + 每连接 `strand`，保证 session 状态与写队列无数据竞争。
- [ ] **BE-REL-02** 写队列上限与背压：慢客户端达到阈值后断开或降级，避免内存耗尽。
- [ ] **BE-REL-03** 异步回调异常兜底：回调内 `try/catch`，避免进程 `terminate`。
- [ ] **BE-REL-04** 优雅停机：SIGINT/SIGTERM 停止 accept、排空连接、落库、关闭连接池。
- [ ] **BE-REL-05** 连接数与单 IP 限制：防连接洪泛、防登录爆破、防消息刷屏。
- [ ] **BE-REL-06** 心跳确认：服务端回 pong，客户端可测 RTT；检测半开连接。
- [ ] **BE-REL-07** 可靠投递：至少一次 + ACK + 重试 + 幂等；离线队列与在线投递统一语义。
- [ ] **BE-REL-08** 消息状态机：`sending/sent/delivered/read/failed/recalled/deleted`，客户端可补偿。
- [ ] **BE-REL-09** Outbox/事务一致性：消息落库、离线入队、回执更新明确事务边界。
- [ ] **BE-REL-10** 慢查询治理：索引、慢日志、超时、连接池健康检查与断线重连。

### 3.3 P0：可观测性与运维

- [ ] **BE-OPS-01** 正式日志：spdlog/glog，分级、滚动、结构化、traceId/requestId。
- [ ] **BE-OPS-02** 关键指标：在线数、连接数、消息 TPS、延迟 P50/P95/P99、错误率、队列积压。
- [ ] **BE-OPS-03** 健康检查：`/healthz`、`/readyz`、DB/Redis/MQ 依赖检查。
- [ ] **BE-OPS-04** 监控告警：Prometheus + Grafana + Alertmanager。
- [ ] **BE-OPS-05** 崩溃兜底：core dump、panic 日志、进程守护、自动重启。
- [ ] **BE-OPS-06** 配置外置：端口、超时、DB、TLS、限额、功能开关统一配置并校验。
- [ ] **BE-OPS-07** 容器化：Dockerfile、docker-compose、systemd 单元。
- [ ] **BE-OPS-08** CI：构建、单测、集成测试、验收脚本、静态检查、镜像构建。
- [ ] **BE-OPS-09** 可复现构建：`find_package(Boost)` 或 vcpkg manifest，固定依赖版本。

### 3.4 P1：扩展性与多实例

- [ ] **BE-SCALE-01** 在线状态外置：Redis 保存 `userId -> gateway/connection`，支持多实例。
- [ ] **BE-SCALE-02** 网关与业务分离：接入层无状态，业务层可水平扩容。
- [ ] **BE-SCALE-03** 跨节点投递：Redis Pub/Sub 或 Kafka/RabbitMQ。
- [ ] **BE-SCALE-04** 服务注册与发现：Consul/Nacos；配置中心可选 Nacos Config。
- [ ] **BE-SCALE-05** API 网关：HTTP/WS 统一入口，鉴权、路由、限流、灰度。
- [ ] **BE-SCALE-06** 消息队列：消息写入后异步持久化、推送、扩散、审计。
- [ ] **BE-SCALE-07** 容量规划：单机连接数、消息 TPS、DB 写入、对象存储带宽基线。

### 3.5 消息类型 TODO

> 当前协议：`Text=1`、`Image=2`、`Heartbeat=3`、`Login=100`、`LoginAck=101`、`Error=102`、`HistoryReq=110`、`HistoryResp=111`、`Ack=112`、`SendAck=113`。新增类型必须避开现有值，并写入协议文档。

#### 3.5.1 建议消息类型表

| 值 | 类型 | 状态 | content 结构建议 |
| --- | --- | --- | --- |
| 1 | Text | 已支持 | `{"text":"...","mentions":[...],"reply_to":...}` |
| 2 | Image | 协议预留 | `{"url":"...","thumb":"...","width":0,"height":0,"size":0}` |
| 3 | Heartbeat | 已支持 | 空或 `{"ts":...}` |
| 4 | Voice | 待实现 | `{"url":"...","duration":0,"format":"opus","waveform":[...]}` |
| 5 | Video | 待实现 | `{"url":"...","cover":"...","duration":0,"width":0,"height":0}` |
| 6 | File | 待实现 | `{"url":"...","name":"...","size":0,"mime":"...","hash":"..."}` |
| 7 | Custom | 待实现 | `{"business_type":"...","payload":{...},"version":1}` |
| 8 | Location | 待实现 | `{"lat":0,"lng":0,"title":"...","address":"..."}` |
| 9 | Sticker | 待实现 | `{"sticker_id":"...","url":"...","package_id":"..."}` |
| 10 | System | 待实现 | `{"event":"...","text":"...","params":{...}}` |
| 11 | RichText/Card | 待实现 | `{"title":"...","elements":[...]}` |
| 100–113 | 控制/会话类型 | 部分支持 | 已使用 100–102、110–113，其余预留 |
| 120 | Recall | 待实现 | `{"msg_id":...,"reason":"..."}` |
| 121 | RecallAck | 待实现 | `{"msg_id":...,"code":0}` |
| 122 | Typing | 待实现 | `{"conversation_id":...,"action":"typing\|stop"}` |
| 123 | Presence | 待实现 | `{"status":"online\|offline","last_seen":...}` |
| 124 | ReadReceipt | 待实现 | `{"conversation_id":...,"seq":...}` |

#### 3.5.2 文本消息增强

- [ ] **BE-MSG-01** `@` 提及与 `mentions` 列表。
- [ ] **BE-MSG-02** 引用回复 `reply_to`、消息摘要。
- [ ] **BE-MSG-03** 转发、合并转发、多选转发。
- [ ] **BE-MSG-04** 消息编辑（可选）、删除、撤回、置顶、收藏。
- [ ] **BE-MSG-05** 表情/贴纸、富文本、Markdown 安全渲染。
- [ ] **BE-MSG-06** 敏感词过滤与内容审核钩子。
- [ ] **BE-MSG-07** 消息搜索（按会话、发送者、时间、关键词）。

#### 3.5.3 图片消息

- [ ] **BE-IMG-01** 对象存储上传：预签名 URL / 服务端中转，禁止把二进制塞进消息体。
- [ ] **BE-IMG-02** 元数据：宽高、大小、格式、缩略图、BlurHash。
- [ ] **BE-IMG-03** 压缩、EXIF 清理、水印（按产品需求）。
- [ ] **BE-IMG-04** MIME 白名单、大小限制、病毒扫描。
- [ ] **BE-IMG-05** CDN 加速与防盗链。

#### 3.5.4 语音消息

- [ ] **BE-VOICE-01** 音频格式与转码：Opus/AAC，采样率、码率约定。
- [ ] **BE-VOICE-02** 时长、波形、文件大小、上传状态。
- [ ] **BE-VOICE-03** 语音转文字（可选，第三方 ASR）。
- [ ] **BE-VOICE-04** 播放鉴权与 URL 过期策略。

#### 3.5.5 视频消息

- [ ] **BE-VIDEO-01** 上传、转码、封面、时长、宽高、码率。
- [ ] **BE-VIDEO-02** HLS/DASH 播放地址，或对象存储直链 + CDN。
- [ ] **BE-VIDEO-03** 大文件分片上传、断点续传。
- [ ] **BE-VIDEO-04** 视频审核与截图抽帧。

#### 3.5.6 文件消息

- [ ] **BE-FILE-01** 文件名、大小、MIME、哈希（SHA-256）、下载 URL。
- [ ] **BE-FILE-02** 分片上传、断点续传、秒传。
- [ ] **BE-FILE-03** 下载鉴权、有效期、防盗链。
- [ ] **BE-FILE-04** 病毒扫描、可执行文件拦截、配额限制。

#### 3.5.7 自定义消息

- [ ] **BE-CUSTOM-01** 业务类型命名空间：如 `order`、`red_packet`、`approval`。
- [ ] **BE-CUSTOM-02** payload 版本化与向后兼容。
- [ ] **BE-CUSTOM-03** 业务方鉴权与回调校验。
- [ ] **BE-CUSTOM-04** 未知自定义消息的降级展示。

#### 3.5.8 消息生命周期与操作

- [ ] **BE-MSG-08** 客户端临时消息号与服务端 `msg_id` 映射。
- [ ] **BE-MSG-09** 发送中/成功/失败/重试 UI 状态与协议支持。
- [ ] **BE-MSG-10** 本地消息与服务端消息去重、排序、补偿。
- [ ] **BE-MSG-11** 消息已读、投递、撤回事件的跨端同步。

### 3.6 会话形态 TODO

#### 3.6.1 单聊

- [x] **BE-CONV-01** 单聊会话去重与复用（已完成）。
- [ ] **BE-CONV-02** 会话列表：最后一条消息、未读数、置顶、免打扰、草稿。
- [ ] **BE-CONV-03** 会话设置：清空、删除、标记已读、消息免打扰。
- [ ] **BE-CONV-04** 黑名单/拉黑与消息拦截。
- [ ] **BE-CONV-05** 好友关系校验与陌生人消息策略。

#### 3.6.2 群聊

- [ ] **BE-GROUP-01** 群表与成员表：`groups`、`group_members`、`group_requests`、`group_invites`。
- [ ] **BE-GROUP-02** 建群、解散、转让群主、退出群聊。
- [ ] **BE-GROUP-03** 邀请入群、申请入群、审批、拒绝。
- [ ] **BE-GROUP-04** 角色与权限：群主、管理员、普通成员。
- [ ] **BE-GROUP-05** 群公告、群昵称、群头像、群简介、群二维码。
- [ ] **BE-GROUP-06** 全员禁言、单人禁言、踢人、黑名单。
- [ ] **BE-GROUP-07** 群消息扩散：在线成员直推、离线成员入队。
- [ ] **BE-GROUP-08** 群已读回执与已读人数统计。
- [ ] **BE-GROUP-09** `@` 全员、`@` 指定成员、群消息搜索。
- [ ] **BE-GROUP-10** 群文件、群相册、群任务（按产品裁剪）。

#### 3.6.3 频道 / 房间

- [ ] **BE-CHANNEL-01** 频道模型：订阅关系、管理员、内容发布权限。
- [ ] **BE-CHANNEL-02** 广播消息与订阅者扩散策略。
- [ ] **BE-CHANNEL-03** 历史可见性策略：新成员可看最近 N 条 / 全部 / 不可见。
- [ ] **BE-CHANNEL-04** 频道消息审核与运营工具。

#### 3.6.4 万人群 / 大群

- [ ] **BE-LARGE-01** 写扩散 vs 读扩散选型：按群规模切换策略。
- [ ] **BE-LARGE-02** 在线成员 fanout：只推在线，离线用游标补拉。
- [ ] **BE-LARGE-03** 群成员分片、消息分片、热点群隔离。
- [ ] **BE-LARGE-04** Kafka/RocketMQ 异步扩散与削峰。
- [ ] **BE-LARGE-05** 大群限流、慢用户降级、消息优先级。
- [ ] **BE-LARGE-06** 大群容量压测与容量基线。

### 3.7 实时能力 TODO

- [ ] **BE-RT-01** 在线状态：在线/离线/离开，最后在线时间，多设备聚合。
- [ ] **BE-RT-02** 在线状态订阅：好友/群成员在线状态推送。
- [ ] **BE-RT-03** 正在输入：`Typing=122`，客户端节流，服务端过期丢弃。
- [x] **BE-RT-04** 投递回执与已读回执（已完成基础版）。
- [ ] **BE-RT-05** 群已读回执：已读人数、已读成员列表（按需求）。
- [ ] **BE-RT-06** 消息撤回：`Recall=120`、`RecallAck=121`，时限与权限校验。
- [ ] **BE-RT-07** 撤回事件跨端同步：本地删除 + 墓碑消息 + 重新拉取。
- [ ] **BE-RT-08** 多端消息漫游：设备表、同步游标、消息补拉。
- [ ] **BE-RT-09** 多端已读同步：一端已读，其他端同步游标。
- [ ] **BE-RT-10** 多端在线策略：同端互踢、多端共存、消息多端投递。
- [ ] **BE-RT-11** 离线推送：APNs、FCM、华为/小米/OPPO/vivo 等厂商通道。
- [ ] **BE-RT-12** 推送模板、角标、深链、免打扰与推送开关。
- [ ] **BE-RT-13** 消息补偿：断线重连后按 `seq` 补拉，处理空洞与重复。
- [ ] **BE-RT-14** 实时事件总线：在线/输入/已读/撤回/群变更统一事件模型。

### 3.8 音视频能力 TODO

> 原则：IM 长连接只负责信令，音视频媒体流走 WebRTC，不经过 TCP IM 网关。

- [ ] **BE-AV-01** 信令协议：呼叫邀请、接听、拒绝、取消、挂断、忙线、超时。
- [ ] **BE-AV-02** 信令消息类型：`CallInvite/CallAccept/CallReject/CallCancel/CallHangup/CallBusy/CallTimeout`。
- [ ] **BE-AV-03** 1v1 通话：SDP Offer/Answer、ICE Candidate 交换。
- [ ] **BE-AV-04** 群通话：SFU 架构（LiveKit / mediasoup / Janus）或第三方 RTC。
- [ ] **BE-AV-05** STUN/TURN：coturn 部署、NAT 穿透、中继带宽规划。
- [ ] **BE-AV-06** 媒体能力：音频、视频、屏幕共享、静音、关闭摄像头、切换设备。
- [ ] **BE-AV-07** 通话状态机、超时、重连、弱网降级。
- [ ] **BE-AV-08** 通话记录、未接来电、通话时长统计。
- [ ] **BE-AV-09** 录制、截图、虚拟背景、美颜（按产品裁剪）。
- [ ] **BE-AV-10** 安全：通话 token、房间鉴权、防骚扰、E2EE（可选）。
- [ ] **BE-AV-11** 监控：通话成功率、建立时延、卡顿率、丢包率、MOS。

### 3.9 存储与中间件 TODO

#### 3.9.1 MariaDB 表结构扩展

- [ ] **BE-DB-01** `devices`：设备 ID、平台、推送 token、最后活跃时间。
- [ ] **BE-DB-02** `groups`、`group_members`、`group_requests`、`group_invites`。
- [ ] **BE-DB-03** `user_settings`：通知、隐私、免打扰、主题等。
- [ ] **BE-DB-04** `message_receipts` 或继续使用游标方案（按群规模评估）。
- [ ] **BE-DB-05** `recalls`、`message_reactions`、`message_edits`。
- [ ] **BE-DB-06** `uploads`：对象存储文件元数据、归属、状态、过期时间。
- [ ] **BE-DB-07** `push_tokens`、`push_logs`。
- [ ] **BE-DB-08** `admin_users`、`admin_roles`、`audit_logs`、`moderation_records`。
- [ ] **BE-DB-09** `outbox` / `event_log`：可靠投递与异步事件。
- [ ] **BE-DB-10** 索引与分区：大表按时间/会话分区，慢查询治理。
- [ ] **BE-DB-11** 数据保留与清理：消息保留周期、离线消息过期、归档。
- [ ] **BE-DB-12** 备份恢复：全量 + binlog PITR，定期恢复演练。

#### 3.9.2 Redis

- [ ] **BE-REDIS-01** 在线状态、连接路由。
- [ ] **BE-REDIS-02** 会话缓存、用户资料缓存、群成员缓存。
- [ ] **BE-REDIS-03** 限流、验证码、防重放 nonce。
- [ ] **BE-REDIS-04** 分布式锁、排行榜、计数器。
- [ ] **BE-REDIS-05** Pub/Sub 跨节点事件。

#### 3.9.3 消息队列

- [ ] **BE-MQ-01** 消息持久化异步化。
- [ ] **BE-MQ-02** 离线推送异步化。
- [ ] **BE-MQ-03** 群消息扩散异步化。
- [ ] **BE-MQ-04** 审计、统计、内容审核异步化。
- [ ] **BE-MQ-05** 死信队列、重试、幂等消费。

#### 3.9.4 对象存储

- [ ] **BE-OSS-01** MinIO（自建）/ 腾讯云 COS / S3 兼容存储。
- [ ] **BE-OSS-02** 预签名上传、服务端回调、文件元数据入库。
- [ ] **BE-OSS-03** 图片/视频处理：缩略图、转码、截图、压缩。
- [ ] **BE-OSS-04** CDN、防盗链、下载鉴权、生命周期清理。

### 3.10 测试与安全评审

- [ ] **BE-TEST-01** 单元测试：frame、codec、SessionManager、DAO。
- [ ] **BE-TEST-02** 集成测试：登录、转发、离线补推、重连、超时、重复登录。
- [ ] **BE-TEST-03** 端到端测试：HTTP 注册登录 + TCP 双用户聊天。
- [ ] **BE-TEST-04** 协议 fuzz：畸形帧、超长、非法 JSON、深嵌套。
- [ ] **BE-TEST-05** 压测：连接数、消息 TPS、长稳测试、容量基线。
- [ ] **BE-TEST-06** ASan/UBSan、clang-tidy、静态分析。
- [ ] **BE-TEST-07** CI 中运行验收脚本，而非手工执行。
- [ ] **BE-SEC-07** 安全评审：限流、防刷、验证码、防重放、权限、依赖漏洞。
- [ ] **BE-SEC-08** 渗透测试与协议安全评审。

### 3.11 后端 MVP 验收标准

- [ ] 单聊文本消息在线实时送达。
- [ ] 离线消息落库、上线补推、ACK 后删除。
- [ ] 历史消息按 `seq` 分页正确。
- [ ] 重复 `client_msg_id` 不产生重复消息。
- [ ] JWT 过期/伪造/越权请求被拒绝。
- [ ] TLS 开启后客户端可正常连接。
- [ ] 慢客户端不会拖垮服务，写队列有上限。
- [ ] 多线程 io + strand 下无数据竞争。
- [ ] 健康检查、指标、日志可观测。
- [ ] 压测达到既定连接数与 TPS 基线。

---

## 4. Web 管理后台 TODO

### 4.1 目标

为运营与管理员提供 B/S 控制台，覆盖用户、群组、消息、内容审核、推送、统计、系统配置与审计。管理后台与用户端账号体系分离，默认独立域名、独立鉴权、独立权限模型。

### 4.2 C++ 后端框架选型

| 框架 | 优点 | 不足 | 适用性 |
| --- | --- | --- | --- |
| **Drogon**（推荐） | C++17/20、HTTP/1.1/2、WebSocket、ORM、插件、协程、异步、跨平台、MIT | 需要引入新依赖，团队需学习 | 最适合管理后台 REST/WS，开发效率高 |
| oatpp | 现代 C++、异步、API 文档/校验 | ORM 与生态相对小 | 可选 |
| Crow | 轻量、路由简单 | 功能与生态有限 | 小型后台可选 |
| Pistache | REST 风格清晰 | 社区活跃度一般 | 不优先 |
| cpp-httplib | header-only、上手快 | 生产级能力弱 | 原型/内部工具 |
| Boost.Beast | 已在现有技术栈、底层可控 | 手写路由/中间件成本高 | 不想引入新依赖时复用 |
| userver | 高性能、异步、框架完整 | 学习曲线与重量级 | 大规模团队可选 |

**推荐方案**：

- 管理后台后端采用 **Drogon**，作为独立进程部署，不与 IM 长连接服务混部。
- 数据库继续使用 MariaDB；Drogon ORM 可用于管理端 CRUD，复杂统计走手写 SQL。
- 管理端与用户端共享表结构，但使用独立的数据库账号与权限，生产环境建议只读账号 + 写账号分离。
- 如果团队希望零新增依赖，可继续使用 Boost.Beast 实现管理 API，但开发效率低于 Drogon。
- 前端采用 **Vue 3 + TypeScript + Vite + Pinia + Vue Router + Element Plus + ECharts + Axios**。

### 4.3 后端模块

- [ ] **ADM-BE-01** 管理员登录：账号密码 + bcrypt/Argon2 + JWT，独立于用户端。
- [ ] **ADM-BE-02** RBAC：超级管理员、管理员、运营、审核员、客服、只读审计。
- [ ] **ADM-BE-03** 二次验证：TOTP/短信/邮箱验证码，高风险操作二次确认。
- [ ] **ADM-BE-04** 登录安全：IP 白名单、失败锁定、验证码、会话超时。
- [ ] **ADM-BE-05** 用户管理：查询、详情、封禁/解禁、重置密码、注销、标签。
- [ ] **ADM-BE-06** 好友关系管理：查询、解除、黑名单。
- [ ] **ADM-BE-07** 群组管理：查询、解散、转让、成员管理、禁言。
- [ ] **ADM-BE-08** 会话与消息查询：按用户/会话/时间/类型检索，支持导出。
- [ ] **ADM-BE-09** 消息撤回/删除：权限校验、操作留痕。
- [ ] **ADM-BE-10** 内容审核：敏感词库、举报处理、违规内容下架。
- [ ] **ADM-BE-11** 推送管理：模板、人群、定时推送、推送记录。
- [ ] **ADM-BE-12** 数据统计：DAU/MAU、在线峰值、消息量、群量、留存。
- [ ] **ADM-BE-13** 系统配置：功能开关、限流参数、公告、版本配置。
- [ ] **ADM-BE-14** 审计日志：谁在什么时间做了什么，不可篡改/可导出。
- [ ] **ADM-BE-15** 文件管理：对象存储文件查询、清理、配额。
- [ ] **ADM-BE-16** 开放 API：为运营工具提供受控接口，签名 + 限流。

### 4.4 API 设计建议

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/admin/api/auth/login` | 管理员登录，返回 access/refresh token |
| POST | `/admin/api/auth/refresh` | 刷新 token |
| POST | `/admin/api/auth/logout` | 登出并吊销 token |
| GET | `/admin/api/users` | 用户分页查询 |
| GET | `/admin/api/users/{id}` | 用户详情 |
| PATCH | `/admin/api/users/{id}/status` | 封禁/解禁 |
| GET | `/admin/api/groups` | 群组分页查询 |
| GET | `/admin/api/messages` | 消息检索 |
| POST | `/admin/api/messages/{id}/recall` | 撤回消息 |
| GET | `/admin/api/reports` | 举报列表 |
| PATCH | `/admin/api/reports/{id}` | 处理举报 |
| GET | `/admin/api/stats/overview` | 核心指标 |
| GET | `/admin/api/audit-logs` | 审计日志 |
| GET | `/admin/api/configs` | 系统配置 |
| PUT | `/admin/api/configs/{key}` | 更新配置 |

统一响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {},
  "request_id": "..."
}
```

### 4.5 前端功能

- [ ] **ADM-FE-01** 登录页、找回密码、二次验证。
- [ ] **ADM-FE-02** 主布局：侧边栏、面包屑、标签页、主题切换。
- [ ] **ADM-FE-03** 仪表盘：在线数、消息量、DAU、趋势图、告警。
- [ ] **ADM-FE-04** 用户管理：表格、筛选、详情抽屉、批量操作。
- [ ] **ADM-FE-05** 群组管理：群详情、成员列表、禁言/解散。
- [ ] **ADM-FE-06** 消息查询：多条件检索、消息详情、撤回。
- [ ] **ADM-FE-07** 内容审核：举报队列、敏感词、审核记录。
- [ ] **ADM-FE-08** 推送管理：模板编辑、人群选择、定时任务。
- [ ] **ADM-FE-09** 系统配置：表单、开关、版本管理。
- [ ] **ADM-FE-10** 审计日志：筛选、导出。
- [ ] **ADM-FE-11** 权限控制：路由守卫、按钮级权限、菜单动态生成。
- [ ] **ADM-FE-12** 国际化、暗色模式、可访问性。
- [ ] **ADM-FE-13** 错误处理：统一请求拦截、401 跳转、重试、提示。

### 4.6 安全与部署

- [ ] **ADM-SEC-01** 管理后台独立域名、HTTPS、WAF/限流。
- [ ] **ADM-SEC-02** JWT 放 HttpOnly Cookie 或内存 + 短期有效，防 XSS 窃取。
- [ ] **ADM-SEC-03** CSRF 防护（Cookie 模式）、CSP、X-Frame-Options。
- [ ] **ADM-SEC-04** SQL 注入防护：参数化查询、ORM 白名单。
- [ ] **ADM-SEC-05** 操作审计与不可抵赖。
- [ ] **ADM-SEC-06** 敏感数据脱敏展示、导出审批。
- [ ] **ADM-OPS-01** Nginx 托管前端静态资源，反向代理 Drogon API。
- [ ] **ADM-OPS-02** Docker Compose：admin-backend、admin-frontend、MariaDB、Redis。
- [ ] **ADM-OPS-03** CI：前端构建、后端构建、单测、镜像。
- [ ] **ADM-OPS-04** 灰度发布与回滚。

### 4.7 管理后台验收标准

- [ ] 管理员可登录、登出、刷新 token。
- [ ] RBAC 生效，低权限角色无法访问高权限接口。
- [ ] 用户封禁后无法登录/发消息。
- [ ] 消息检索与撤回有审计记录。
- [ ] 举报处理闭环。
- [ ] 仪表盘数据与数据库统计一致。
- [ ] 前后端均通过安全扫描与基础压测。

---

## 5. Windows 桌面端 TODO（迁移到 Electron）

### 5.1 现状

- duilib + C++17 + MSVC/CMake/vcpkg。
- 通过 `ImSDK.dll` 接入腾讯云 IM，完成登录、好友、单聊、历史消息。
- 已实现登录窗、主界面、会话列表、聊天窗口、日志、部署包。

### 5.2 迁移前必须解决的通信问题

Electron 的渲染进程是浏览器环境，**不能直接创建裸 TCP 连接**。有两种方案：

| 方案 | 做法 | 优点 | 缺点 | 建议 |
| --- | --- | --- | --- | --- |
| A. 主进程 TCP | Electron 主进程用 Node.js `net` 连接现有 `:8888`，按 4 字节长度分帧，通过 IPC 暴露给渲染进程 | 不改后端 | 只能在 Electron 用，Web 端无法复用；IPC 需严格校验 | 过渡方案 |
| B. 新增 WebSocket 网关 | 后端增加 WS/WSS 入口，复用 JSON 消息模型，WS 自带帧边界 | Web/Electron/移动端可复用；穿透代理友好 | 需要后端开发 | **推荐** |

**推荐顺序**：先做 WebSocket 网关（同时服务 Web 与 Electron），再迁移 Electron；过渡期可用方案 A 保持桌面端可用。

### 5.3 目标技术栈

| 层 | 选型 |
| --- | --- |
| 桌面容器 | Electron（主进程 + preload + renderer） |
| 前端框架 | Vue 3 + TypeScript + Vite |
| 状态与路由 | Pinia + Vue Router |
| UI | Element Plus / Naive UI + Tailwind CSS |
| 本地缓存 | better-sqlite3 / electron-store |
| 日志 | electron-log |
| 更新 | electron-updater + electron-builder |
| 实时通信 | WebSocket（推荐）/ Node `net`（过渡） |
| 音视频 | WebRTC + getUserMedia / getDisplayMedia |
| 图片处理 | sharp |
| 安全存储 | Electron `safeStorage` / keytar |

### 5.4 迁移阶段

- [ ] **DESK-01** 功能盘点：列出现有 duilib 客户端全部页面、交互、SDK 调用。
- [ ] **DESK-02** 协议适配层：把腾讯云 IM SDK 调用封装为接口，便于替换为自研协议。
- [ ] **DESK-03** 后端 WebSocket 网关：WSS、鉴权、心跳、消息收发、历史、回执。
- [ ] **DESK-04** Electron 脚手架：主进程/preload/renderer 分层，开发/生产构建。
- [ ] **DESK-05** 安全基线：`contextIsolation`、`sandbox`、禁用 `nodeIntegration`、CSP、IPC 白名单。
- [ ] **DESK-06** 登录与token 管理：安全存储、刷新、登出、自动登录。
- [ ] **DESK-07** 会话列表：最后消息、未读、置顶、免打扰、搜索。
- [ ] **DESK-08** 联系人/好友：列表、申请、备注、黑名单。
- [ ] **DESK-09** 单聊窗口：文本、表情、引用、转发、撤回、已读。
- [ ] **DESK-10** 群聊：群列表、群成员、@、群公告、群设置。
- [ ] **DESK-11** 富媒体：图片、语音、视频、文件、拖拽上传、粘贴截图。
- [ ] **DESK-12** 历史消息：分页、跳转、搜索、本地缓存。
- [ ] **DESK-13** 实时能力：在线状态、正在输入、已读回执、撤回。
- [ ] **DESK-14** 离线与重连：本地消息队列、断线重连、补拉。
- [ ] **DESK-15** 系统集成：托盘、通知、任务栏角标、快捷键、开机启动。
- [ ] **DESK-16** 多窗口：独立聊天窗口、截图窗口。
- [ ] **DESK-17** 音视频：1v1 通话、群通话、屏幕共享、设备切换。
- [ ] **DESK-18** 自动更新：增量更新、签名校验、回滚。
- [ ] **DESK-19** 崩溃与日志：崩溃上报、日志导出、诊断信息。
- [ ] **DESK-20** 打包发布：NSIS/MSI、代码签名、安装/卸载体验。

### 5.5 功能清单

- [ ] 登录/登出/自动登录/Token 刷新。
- [ ] 会话列表、未读数、置顶、免打扰、草稿。
- [ ] 好友列表、好友申请、备注、黑名单。
- [ ] 单聊、群聊、频道（后续）。
- [ ] 文本、表情、图片、语音、视频、文件、自定义消息。
- [ ] 消息引用、转发、撤回、删除、已读、搜索。
- [ ] 在线状态、正在输入、离线补推。
- [ ] 通知、托盘、角标、快捷键。
- [ ] 音视频通话、屏幕共享。
- [ ] 设置：主题、语言、通知、隐私、存储。
- [ ] 多账号/多窗口（可选）。
- [ ] 自动更新、崩溃恢复。

### 5.6 安全与性能

- [ ] **DESK-SEC-01** 渲染进程不直接访问 Node/文件系统，全部经 preload IPC。
- [ ] **DESK-SEC-02** IPC 参数校验、来源校验、最小权限。
- [ ] **DESK-SEC-03** CSP、禁用远程代码、依赖审计。
- [ ] **DESK-SEC-04** 代码签名、更新包签名校验。
- [ ] **DESK-PERF-01** 虚拟列表、图片懒加载、消息分页。
- [ ] **DESK-PERF-02** SQLite 索引、缓存策略、启动优化。
- [ ] **DESK-PERF-03** 内存与 CPU 监控，长时间运行稳定性。

### 5.7 桌面端验收标准

- [ ] 登录后能收发文本消息，离线消息可补推。
- [ ] 断网重连后消息不丢、不重复。
- [ ] 图片/文件上传下载正常，进度可见。
- [ ] 通知、托盘、角标正常。
- [ ] 自动更新可完成升级与回滚。
- [ ] 安全基线检查通过（无 `nodeIntegration`、CSP 生效）。
- [ ] 安装包在 Windows 10/11 可正常安装、卸载。

### 5.8 Electron 备选方案

- [ ] **DESK-ALT-01** 评估 Tauri：包体更小、内存更低，但 Rust 技术栈与 WebRTC 生态需验证。
- [ ] **DESK-ALT-02** 评估继续维护 duilib：性能好、包体小，但开发效率与生态弱于 Electron。

---

## 6. 移动端 TODO（放在最后）

### 6.1 技术选型

| 方案 | 优点 | 不足 | 建议 |
| --- | --- | --- | --- |
| **Flutter**（推荐） | 跨平台、性能好、UI 一致、生态成熟 | Dart 学习成本 | 与项目原规划一致，优先 |
| React Native | JS/TS 生态、前端团队易上手 | 原生模块与性能调优成本 | 可选 |
| uni-app | 多端覆盖快 | 复杂 IM/音视频能力受限 | 不优先 |

**推荐**：Flutter 3 + Dart，复用后端 WebSocket/HTTP 协议与统一消息模型。

### 6.2 目标技术栈

| 层 | 选型 |
| --- | --- |
| 框架 | Flutter 3 / Dart |
| 状态管理 | Riverpod / Bloc |
| 路由 | go_router |
| 网络 | Dio + web_socket_channel |
| 本地存储 | Drift（SQLite）/ Isar |
| 安全存储 | flutter_secure_storage |
| 推送 | Firebase Cloud Messaging + APNs + 厂商通道 |
| 音视频 | WebRTC（flutter_webrtc） |
| 媒体 | image_picker、file_picker、camera |
| 权限 | permission_handler |
| 国际化 | intl / flutter_localizations |

### 6.3 功能清单

- [ ] **MOB-01** 登录/注册/Token 刷新/自动登录。
- [ ] **MOB-02** 会话列表、未读数、置顶、免打扰。
- [ ] **MOB-03** 好友列表、申请、备注、黑名单。
- [ ] **MOB-04** 单聊、群聊、频道（后续）。
- [ ] **MOB-05** 文本、表情、图片、语音、视频、文件、位置。
- [ ] **MOB-06** 拍照、相册、录音、录像、文件选择。
- [ ] **MOB-07** 历史消息、搜索、本地缓存。
- [ ] **MOB-08** 在线状态、正在输入、已读回执、撤回。
- [ ] **MOB-09** 离线消息、断线重连、后台恢复。
- [ ] **MOB-10** 推送通知、角标、深链跳转。
- [ ] **MOB-11** 音视频通话、屏幕共享（按平台能力）。
- [ ] **MOB-12** 设置、隐私、通知、存储管理。
- [ ] **MOB-13** 多端消息同步与已读同步。
- [ ] **MOB-14** 国际化、暗色模式、无障碍。

### 6.4 平台专项

- [ ] **MOB-AND-01** Android：FCM/厂商推送、后台保活策略、权限适配。
- [ ] **MOB-IOS-01** iOS：APNs、后台音频/voip、权限与隐私声明。
- [ ] **MOB-SEC-01** 安全存储、证书校验、越狱/root 检测（按需）。
- [ ] **MOB-OPS-01** 构建流水线、签名、灰度、崩溃上报。

### 6.5 移动端验收标准

- [ ] 双端安装包可正常安装、登录、收发消息。
- [ ] 离线消息与推送正常。
- [ ] 图片/语音/视频/文件收发正常。
- [ ] 断网重连后消息不丢、不重复。
- [ ] 音视频通话可用（按发布范围）。
- [ ] 通过隐私合规与权限检查。

---

## 7. 跨端统一事项

- [ ] **OPS-01** 统一消息协议与版本：TCP/WS/HTTP 共用同一套消息模型。
- [ ] **OPS-02** 统一错误码表：客户端可国际化、可重试、可提示。
- [ ] **OPS-03** 统一上传流程：预签名、分片、断点续传、回调。
- [ ] **OPS-04** 统一推送协议：离线消息、推送模板、深链参数。
- [ ] **OPS-05** 统一鉴权：用户端 JWT、管理端 JWT 分离；刷新与吊销策略一致。
- [ ] **OPS-06** API 文档：OpenAPI/Swagger + 协议时序图。
- [ ] **OPS-07** 代码生成：从 OpenAPI/Protobuf 生成多端 SDK。
- [ ] **OPS-08** 共享测试语料：消息编解码、边界用例、异常用例。
- [ ] **OPS-09** 功能开关与灰度：按用户/设备/版本控制新功能。
- [ ] **OPS-10** 埋点与统计：登录、消息、通话、推送、崩溃。
- [ ] **OPS-11** 设计系统：颜色、字体、间距、组件规范跨端统一。
- [ ] **OPS-12** 国际化与无障碍。

---

## 8. 测试与质量门禁

- [ ] **QA-01** 单元测试覆盖率基线。
- [ ] **QA-02** 协议兼容性测试：旧客户端 vs 新服务端。
- [ ] **QA-03** 端到端测试：注册、登录、单聊、群聊、离线、重连。
- [ ] **QA-04** 弱网测试：延迟、丢包、抖动、断网恢复。
- [ ] **QA-05** 压测与长稳：连接数、消息 TPS、内存泄漏。
- [ ] **QA-06** 安全测试：越权、注入、重放、限流、文件上传。
- [ ] **QA-07** 多端一致性测试：Web/Electron/Flutter 消息顺序与已读。
- [ ] **QA-08** 发布门禁：构建、测试、扫描、审批、回滚预案。

---

## 9. 建议执行顺序

| 顺序 | 阶段 | 主要任务 | 完成标志 |
| --- | --- | --- | --- |
| 1 | 后端基线加固 | TLS、限流、日志脱敏、多线程 io、CI、压测 | 单聊可安全试运行 |
| 2 | WebSocket 网关 | WSS、鉴权、消息收发、历史、回执 | Electron/Web 可接入 |
| 3 | Electron MVP | 登录、会话、单聊、文本、离线、通知 | 桌面端 beta 可用 |
| 4 | 群聊与富媒体 | 群表/API、上传服务、图片/语音/视频/文件 | 群聊与富媒体可用 |
| 5 | 管理后台 | Drogon + Vue3、RBAC、用户/群/消息/审核 | 运营可后台管理 |
| 6 | 实时增强 | 多设备、推送、正在输入、撤回 | 多端体验完整 |
| 7 | 音视频 | 信令、WebRTC、TURN/SFU | 1v1/群通话可用 |
| 8 | 移动端 | Flutter、推送、音视频、发布 | iOS/Android 可用 |
| 9 | 规模化 | Redis/Kafka、多实例、监控告警 | 可水平扩容 |

---

## 10. 风险与决策记录

| 决策点 | 建议 | 理由 |
| --- | --- | --- |
| TCP vs WebSocket | 保留 TCP，新增 WS 网关并存 | 现有客户端可控；Web/Electron/移动端需要 WS |
| JSON vs Protobuf | MVP 继续 JSON，规模上来后评估 Protobuf | 兼容现有协议，降低迁移成本 |
| Drogon vs Boost.Beast | 管理后台用 Drogon，IM 服务保留 Beast | 管理端开发效率优先，IM 服务保持轻量 |
| Electron vs Tauri | 先 Electron，后续评估 Tauri | 生态成熟、WebRTC/Node 能力完整 |
| Flutter vs React Native | Flutter | 与原规划一致，跨端 UI 与性能好 |
| 自建 RTC vs 第三方 | 先第三方/信令自研，后评估自建 SFU | 降低早期媒体运维成本 |
| MariaDB vs PostgreSQL | 继续 MariaDB | 现有 schema 与部署已适配 |
| 单实例 vs 多实例 | 先单实例 + 预留 Redis/Kafka 接口 | 控制早期复杂度，按容量演进 |

主要风险：

- **协议兼容**：新增消息类型必须避开现有枚举值，建议引入 `version` 字段并保留旧类型。
- **安全**：TLS、鉴权、限流、日志脱敏必须在公网暴露前完成。
- **数据一致性**：消息落库、离线队列、回执更新需要明确事务边界与幂等。
- **多端同步**：需要统一 `seq`、游标、补拉与去重策略。
- **音视频运维**：TURN/SFU 带宽、NAT、弱网与监控成本较高，建议分阶段投入。
- **Electron 体积与内存**：需通过虚拟列表、缓存、按需加载控制资源占用。

---

## 11. 参考文档

- [项目 README](../readme.md)
- [TCP/WebSocket 选型说明](KC-CXIM-IMS-DESIGN.md)
- [现有后端待办与上线路径](total_comment.txt)（第 15–80 行）
- [腾讯云 IM Windows SDK 使用说明](TIM_USAGE.md)
- [数据库表结构](../dev/IMS/KC-CXIM-Svc/basic_service_20261107/db/schema.sql)
- [HTTP + TCP 验收脚本](../dev/IMS/KC-CXIM-Svc/basic_service_20261107/step3_verify/verify_tcp_http.sh)
