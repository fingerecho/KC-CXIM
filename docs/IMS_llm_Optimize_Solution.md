# KC-CXIM IMS 服务端性能优化方案（LLM 分析稿）

> 分析对象：`dev/IMS/KC-CXIM-Svc/basic_service_20261107/`（`step1_tcp_echo` 服务端 + `step2_test_client` 压测客户端）
> 数据来源：`stress_report/test_stress_03_20261117.txt`、`test_stress_03_20261112.txt`、`test_stress_02_20261117.txt`
> 部署环境：腾讯云 2 核 vCPU / 2GB 内存 / 3Mbps 带宽（`110.40.206.253`），MariaDB 与 cxim-server 同机
> 本文只做分析与方案，不含任何代码改动；所有结论均给出文件:行号定位与可验证的量化依据。
> 与既有文档的关系：死锁（1213/1205/1020）与 `friends` 覆盖索引已在 `basic_service_20261107/fixme.md` 中分析并已落地（P0-1/P0-2/索引），本文不重复，只处理**延迟与吞吐**。

---

## 0. 一页结论（TL;DR）

| # | 瓶颈 | 证据 | 定位 | 严重度 |
|---|------|------|------|--------|
| 1 | **bcrypt cost=12 同步哈希在 2 核机器上打满 CPU**，注册/登录延迟随并发数**线性**增长 | 拟合 `login_max ≈ 138ms × N + 0.6s`，三点全部命中（N=20/100/200 → 3.38/14.38/28.20s） | `db.cpp:454`、`db.cpp:513` | **致命** |
| 2 | **CPU 密集的哈希与阻塞式 SQL 共用同一条 FIFO 队列、同一组 worker**，导致发消息/好友接口被注册风暴**队头阻塞** | `create request` p50 1043ms、`tcp reply` p50 5088ms，而 `test_stress_02`（无 bcrypt）同机同类接口 p50 仅 1.2s/342ms | `db.h:293-303`、`db.cpp:354-383` | **高** |
| 3 | **注册/登录/发消息的单请求 DB 往返次数过多**（预编译未复用 + 前置查重 SELECT + `SELECT LAST_INSERT_ID()` + 池借还各 ping 一次） | `sendMessage` 一个事务 8 条语句；`registerUser` 3 条语句 + 池 3 次往返 | `db.cpp:551-657`、`db.cpp:444-472`、`db.cpp:213-214`、`db.cpp:252-253` | **高** |
| 4 | **Linux 构建没有优化等级**（`CMAKE_BUILD_TYPE` 未设置，`build.sh` 直接 `cmake --build`），服务端业务代码以 `-O0` 跑在生产机上 | 顶层 `CMakeLists.txt` 全文无 `CMAKE_BUILD_TYPE`/`-O`；`build.sh:256` | `CMakeLists.txt`、`build.sh:253-259` | **高（纯免费收益）** |
| 5 | **TCP 连接从不设置 `TCP_NODELAY`**；HTTP 每请求短连接且服务端主动关闭 | `server.cpp:33-40`、`http_server.cpp:1327-1336` 无任何 `set_option`；`http_server.cpp:1288` `keep_alive(false)` | 全仓库 `no_delay` 零命中 | 中 |
| 6 | **登录后一次性触发 5~7 个 DB 任务**（离线消息 200 条 + 好友事件 + 群事件 + 标记位更新），风暴式重连时既拖慢登录又打满 3Mbps 带宽 | `session.cpp:183-195`、`session.cpp:563`、`session.cpp:617/644/671`、`session.cpp:473/499/523` | 中 |
| 7 | **离线消息队列表（`offline_messages`）与游标（`conversation_members.last_delivered_seq`）双轨并存**：每条消息固定多 1 次 INSERT + 1 次 DELETE + 索引维护 | `db.cpp:633-642`、`db.cpp:814` | 中 |
| 8 | **资源与配置错配**：`DB_POOL_SIZE=50` vs `DB_WORKER_COUNT=8`（并发连接实际只有 8）；systemd 单元未设 `LimitNOFILE`；`max_connections=100` 已被 DB 池 + Admin 后端 + `mariadbd` 内部线程挤占 | `build.sh:9-10`、`build.sh:18-42`、`prepare_env_V0.sh:310` | 中（500 并发失败的直接推手之一） |
| 9 | **无背压、无限流、无高水位**：任务队列无界、写队列无界、HTTP 体无大小上限 | `db.cpp:356-360`、`session.cpp:721-723`、`http_server.cpp:412` | 中（稳定性风险） |

**根因判断（一句话）**：13 秒不是"网络慢"也不是"数据库慢"，而是**整台 2 核服务器的 CPU 被 bcrypt 占满**——注册风暴期间，`mariadbd` 与业务线程抢不到核，所有接口（发消息、好友列表、删除好友）被连带拖成秒级；`test_stress_02` 用已存在用户（不触发 bcrypt）测得同类接口 p50 只有 342ms/1.2s，是一组天然对照实验。

**核心容量结论**：当前配置下，本机**注册+登录吞吐上限 ≈ 7.2 用户/秒**（推导见 §1.3）。任何瞬时登录并发 > 7 的场景（新版本发布、网络抖动后的重连风暴、渠道投放拉新）都会把登录延迟推到 10 秒以上，并连带劣化全部业务接口。

---

## 1. 压测数据复盘与量化归因

### 1.1 原始数据抽取（同一脚本、同一台机、同一天）

| 指标（ms） | N=20 | N=100 | N=200 | N=500 |
|---|---|---|---|---|
| register p50 / max | 2190 / 3278 | 7447 / 13968 | 14060 / 28111 | 超时中止 |
| login p50 / max | 2284 / 3384 | 13260 / 14384 | 26920 / 28198 | 超时中止 |
| create request p50 / max | 126 / 169 | 1043 / 3099 | 2494 / 7121 | — |
| accept p50 / max | 104 / 1099 | 261 / 3109 | 660 / 7114 | 499/500 超时 |
| tcp send p50 | 146 | 500 | 968 | — |
| tcp reply p50 | 1122 | 5120 | 10779 | — |
| list friends p50 | 103 | 477 | 888 | — |
| delete friend p50 | 91 | 211 | 366 | — |
| 总耗时 | 10.97s | 50.40s | 104.14s | — |

历史对照（11-12，`test_stress_03_20261112.txt`，N=100）：login p50 13027/13972ms。**两次跑分几乎一致**，说明瓶颈稳定、且与"连接池扩到 50、DB worker 扩到 8、io 线程 ≥4"这轮优化无关——即：**加线程/加连接对瓶颈毫无帮助**，这是判断"CPU 受限而非并发度受限"的第一个信号。

### 1.2 关键线性拟合：延迟 = 并发数 × 单次哈希 CPU 时间

取 `login` 的最大值（即最后一个完成的用户，等价于"整个哈希风暴的排空时间"）做差分：

```
(14384 - 3384) / (100 - 20) = 137.5 ms / 用户
(28198 - 14384) / (200 - 100) = 138.1 ms / 用户
截距 = 3384 - 20 × 138 ≈ 0.6 s   （WAN RTT + accept + 队列首包）
```

两点斜率一致到 0.6ms，**这不是巧合，是排队论的必然**：如果系统被单一资源饱和，最后一个任务的完成时间 = 任务总量 ÷ 该资源吞吐。

每个用户在这段窗口里对 CPU 提出的需求恰好是 **2 次 bcrypt**（1 次 `generateHash` + 1 次 `validatePassword`，见 `db.cpp:454` 与 `db.cpp:513`）。在 C 个核上：

```
每用户墙钟增量 = 2 × t_bcrypt / C
138 ms        = 2 × t_bcrypt / 2   →   t_bcrypt ≈ 138 ms（本机 cost=12 的真实单次耗时）
```

反推校验：`test_stress_03` 的 register 阶段本身在 N=100 时 max=13968ms，与 login max 14384ms 只差 416ms——因为注册与登录在同一线程里背靠背提交，全部 202 个哈希任务在同一个 ~14.4s 窗口内被 2 个核串行消化。**拟合完全成立**。

### 1.3 吞吐上限推导

```
CPU 供给      = 2 核 = 2000 ms CPU/s
每用户需求    = 2 × 138 = 276 ms CPU
注册+登录容量 = 2000 / 276 ≈ 7.2 用户/s
```

| 目标 N（瞬时并发登录） | 排空时间 ≈ N / 7.2 |
|---|---|
| 20 | 2.8 s（实测 3.4s ✓） |
| 100 | 13.9 s（实测 14.4s ✓） |
| 200 | 27.8 s（实测 28.2s ✓） |
| 500 | 69 s（实测：`accept` 阶段在 240s 内没跑完，中止 ✓） |

**推论 1**：只要 bcrypt 还在主链路上，加连接池、加 worker、加 io 线程都不可能改善登录延迟——因为限制是"CPU 秒数"，不是"并发度"。这解释了 11-12 到 11-17 两轮优化后数据原地不动的现象。

**推论 2**：CPU 被哈希占满期间，`mariadbd`（同机、同 2 核）拿不到时间片。于是所有纯 SQL 接口被"连坐"：N=100 时 `create request` p50 从 126ms（N=20）涨到 1043ms，`list friends` 从 103ms 涨到 477ms。这些接口**根本不触发 bcrypt**，它们的劣化只能由"CPU 抢占 + DB worker 队列被哈希占用"来解释。

### 1.4 对照实验：去掉 bcrypt 之后系统还剩多少瓶颈

`test_stress_02_20261117.txt`（200 线程，用**已存在**用户做全链路：发消息 + 收消息 + 查好友列表，100s 稳态）：

```
iter throughput: 44.47 iter/s      http p50/p95/p99 = 1207 / 4128 / 8153 ms
tcp   p50/p95/p99 = 342 / 2290 / 2685 ms
```

每个 iteration ≈ 2 次 TCP 交互 + 1 次 HTTP，44.5 iter/s ⇒ **纯 SQL 路径的稳态吞吐 ≈ 44~134 请求/s，且 p95 已到 2~4s**。

结论：**bcrypt 之外的第二瓶颈确实存在**，量级是"每请求 20~50ms 的 DB 侧开销 + 队列排队"，必须靠 §3 的 P1 项解决；但它是"第二位"的——在 100 并发登录场景下它贡献 ~0.6s 截距，bcrypt 贡献 13.8s。**先修 P0，否则任何 SQL 层优化在压测曲线上都看不出来。**

### 1.5 各阶段延迟的构成拆解（N=100）

| 阶段 | p50 | 构成（估算） |
|---|---|---|
| register 7447ms | ≈ 队列等待（哈希任务在 8 个 worker 前 FIFO 排到）+ 1×bcrypt 138ms + 3 条 SQL | `db.cpp:438-472` |
| login 13260ms | ≈ 排在全部注册之后 + 1×bcrypt 138ms + 1 条 SQL | `db.cpp:494-525` |
| create request 1043ms | ≈ 事务重试退避 + 4 条 SQL + 8 次往返；被 CPU 抢占放大 | `db.cpp:923-1009` |
| tcp send 500ms | `areFriends`(1 job) + `sendMessage`(1 job, 8 语句/16 往返) + 队列等待 | `session.cpp:221-242` |
| tcp reply 5120ms | **客户端设计使然**：主用户先收满 100 条，再**串行**发 100 条回复、每条等 SendAck（`test_stress_03.cpp:734-753`）。5088ms/100 ≈ 51ms/条 ⇒ 真实单条发送往返 ≈ 50ms | 见 §6 |
| list friends 477ms | 1 条覆盖索引 SQL + 队列等待 + CPU 抢占 | `db.cpp:1179-1184` |
| delete friend 211ms | 事务内 2 条 DELETE + 重试退避 | `db.cpp:1273-1295` |

> `tcp reply` 这一行必须澄清：它**大部分不是服务端的锅**，而是压测脚本的串行回复模型（见 §6 方法学修正）。但由它反推的"单条消息端到端 ≈ 50ms"是服务端消息路径的真实基线，可作为 §4 优化的验收口径。

### 1.6 长尾特征：~3.1s / ~7.1s / ~8.2s 的聚簇

多轮数据里 max 反复落在 3.07~3.13s、7.1s、8.2s、16.3s、18.8s 附近（`create request 3099`、`accept 3109`、`list 3074`、`delete 3088`、`tcp reply 9306`；N=200 时整族变成 7.1s）。**固定间隔 ~3s、且随负载放大倍数漂移**，典型特征是**重传超时（RTO）指数退避**（1s → 2s → 4s 累积）或内核 socket 缓冲/backlog 溢出，而不是慢查询（慢查询不会这么整齐）。最可能的三个来源，需在 §7 的观测项下证实：

1. CPU 100% 期间 `accept()` 无法及时执行 → SYN 队列/`accept` 队列溢出 → 客户端 SYN 重传（1s+2s）；`test_stress_03.cpp:239` 里 N=500 出现的 `system:10060 connect 超时` 与此同源。
2. 服务端未开 `TCP_NODELAY`，与对端延迟 ACK 交互产生 40~200ms 级停顿；在小包 + 连续多帧（登录 ACK + 离线补推 + 事件）场景下被放大。
3. `ulimit nofile` / MariaDB `max_connections` 触顶后新建连接被拒或排队（500 并发时几乎必然）。

---

## 2. 瓶颈定位地图（按资源维度）

### 2.1 CPU（决定性）

| 事实 | 位置 |
|---|---|
| 注册：`BCrypt::generateHash(password, 12)`，实测单次 ~138ms | `db.cpp:454` |
| 登录：`BCrypt::validatePassword(password, hash)`，同量级 | `db.cpp:513` |
| 哈希在 **DB worker 线程**内执行，与所有 SQL 任务共用 **一条无界 FIFO + 同一组 worker** | `db.h:293-303`、`db.cpp:354-383`、`db.cpp:432`、`db.cpp:487` |
| 部署时 worker = 8（`DB_WORKER_COUNT=8`），核数 = 2 ⇒ **4 倍超订**，切换开销 + 抢占 MariaDB | `build.sh:10` |
| io 线程 = `max(4, hardware_concurrency())` ⇒ 2 核机器上仍起 4 个 io 线程 | `main.cpp:100-102` |
| 服务端业务代码在生产构建中**无优化等级**（顶层未设 `CMAKE_BUILD_TYPE`，`build.sh` 未传 `-DCMAKE_BUILD_TYPE`）⇒ GCC 默认 `CXXFLAGS` 为空 = `-O0` | `CMakeLists.txt`、`build.sh:240-259` |
| `libbcrypt` 自身子工程硬编码 `-O3`（因此 bcrypt 本体不是被 `-O0` 拖累的部分，但业务/JSON/连接器代码全部是 `-O0`） | `third-party/libbcrypt/CMakeLists.txt:38-39` |
| 同机还跑 `mariadbd`（`innodb_buffer_pool_size=384M`）与 Admin 后端（`prepare_env_V0.sh:418` 导入 `schema_admin.sql`） | `prepare_env_V0.sh:302-323` |

> **注意 4 的边界**：`-O0` 对本方案的贡献主要是"每请求若干毫秒"的常数项（JSON 解析、string 拷贝、连接器调用栈、迭代器调试），在 44 iter/s 的稳态路径上可测；在 bcrypt 主导的登录曲线上几乎不可见。**修它是免费的，但不要指望它把 13s 变成 1s。**

### 2.2 DB 队列与连接

| 事实 | 位置 |
|---|---|
| `submit()` 无界队列、无优先级、无队列深度/等待时间指标 | `db.cpp:354-361` |
| 每个任务借还连接各做一次往返健康检查：`acquire()` 里 `isValid()`、`release()` 里 `setAutoCommit(true)` + `isValid()` ⇒ **每任务 +2~3 次 RTT** | `db.cpp:213-214`、`db.cpp:247-272` |
| `DB_POOL_SIZE=50` 但 worker=8 ⇒ 池上限永远用不到，配置误导 | `build.sh:9-10` vs `build.sh:10` |
| MariaDB `max_connections=100`，与 cxim(≤8~50) + Admin 后端 + 内部线程共享；N=500 时余量吃紧 | `prepare_env_V0.sh:310` |
| systemd 单元未设 `LimitNOFILE`（500 TCP + 500 HTTP + DB 连接 + binlog/日志 fd） | `build.sh:18-42` |
| 死锁重试内部 `sleep_for(5*attempt + rand()%20)` **占用 worker 线程**，风暴期进一步减少可用算力 | `db.cpp:90-95` |

### 2.3 单请求 SQL 往返与语句数

| 路径 | 语句/事务 | 位置 |
|---|---|---|
| register | SELECT 查重 → bcrypt → INSERT → `SELECT LAST_INSERT_ID()`（4 次往返 + 池 ping 2 次） | `db.cpp:444-472` |
| login | SELECT → bcrypt（2 次往返 + 池 ping 2 次） | `db.cpp:497-520` |
| 单聊发消息 | `areFriends` 一个任务（1 SELECT）+ `sendMessage` 一个任务（**事务内 8 条语句**：SELECT 1 users / INSERT conversations / INSERT IGNORE members / SELECT 去重 / UPDATE seq / INSERT messages / INSERT offline_messages / UPDATE conversation_members） | `session.cpp:221`、`session.cpp:242`、`db.cpp:551-657` |
| TCP 登录补推 | `fetchOffline(200)` + `fetchPendingIncoming(100)` + `fetchUnnotifiedResults(100)` + `markResultsNotified` + `fetchPendingGroupRequestsForApprover(100)` + `fetchUnnotifiedGroupResults(100)` + `markGroupResultsNotified` ⇒ **最多 7 个独立 DB 任务** | `session.cpp:183-195`、`session.cpp:563`、`session.cpp:617/644/671`、`session.cpp:473/499/523` |
| 好友申请/通过/删除 | 事务内 3~5 条语句，每条 `prepareStatement` 现场预编译（未见任何语句缓存） | `db.cpp:923-1009`、`db.cpp:1057-1095`、`db.cpp:1275-1293` |

### 2.4 锁与热点行（风暴期放大）

- 100 个并发"创建申请"来自**同一个 requester_id**：`uk_friend_requests_pair(requester_id,target_id)` 同索引范围内并发插入（`db.cpp:964-976`）。
- 100 个并发"删除好友"都要删 **mainUid 的那些行**：`DELETE ... WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)`（`db.cpp:1276-1277`）——同一用户索引区间的行锁 + 二级索引维护集中碰撞。这是 `fixme.md` 已识别的锁风暴面，本文只在"合并写/批量化"维度补强。

### 2.5 网络与内存（3Mbps / 2GB 预算）

| 项 | 估算 | 依据 |
|---|---|---|
| 单次 register/login HTTP 请求+响应 | ≈ 0.4~0.7KB | 请求体 ~90B，响应带 128B JWT + JSON |
| N=100 全生命周期 HTTP/TCP 总流量 | ≈ 0.6~1.0 MB | 在 3Mbps(=375KB/s) 上约 2~3s 的净传输时间，**非本次主因** |
| 100 条长连接心跳（5s） | ≈ 2.4 KB/s | `test_stress_03.cpp:268` |
| **重连风暴 + 深离线队列** | 每用户登录最多补推 200 条 × ~250B = **50KB**；100 用户同时重连 = **5MB ≈ 13s**（打满 3Mbps） | `session.cpp:563` |
| 2GB 内存占用 | mariadbd(384M pool + 每连接 thread/sort buffer) + cxim(每 session 的 `body_`、`write_queue_` 无上限) + Admin 后端 + 系统 | `session.h:93-95`、`session.cpp:721-723` |
| HTTP 体无大小上限 | `http::async_read` 未传 `body_max` ⇒ 大 body 可耗尽 2GB | `http_server.cpp:410-417` |
| TIME_WAIT 堆积 | 服务端 `keep_alive(false)` 且主动 close ⇒ 44 req/s × 60s ≈ **2600 个 TIME_WAIT**，fd 与端口churn | `http_server.cpp:1282-1297` |

### 2.6 可观测性（当前为零）

- 没有任何分阶段计时（队列等待 vs 借连接 vs SQL vs bcrypt vs 回调排队 vs 网络写）。
- `QUIET_LOG=1`（`build.sh:13`）把 `std::cout/std::cerr` 全量丢弃（`quiet_log.h:12-20`）——好事（避免 `std::endl` 刷 journald 拖慢 io 线程，`session.cpp:172/279/592` 等处每条消息都在打印），但**代价是现在完全没有运行时证据**。
- 无 Prometheus/无慢查询日志/无 `performance_schema`（`prepare_env_V0.sh:311` 显式 OFF，2GB 机器上合理）。

---

## 3. 优化方案总表

> 工作量：S ≤0.5 天，M ≤2 天，L ≥3 天。收益按 N=100 好友全生命周期压测（当前基线 50.4s，login p50 13.3s）估算。

| ID | 方案 | 优先级 | 工作量 | 预期收益（登录/整体） | 风险 |
|---|---|---|---|---|---|
| P0-1 | bcrypt 成本标定 + **降 cost 到 10** + 哈希并发上限（信号量=核数） | P0 | S | login 14.4s → **~4s**；配合 P0-2 后 <1s | 抗离线爆破强度下降（用 §4.1 的补偿措施） |
| P0-2 | **拆分 CPU 池与 SQL 池**（哈希不再阻塞式占用 DB worker） | P0 | M | 消除队头阻塞：`create request` 1043 → **~150ms** | 低；需保证任务数上限 |
| P0-3 | **Refresh Token / 免重登**：重连只刷 JWT，不再走 bcrypt 登录 | P0 | M | 重连风暴下登录成本 **≈0**（收益最大的一项） | 需新增接口与客户端改造 |
| P0-4 | 注册/登录接口**令牌桶限流 + 429/Retry-After + 队列深度熔断** | P0 | S | 保护在线用户；把"无限排队"变成"可预期降级" | 需要客户端退避重试 |
| P0-5 | 生产构建改 `-O2`（`CMAKE_BUILD_TYPE=Release`），压测客户端也用 Release | P0 | S | 常数项 -20~40%；零逻辑风险 | 需保留符号以便定位 |
| P0-6 | 注册/登录**削减往返**：去掉前置 SELECT、用 `getLastInsertId()`、降低 `isValid()` 频率 | P0 | S | register 每用户 -2~3 RTT | 需保留 1062 兜底语义 |
| P0-7 | 部署参数：`LimitNOFILE=65535`、`DB_POOL_SIZE≈DB_WORKER_COUNT`、worker 与核数匹配、确认 `max_connections` 余量 | P0 | S | 解除 500 并发的 fd/连接墙 | 低 |
| P1-1 | `areFriends` 结果**进程内 LRU 缓存**（增删/拉黑即时失效） | P1 | M | 消息路径少 1 个 DB job（-30~50%） | 多实例部署需换 pub/sub 失效 |
| P1-2 | `areFriends + sendMessage` **合并为一个事务任务** | P1 | M | 消息 p50 500 → **~150ms** | 需重构回调链 |
| P1-3 | 登录补推**合并成 1~2 个任务**（UNION / 一次多语句），并按需触发 | P1 | M | 登录侧 DB job 7 → 2；补推风暴可控 | SQL 复杂度上升 |
| P1-4 | 用**游标模型替代 `offline_messages` 行级队列**（已有 `last_delivered_seq`） | P1 | L | 每条消息少 1 INSERT + 1 DELETE；写放大 -30% | 多端投递/回溯语义要重新推导 |
| P1-5 | `TCP_NODELAY`（TCP + HTTP 两处 accept 后） | P1 | S | 消除 40~200ms 级停顿与 3.1s 长尾嫌疑 | 极低 |
| P1-6 | HTTP **keep-alive + 空闲超时 + body 上限** | P1 | M | 消除 TIME_WAIT/fd churn；降低每请求握手 | 需正确设超时防慢客户端 |
| P1-7 | 好友写操作**批量化**（同 requester 多目标合并 INSERT；同 actor 多删除合并） | P1 | M | 削峰 + 减少锁碰撞（配合 `fixme.md`） | 需新增批量接口或合并窗口 |
| P1-8 | 事件**合并写**：同一连接的多个待推帧一次 `async_write`（writev 语义） | P1 | S | 降低系统调用与 Nagle 交互；长连接推 100 事件更快 | 需保持帧序 |
| P2-1 | 观测埋点（队列深度/等待分位数/bcrypt 耗时/SQL 耗时/RTT 计数）+ 每接口 histogram | P2 | M | 把"猜"变成"看" | 低 |
| P2-2 | MariaDB 侧：慢日志阈值、`Com_stmt_prepare` 计数、直方图统计、语句预编译复用 | P2 | M | 定位并削减 DB CPU | 低 |
| P2-3 | `SessionManager` 全局单锁 → 分片锁（在线数 ≥1k 时才有意义） | P2 | S | 减少推送期锁竞争 | 代码复杂度 |
| P2-4 | 写队列/离线队列**高水位 + 丢弃或降级策略**；内存预算表 | P2 | M | 2GB 下的稳定性 | 需产品定义"丢什么" |
| P3-1 | 哈希算力外置（独立 auth 进程/实例/托管），或 DB 迁出本机 | P3 | L | 彻底解开 CPU 天花板（登录上限 → ∞） | 运维复杂度、RTT |
| P3-2 | 引入 Redis（关系缓存/在线表/限流计数），消息写扩散异步化（MQ） | P3 | L | 支撑 5k~50k 并发 | 架构换代，超 2C2G 范畴 |
| P3-3 | 资源升配（2C2G→4C4G）与收益对照 | P3 | S | 线性抬升 CPU 天花板 | 成本 |

---

## 4. 关键方案详述

### P0-1：bcrypt 成本标定、降档与算力上限

**现状**：`db.cpp:454` 固定 `cost=12`，实测单次 ~138ms（§1.2 反推），2 核容量 = 14.5 哈希/s。

**改法**：

1. **先标定，再决策**。写一次性微基准（不进主分支亦可）：在本机分别跑 cost=8/9/10/11/12，记录 `ms/hash`，得到本机真实曲线。下文用实测值 `t(cost)` 代替估算。
2. **把 cost 从 12 降到 10**（`db.cpp:454`），并按 OWASP 口令存储基线保持 "work factor ≥ 10"。注意：`cost` 每降 1，CPU 成本减半，安全性（离线爆破抗性）减半。
3. **哈希并发上限 = 核数**（`hardware_concurrency()`，本机=2），用信号量/专用线程池强制约束；**而不是现在的 8 个 worker 同时抢 2 个核**。超订不会提高吞吐，只会推高所有线程的延迟（含 SQL）。
4. **兼容性**：bcrypt 哈希串自带 cost 前缀（`$2a$12$...`），`validatePassword` 按串内 cost 校验，**因此降 cost 只需处理新注册用户**，老用户登录仍按其原始 cost 校验（延迟不变，但不会被新策略放大）。若希望统一抬升，可在用户下次成功登录时透明 rehash 到新 cost（登录已验证密码，成本仅为 1 次额外哈希，且发生在无风暴的正常时段）。

**补偿措施（必须同时做，否则等于降低安全基线）**：

- `/api/login` 与 `/api/register` 按 IP + 账号双维度限流（P0-4），失败计数与锁定策略（现在完全没有失败计数）。
- 保持密码 ≥6 位策略不变，但 `parse_credentials`（`http_server.cpp:651`）的 `password.size() < 6` 偏低，建议提升到 8 并加弱口令黑名单，用"口令熵"补回一部分成本预算。
- 登录接口加"人机/设备指纹"或验证码兜底（产品层）。

**预期**：cost 10 → `t≈35ms` → 斜率 `2×35/2 = 35ms/用户` → **N=100 login max ≈ 0.6 + 3.5 ≈ 4.1s**。单靠它到不了亚秒级，因此 P0-2/P0-3 是配套项，不是可选项。

**验证**：压测 `--friends 100/200` 的 `login` 斜率应从 138ms/用户降到 ~35ms/用户；`perf top` 里 `E_*/bf_crypt_gensalt` 等符号占比应显著下降。

**回滚**：仅配置/常量变更，改回 12 即可（老哈希不受影响）。

---

### P0-2：拆池——CPU 型任务与阻塞型任务彻底分离

**现状**：`Db` 只有一个 `jobs_` FIFO + 一组 worker（`db.h:293-303`、`db.cpp:354-383`），`registerUser`/`loginUser` 的 bcrypt 与 `sendMessage`/`listFriends` 的 SQL 抢同一批 worker。**结果**：100 个注册把 8 个 worker 全部占住，此时一条"发消息"要排在 100 个 138ms 任务之后 → `tcp send` p50 500ms、`create request` p50 1043ms 的直接来源（这就是队头阻塞，Head-of-Line Blocking）。

**改法**：

1. `Db` 内部拆成两个池：
   - `hashPool`：线程数 = `hardware_concurrency()`（本机 2），**只**跑 bcrypt；
   - `sqlPool`：线程数 = `hardware_concurrency()×2~3`（本机 4~6），只跑 SQL；
   - 连接池上限与 `sqlPool` 对齐（见 P0-7）。
2. 或者更彻底：`registerUser`/`loginUser` 拆成 `step1(查重/取回哈希) → hashPool → step2(写库/返回)`，让同一请求不再长时间霸占一条 DB 连接（现在是"借连接 → 哈希 138ms → 执行 SQL → 还连接"，连接被哈希白占 138ms，见 `db.cpp:438-472` 中 `pc` 的生命周期覆盖了 bcrypt）。**这一点很重要**：即使不拆池，只要把 bcrypt 移出 `PooledConn` 的作用域，SQL 连接的占用时间就只剩 SQL 本身。
3. 给 `sqlPool` 加**优先级**（在线用户交互类 > 批量/后台类），或至少把"重连补推"这类可延迟任务标记为低优先级。

**预期**：`create request` / `tcp send` / `list friends` 的 p50 与登录风暴**解耦**——按 §1.4 的对照组（342ms/1207ms 量级）看，风暴期这些接口应回落至 100~300ms。

**风险**：线程数增加要受 CPU 约束；两个池都需正确停机（现有 `stopping_`/`pool_->stop()` 逻辑要复制，`db.cpp:342-352`）。

---

### P0-3：Refresh Token——把"重连"从哈希成本里摘出去（收益最大）

**现状分析**：架构已经做对了一半——**TCP 登录只验 JWT、不查库、不算哈希**（`session.cpp:143-157`，`do_login` 注释明确）。问题在**上游**：TCP 登录需要的 JWT 只能由 `POST /api/login` 签发（`http_server.cpp:671-683`），而后者每次都做 bcrypt verify。于是：

- 每次网络抖动/服务端重启后 100 个客户端同时重连 → 100 次 bcrypt verify → 14s 排空 → TCP 登录拿不到 token → `Session::connect` 的 **10s 等待超时**（`test_stress_03.cpp:231`）→ 客户端继续重试 → **正反馈雪崩**。

**改法**：

1. 新增 `POST /api/token/refresh`：入参为**长效 refresh token**（或已签发的 access token 仍在有效期内），服务端**只做 HMAC 验签**（微秒级），直接返回新 access token。**不查库、不算哈希。**
2. 登录响应（`http_server.cpp:76` `make_login_ok`）附带 `refreshToken` + `expiresIn`；客户端在 access 过期前刷新，仅在 refresh 也失效/密码变更时才回落到密码登录。
3. 客户端重连策略：TCP 断线后**先用 refresh 换新 JWT 再 TCP 登录**；失败才走密码登录。并加**抖动退避（jittered backoff）**，把"同时重连"打散成"错峰重连"。
4. refresh token 建议：不透明随机串 + 服务端存储（或自包含 + 版本号），支持"改密即全部失效"。2GB 内存下用 MariaDB 一张小表即可（`id BIGINT PK, user_id, token_hash, expires_at` + `(user_id, expires_at)` 索引），或先复用现有 users 表旁的 KV 列。

**预期**：重连风暴场景下**每用户 CPU 成本从 276ms 降到 ~0**；`login` 指标不再出现在重连路径上。这是唯一能把"13 秒"变成"几十毫秒"的方案——因为它消灭了工作量本身，而不是优化工作量。

**风险**：新增凭据类型带来安全评审需求（存储、旋转、撤销）；需要 Desktop / 微信小程序 / 压测客户端同步适配。**灰度顺序：先发接口、后改客户端，服务端保留密码登录不动。**

---

### P0-4：背压与限流（把不可控的雪崩变成可预期的降级）

**现状**：`submit()` 无界队列（`db.cpp:356-360`）；`write_queue_` 无界（`session.cpp:721-723`）；HTTP 无 body 上限（`http_server.cpp:412`）；注册/登录无速率限制。**结果**：过载时系统不停 "变慢一点"，而是**所有人都变慢 20 倍**，且第 500 个并发直接把连接建不起来（N=500 的 `connect 10060`）。

**改法**：

1. `/api/register`、`/api/login`：全局令牌桶 + 每 IP 令牌桶。速率按 §1.3 的容量算（cost=10、2 核 ⇒ 全局 ~28 用户/s，取 70% ⇒ **20/s**）。超限返回 **`429 Too Many Requests` + `Retry-After: <秒>`**（`http_server.cpp:1282` 的 `send_json` 增加一个带 header 的重载）。
2. 队列深度阈值（例如 `jobs_` > 500）时直接快速失败 503，而不是继续接收。
3. `write_queue_` 高水位（例如 4MB 或 2000 帧）：超过则丢弃可再生内容（历史/列表类）或断开该连接（由客户端重新拉取），并打点告警。**绝不能无界**——2GB 机器上 500 个慢客户端就能撑爆。
4. `http::async_read` 显式 body 上限（建议 64KB）；`frame::MAX_BODY` 已存在（`frame.h`），确认 TCP 侧一致生效（`session.cpp:50`）。
5. 心跳周期：客户端当前 5s（`test_stress_03.cpp:268`，`hb=5s`），配合 `IDLE_TIMEOUT=60`（`session.h:39`）完全没必要——**改 20~30s**，直接省掉 4~6 倍的 TCP 帧与 wake-up。

**预期**：N=500 场景由"整体失败"变为"前 20/s 正常完成，其余收到 429 后退避重试"，成功率与 p99 都可控。

---

### P0-5：构建与部署的"免费收益"

**现状**：`CMakeLists.txt` 顶层没有 `CMAKE_BUILD_TYPE` 处理，`build.sh:253` 是裸 `cmake -S . -B build`，`build.sh:256` 是 `cmake --build build -j2` ⇒ Linux 生产二进制为**无优化**（`-O0`，且未定义 `-DNDEBUG`，于是 `assert` 等仍在）。

**改法**（配置层，不改业务代码）：

1. `build.sh` 的 cmake 配置增加 `-DCMAKE_BUILD_TYPE=Release`（或 `RelWithDebInfo` 以便线上定位符号）；`CMakeLists.txt` 里加"未显式指定时默认 Release"的兜底。
2. 明确校验：`cmake --build` 后检查产物的编译命令行确实带 `-O2`（`grep -m1 CXX_FLAGS build/CMakeFiles/cxim-server.dir/flags.make`）。
3. 压测客户端同样问题：报告里的样本全部来自 `build/bin/Debug/test_stress_03.exe`（`test_stress_03_20261117.txt:1`）。**Debug 版 MSVC（/Od + 安全迭代器）会让客户端自身处理时间膨胀几十倍**，直接污染 p50/p99 的截距项。以后压测一律用 Release，并在报告里记录客户端构建配置。
4. systemd 单元（`build.sh:18-42` 生成）补：`LimitNOFILE=65535`、`LimitCORE=0`、可选 `CPUWeight=`/`AllowedCPUs=` 给 `mariadbd` 预留至少 1 个核；重启后 `cat /proc/<pid>/limits` 确认生效。

**预期**：业务代码路径常数开销下降（经验值 15~40%）；客户端测量污染消除；解除 500 并发的 fd 上限。**注意这是"必做但非决定性"的一项，收益应体现在 p99 长尾与 SQL 类接口上，而不是 login 的 13s。**

---

### P0-6 / P2-2：削减单请求 DB 往返（含池 ping）

**现状清单（register 为例，共 6~8 次 RTT）**：

```
PooledConn 构造 → acquire() → isValid()  [1]
SELECT id FROM users WHERE username=?     [2]  ← 仅为"友好提示"
INSERT INTO users(...)                    [3]
SELECT LAST_INSERT_ID()                   [4]  ← 可用 API 直接拿
PooledConn 析构 → release() → setAutoCommit(true)+isValid()  [5][6]
（若 connector 对 prepareStatement 走服务端预处理，上面每条 SQL 还要各 +1 次 COM_STMT_PREPARE）
```

**改法**：

1. **删掉前置查重**（`db.cpp:444-450`）：直接 INSERT，靠 `uk_users_username` 的 1062 转成 `"username already exists"`——该分支代码已经存在（`db.cpp:463-466`），语义完全等价，省 1 次往返。
2. **用 `conn->getLastInsertId()` 取代 `SELECT LAST_INSERT_ID()`**（`db.cpp:26-31`、`db.cpp:469`、`db.cpp:573/613/629`）：连接器提供该 API，省 1 次往返/处（`sendMessage` 里就有 3 处）。
3. **降低连接健康检查频率**：`acquire()` 不再每次 `isValid()`（`db.cpp:213-214`）。改为"记录 last_used_ts，超过 30s 未用才 ping"；`release()` 只在连接曾经报错时才 `setAutoCommit`（`db.cpp:250-257`）。InnoDB 侧 `wait_timeout` 远大于本服务的空闲间隔，脏连接靠 SQL 执行失败 + 一次重试即可覆盖。
4. **确认预编译开销**：抓 `SHOW GLOBAL STATUS LIKE 'Com_stmt_prepare'` / `'Com_stmt_execute'` 比值。若 ≈1:1，说明每次 `prepareStatement` 都在服务端重新预编译（`db.cpp` 全文 100+ 处现场 prepare）。对策：按"SQL 文本 → PreparedStatement"做每连接缓存（复用同一连接上的语句对象），或对该版本连接器确认并开启语句缓存开关；此项以**实测**为准再决定是否改造，不要盲改。
5. `sendMessage` 事务内**去掉 `SELECT 1 FROM users WHERE id=?`**（`db.cpp:557-561`）：接收者存在性可由 `fk_messages_sender`/关系校验覆盖，或在 P1-1 的关系缓存里一并带上"目标有效"位；省 1 条语句/1 往返。

**预期**：SQL 类接口每请求 CPU/RTT 下降 20~40%；稳态吞吐（`test_stress_02` 的 44 iter/s）应显著抬升——**这项是 §1.4 那个"第二瓶颈"的主要解药**。

---

### P1-1 / P1-2：消息路径重构（每消息从 2 job/16 往返 → 1 job/若干往返）

- **P1-1 关系缓存**：`areFriends` 是每条消息的必经前置（`session.cpp:221`），而 `friends` 关系变更极低频（加好友/删好友/拉黑）。用进程内 `LRU<(min_uid,max_uid) → {friends,blocked}>`（TTL 5~10s）即可吃掉绝大多数查询；**在 `handleFriendRequest`(accept)、`deleteFriend`、`setFriendBlocked` 三处显式失效双向 key**（`db.cpp:1035`、`db.cpp:1260`、`db.cpp:1316`）。风险边界：单进程部署下成立；将来多实例必须换成 Redis/广播失效，且"拉黑即时生效"是产品要求——保留一个"强制查库"开关给敏感路径（如支付/系统消息）。
- **P1-2 合并为单事务**：`do_send` 现在是"areFriends 回调里再发 sendMessage 任务"（`session.cpp:221-287`），两次进出线程池 + 两次 strand 切换 + 两次排队。合并成 `Db::sendChecked()`：同一连接、同一事务内完成"关系校验 + 会话/seq/消息/游标"，顺带消除"校验与落库之间好友被删"的 TOCTOU 窗口，并去掉一半的 `post/strand` 开销（当前每次 DB 回调都 `boost::asio::post(strand_, ...)` 拷贝捕获的 `content`/`sm` 大对象，`session.cpp:225-247` 两层嵌套拷贝）。

**预期**：`tcp send` p50 500ms → **150ms 以下**（无风暴期）；服务端消息 CPU 预算下降 ~40%。

---

### P1-3：登录补推合并（7 个 DB job → 1~2 个）

现状每次 TCP 登录都会打 5~7 个任务（`session.cpp:183-195`）。风暴期这意味着 **100 × 6 = 600 个 SQL 任务**排在已经被哈希占满的队列里，正是 §1.2 里 0.6s 截距与 3s 长尾的放大器。

改法：

1. 把 `fetchPendingIncoming` + `fetchUnnotifiedResults` + `fetchPendingGroupRequestsForApprover` + `fetchUnnotifiedGroupResults` 合成**一条 SQL**（`UNION ALL` + `kind` 判别列）或一个任务内顺序执行（同一条连接、省 4 次借还连接 + 4 次 ping + 4 次 post 回 io）。
2. `markResultsNotified` / `markGroupResultsNotified` 合并进同一事务（`session.cpp:671`、`session.cpp:523`）。
3. `fetchOffline` 从 200 降到 50，并加"还有更多"标记位（返回 `has_more`，客户端按需继续拉），避免 100 用户同时登录时瞬间推 5MB（§2.5 的带宽炸弹）。
4. 群事件补推只在用户"确实有群"时才发起（用在线表/用户位标记，或在前一条 SQL 的结果里带一行 flag），无群用户直接跳过。

**预期**：登录侧 DB 任务数 -70%，重连风暴可控，带宽突发下降 4 倍。

---

### P1-4：用游标替代 `offline_messages` 行级队列（架构级、但收益直接）

现状每条消息：`INSERT INTO offline_messages`（`db.cpp:633-642`）→ 在线时投递 → 客户端 ACK → `DELETE FROM offline_messages`（`db.cpp:814`）。而系统**已经有** `conversation_members.last_delivered_seq` / `last_read_seq` 游标（`db.cpp:646-655`、schema `idx_members_user`）。双轨制带来：每消息 +1 INSERT +1 DELETE +1 二级索引维护 + `offline_messages` 表持续膨胀（写放大，2 核机器上的 InnoDB 成本主要在写 IO）。

改法（保持"不丢消息"不变量）：

- 补推改为按游标查询：`SELECT ... FROM messages m JOIN conversation_members cm ON ... WHERE cm.user_id=? AND m.conversation_id=? AND m.seq > cm.last_delivered_seq ORDER BY seq`。
- ACK 只推进游标（UPDATE 一行），不再删行。
- `offline_messages` 保留一段时间做迁移/校验，然后下线；或者降级为"仅群消息"用途。
- 需重新推导的点：多端并发（同一账号两台设备，游标会被先 ACK 的那台推进）——若需支持，把游标粒度从 `(conversation,user)` 改为 `(conversation,user,device)`；或者仍以 `offline_messages` 为准。**这条必须先出设计结论再动代码，属于 P1 里唯一需要评审的项。**

**预期**：消息写入路径语句数 8 → 6，写放大 -25~30%；`offline_messages` 不再随消息量线性膨胀。

---

### P1-5 / P1-6 / P1-8：传输层收尾

1. **`TCP_NODELAY`**：`server.cpp:34-38` 与 `http_server.cpp:1329-1333` accept 后 `socket.set_option(boost::asio::ip::tcp::no_delay(true), ec)`。全仓库当前 `no_delay` 零命中。IM 是典型小报文交互协议，Nagle + 对端 Delayed ACK 会带来 40~200ms 停顿，也是 §1.6 那个 3.1s 长尾的首要嫌疑之一。同处建议开启 `socket_.keep_alive?` 交由应用层心跳（已有），并考虑 `send_buffer_size/receive_buffer_size` 上限（3Mbps 链路下小缓冲反而降低积压）。
2. **HTTP keep-alive**：`http_server.cpp:1288` 的 `keep_alive(false)` 改为支持复用（同连接多次请求 + 60s 空闲超时 + 上限次数），消除 2600 级 TIME_WAIT 与每请求 TCP 握手（跨公网握手 + 慢启动本身就给每个 auth 请求加 1~3 个 RTT）。同时补 `Content-Type` 校验与 body 上限（P0-4）。
3. **事件合并写**：`send_message()` 一条帧一次 `async_write`（`session.cpp:714-740`）。登录补推 100 条时 = 100 次系统调用。改为在 strand 内把队列前若干帧拼成一个 buffer（或用 `async_write` 的 buffer sequence）一次下发，**保持帧序不变**（`std::deque` 前 K 个拼接）。

**预期**：推送密集场景（登录、群广播）的 CPU syscall 与长尾明显收敛。

---

### P0-7 / P3：配置与资源边界（不改代码的那一半）

| 项 | 现状 | 建议 |
|---|---|---|
| `DB_POOL_SIZE` vs `DB_WORKER_COUNT` | 50 vs 8（`build.sh:9-10`）——池上限永远用不到，误导后来人 | 池上限 = `sqlPool` 线程数（4~6），并在文档里写清"并发连接数 = worker 数" |
| MariaDB `max_connections` | 100（`prepare_env_V0.sh:310`） | 与 cxim + Admin + 运维工具核对总量后调至 150~200，同时盯内存（每连接 sort/join buffer 默认值 2GB 机器上偏大） |
| `innodb_buffer_pool_size` | 384M | 保持（2GB 机器合理）；若 DB 迁出本机则不再受此约束 |
| `innodb_flush_log_at_trx_commit` | 1（刻意为之，注释"IM 消息优先不丢"） | **不要为了压测改它**（见 §8）；改由"合并事务/减少提交次数"来降 fsync |
| binlog | ROW + `expire_logs_days=7` | 保留；关注 binlog fsync 与 redo 的叠加，考虑 `sync_binlog` 适度放宽（同属可靠性权衡，需产品确认） |
| ulimit | 单元无 `LimitNOFILE`（`build.sh:18-42`） | `LimitNOFILE=65535`，并用 `/proc/<pid>/limits` 实证 |
| 心跳 | 5s（`test_stress_03.cpp:268`）+ `IDLE_TIMEOUT=60s` | 客户端 20~30s；服务端按接入端区分 |
| 容量 | 2C2G/3Mbps | 见 §5 的成本对照：本方案全落地后 2C2G 大约能撑"1k~3k 长连接、100~150 消息/s 稳态"；要 N=500 登录不超时，**升 4C4G 或做 P3-1 算力外置是更省事的解** |

---

## 5. 收益预测（合并后的量化预期，N=100）

| 阶段 | 现状 p50 | P0 后 | P0+P1 后 | 主要来源 |
|---|---|---|---|---|
| register | 7447ms | 250~400ms | 200~350ms | P0-1（÷4 CPU）+ P0-2（不排队）+ P0-6（-3 RTT） |
| login | 13260ms | 400~700ms | **<150ms（走 refresh 路径≈RTT）** | P0-1 + P0-2 + **P0-3** |
| create request | 1043ms | 150~250ms | 100~200ms | P0-2 解队头阻塞 + P0-5 编译 |
| accept | 261ms | 150ms | 100ms | 同上 + P1-7 批量 |
| tcp send | 500ms | 200ms | **80~150ms** | P1-1/P1-2（少 1 job/8 语句） |
| tcp reply | 5120ms | ~1000ms | ~300ms | 主要是客户端串行模型（§6）+ 服务端单条 50ms→15ms |
| list friends | 477ms | 120ms | 80ms | P0-5/P0-6 |
| 全流程 elapsed | 50.4s | ~12s | **~5s** | — |
| N=500 | 失败（accept 240s 超时） | 部分成功 | 成功（限流退避） | P0-3/P0-4/P0-7 |

**成本–容量对照（决定要不要花钱而不是改代码）**：

| 配置 | bcrypt cost=12 容量 | cost=10 容量 | 备注 |
|---|---|---|---|
| 2C2G（现状） | 7.2 用户/s | ~29 用户/s | 且必须给 MariaDB 留核 |
| 4C4G | ~29 用户/s | ~115 用户/s | 单价约 2 倍，容量 4 倍（含核数+内存+带宽包） |
| 2C2G + auth 外置/DB 外置 | 不受本机限制 | 不受本机限制 | 运维复杂度换硬件钱 |

---

## 6. 压测方法学修正（否则修完也看不见）

1. **`tcp reply` 有相当比例是脚本自己的锅**：`test_stress_03.cpp:734-753` 先串行收满 N 条，再**串行**逐条 `send_text_with_retry`（每条等 SendAck）。于是第 i 个好友的"收回复延迟" ≈ i × 单条往返 —— 这是 O(N²) 的测量伪影。改法：主用户改为**并发**回复（或收到一条立刻回一条），并同时记录"服务端单条 SendAck 延迟"作为独立指标。
2. **栅栏同步放大尾部**：`wait_counter(g_ready, N)` 让所有阶段在"最后一个注册完成"之后才开始（`test_stress_03.cpp:680`），所以每个阶段的延迟里都含着前一批的排空时间。**每个阶段单独统计"到达-完成"吞吐**，或用阶梯式 ramp-in。
3. **客户端必须 Release 构建**（§P0-5），并记录客户端 CPU/连接数/线程数。当前 N=500 时脚本要 **1500+ 线程**（每 friend 1 worker + recv + hb，`test_stress_03.cpp:266-280`），Windows 端本身就先到极限；500 用例应**分布式压测**或多进程分片。
4. **区分首次注册与重复登录**：`runId` 每次都新注册（`test_stress_03.cpp:451`），测的是"新用户洪峰"，与真实日常"老用户登录"是两种曲线。至少各跑一轮，并单独跑一轮"纯登录风暴"（预热用户池 + 只测 login）来验证 P0-3。
5. **固定失败即中止**：任一 friend 抛异常即 `g_stop`（`test_stress_03.cpp:544-548`）→ N=500 的一例 connect 超时把整轮变成 501 failed。**性能压测应统计错误率而非中止**（保留一个 `--fail-fast` 开关即可）。
6. 每个指标补 **QPS/吞吐**（现在只有延迟分位数 + elapsed），否则无法判断"是否达到容量拐点"。
7. 建议的验收矩阵：`N ∈ {10, 50, 100, 200, 500}` × 场景 ∈ {纯注册, 纯登录, 登录+重连, 全生命周期}，每格记录 p50/p95/p99/max + 各接口 QPS + 服务端 CPU% + `mysqld` CPU% + 队列深度。

---

## 7. 观测补齐清单（先测后改，P2-1）

**服务端埋点（建议用聚合直方图，避免逐条日志）**：

- `Db::submit` → worker 取出：队列深度、等待时间 p50/p99（区分 `hashPool`/`sqlPool`）——**这一条就能直接验证 §1.2**。
- `PooledConn` 借/还耗时；`created_` 峰值；`acquire()` 等待次数。
- 每条 SQL 的执行耗时（按 DAO 名分组）；`runTransactionWithRetry` 的重试次数（`db.cpp:90-95`，用于确认锁风暴是否仍在）。
- bcrypt 单次耗时分布（直接暴露 cost 标定结果）。
- `session` 写队列长度、丢帧计数；`SessionManager` 锁持有时长（`session_manager.cpp:13-45`）。
- HTTP：在途请求数、每请求语句/往返计数（`Com_stmt_prepare`/`Com_stmt_execute` 差值）。

**系统侧（2C2G 上的第一现场）**：

```bash
# CPU：谁在吃核？bcrypt 修好后 E_* 系列应消失，mysqld 占比应回归正常
perf top -p $(pgrep -d, -x cxim-server)     # 关注 blowfish/E_/bcrypt 符号
mpstat -P ALL 1                              # 是否两个核都 100%
vmstat 1                                     # r 列（运行队列长度）——4 个 worker on 2 核会看到 r≈6~10
iostat -x 1                                  # InnoDB fsync 压力（trx_commit=1 + binlog）
ss -s ; netstat -s | grep -i retrans         # 验证 §1.6 的 3.1s 是否为 RTO 重传
cat /proc/$(pgrep cxim-server)/limits        # LimitNOFILE 是否真生效
```

```sql
-- MariaDB 侧
SHOW GLOBAL STATUS LIKE 'Com_stmt%';            -- 预编译是否每次重来
SHOW GLOBAL STATUS LIKE 'Innodb_row_lock%';     -- 行锁等待
SHOW GLOBAL STATUS LIKE 'Aborted_%';            -- 连接异常断开
SELECT * FROM information_schema.PROCESSLIST;   -- 连接数是否被 cxim+admin 挤占
SET GLOBAL slow_query_log=ON; SET GLOBAL long_query_time=0.05;  -- 临时抓慢 SQL
```

**客户端侧**：记录 `cxim-server` 与 `mysqld` 的 CPU 曲线，与压测的 phase 时间轴对齐（现成脚本 `stress_report/show_svc_monitor.sh`、`show_svc_status.sh` 可直接用）。

---

## 8. 风险与"不为清单"（明确不做什么）

| 不做 | 原因 |
|---|---|
| 把 `innodb_flush_log_at_trx_commit` 改成 2/0 换性能 | 违反项目既定"IM 消息优先不丢"（`prepare_env_V0.sh` 注释），收益也只在小 SQL 上 |
| 去掉消息去重（`uk_messages_dedup` / `client_msg_id`） | 客户端重传是常态（`send_text_with_retry` 明确依赖服务端按 `client_msg_id` 去重，`test_stress_03.cpp:341-359`） |
| 去掉发消息前的关系校验 | 陌生人/拉黑拦截是产品不变量；应"缓存"它，不是"取消"它 |
| 用 Debug 客户端测服务端 | 数据不可信（§6.3） |
| 直接把 bcrypt 换成更快的哈希（MD5/SHA/裸 SHA256）来"提性能" | 安全倒退。降 cost 是"在基线内调参"，换算法不是同一个决策 |
| 只调大 `DB_POOL_SIZE`/worker 数 | §1.2/§1.1 已证明：CPU 受限时加线程只会更糟（超订 + 抢占 MariaDB） |
| 一次性动 `offline_messages` 语义 | 需先出多端投递/回溯的设计结论（P1-4） |

**灰度与回滚原则**：

1. P0-1（cost）/P0-5（构建）/P0-7（部署参数）/P1-5（NODELAY）为**参数级**，可独立上线、独立回滚。
2. P0-2（拆池）、P1-1/P1-2（消息路径）、P1-3（补推合并）为**结构级**，每项都要带开关或按 DAO 粒度分别合入，并用 `test_stress_03 --friends 20`（快、可重复）作为回归基线。
3. P0-3（refresh token）需要**服务端先发、客户端后改**的顺序，并保留密码登录通路。
4. 每一项上线后必须重跑 §7 的观测对照，把"斜率"作为验收指标（`login_max` 对 N 的斜率：现状 138ms/用户 ⇒ 目标 <5ms/用户）。

---

## 9. 建议实施顺序（可直接排期）

**第 1 批（1 天内，先拿"看得见的"收益并停止误诊）**
`P0-5` Release 构建 + 客户端 Release + `LimitNOFILE` + `P1-5` `TCP_NODELAY` + `P2-1` 观测埋点（至少队列等待 + bcrypt 耗时）。
→ 产出：一份带 CPU/队列曲线的重跑数据，**证实或推翻 §1.2 的拟合**。

**第 2 批（2~3 天，直击 13 秒）**
`P0-1` cost=10 + 哈希并发=核数；`P0-2` 拆 hash/sql 池且把 bcrypt 移出 `PooledConn` 作用域；`P0-4` 限流+背压+高水位；`P0-6` 削减往返。
→ 目标：N=100 login p50 < 700ms，非 auth 接口与登录风暴解耦。

**第 3 批（1 周，稳态吞吐）**
`P0-3` refresh token + 客户端错峰重连（**收益最大**）；`P1-1`/`P1-2` 消息路径合并与缓存；`P1-3` 补推合并 + 心跳周期调整；`P1-6` HTTP keep-alive。
→ 目标：重连风暴不触发 bcrypt；`tcp send` p50 <150ms。

**第 4 批（按需，规模再上台阶时）**
`P1-4` 游标替代离线队列（先出设计）；`P1-7`/`P1-8` 批量写与合并推；`P0-7`/`P3` DB 或哈希外置、或升 4C4G。

---

## 10. 附录 A：定位索引（本文引用的全部代码位置）

| 主题 | 位置 |
|---|---|
| 服务端入口 / 线程与池配置 | `main.cpp:57-120`（池 100/worker 16 默认在 `:71-78`，io 线程 `:100-102`） |
| 部署环境变量与 systemd 单元 | `build.sh:9-13`、`build.sh:18-42`、`build.sh:253-259` |
| bcrypt 哈希/校验（核心瓶颈） | `db.cpp:454`、`db.cpp:513`；注释 `db.cpp:426` |
| 任务队列与 worker | `db.h:293-303`、`db.cpp:330-388` |
| 连接池借还 + ping | `db.cpp:161-304`（`isValid` `:213-214`、`:250-253`）、`db.cpp:309-323` |
| 事务重试 | `db.cpp:69-102` |
| register / login DAO | `db.cpp:430-480`、`db.cpp:485-527` |
| sendMessage 事务（8 语句） | `db.cpp:532-657` |
| 好友 DAO（申请/通过/列表/删除/拉黑/关系校验） | `db.cpp:895`、`db.cpp:1035`、`db.cpp:1169`、`db.cpp:1260`、`db.cpp:1316`、`db.cpp:1396` |
| 登录补推 | `session.cpp:183-195`、`session.cpp:559-598`、`session.cpp:613-685`、`session.cpp:469-534` |
| TCP 登录只验 JWT | `session.cpp:143-160` |
| 消息发送链（两段回调） | `session.cpp:201-292` |
| 写队列 / 空闲超时 | `session.cpp:714-740`、`session.h:39`、`session.h:93-95` |
| accept（无 socket 选项） | `server.cpp:32-41`、`http_server.cpp:1327-1336` |
| HTTP 短连接与响应 | `http_server.cpp:410-417`、`http_server.cpp:659-683`、`http_server.cpp:1282-1298` |
| 在线表全局锁 | `session_manager.cpp:13-68` |
| 日志静默开关 | `quiet_log.h:12-20`、`build.sh:13` |
| 构建优化等级缺失 | `CMakeLists.txt`（全文无 `CMAKE_BUILD_TYPE`）、`third-party/libbcrypt/CMakeLists.txt:38-39`（子工程自带 `-O3`） |
| MariaDB 配置 | `prepare_env_V0.sh:302-323` |
| 表结构与索引 | `db/schema.sql:51-419`、`db/migrate_add_friends_index.sql` |
| 压测客户端串行回复模型 | `test_stress_03.cpp:734-762` |
| 压测栅栏与阶段推进 | `test_stress_03.cpp:319-337`、`test_stress_03.cpp:680` |
| 客户端 TCP 登录 10s 等待 | `test_stress_03.cpp:231-239` |
| 客户端 5s 心跳 | `test_stress_03.cpp:266-280` |

## 附录 B：既有文档交叉引用

- 死锁根因与修复（已落地）：`dev/IMS/KC-CXIM-Svc/basic_service_20261107/fixme.md`（P0-1 消除 gap lock、P0-2 事务重试、P1 写顺序/错误码、P2 调优复压）。
- 好友列表覆盖索引（已落地，扫描行 651→21）：`fixme.md` 索引分析章节 + `db/migrate_add_friends_index.sql`。
- 已实施但未见效的尝试（本文给出解释）：连接池懒创建、`DB_POOL_SIZE/DB_WORKER_COUNT` 扩容、多线程 `io.run()`、每连接 strand —— 见 `main.cpp:64-82`、`main.cpp:96-120`、`session.cpp:28-36`、`session.h:82-86`。**它们解决的是并发度，而瓶颈是 CPU 秒数，因此曲线不变。**
