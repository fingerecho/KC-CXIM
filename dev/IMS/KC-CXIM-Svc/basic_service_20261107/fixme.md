test_stress\_02的压测结果：
```
./build/bin/Debug/test_stress_02 --host 110.40.206.253 --threads 50 --duration 60 --warmup 5
[stress] host=110.40.206.253 http=8080 tcp=8888 threads=50 duration=60s warmup=5s hb=5s
[stress] preparing 50 workers...
[stress] prepare 36 failed: create friend request failed: HTTP 400 {"code":1,"message":"db error: (conn=2418) Deadlock found when trying to get lock; try restarting transaction"}
[stress] prepare 28 failed: create friend request failed: HTTP 400 {"code":1,"message":"db error: (conn=2421) Deadlock found when trying to get lock; try restarting transaction"}
[stress] prepare 8 failed: create friend request failed: HTTP 400 {"code":1,"message":"db error: (conn=2421) Deadlock found when trying to get lock; try restarting transaction"}
[stress] prepare 1 failed: create friend request failed: HTTP 400 {"code":1,"message":"db error: (conn=2418) Deadlock found when trying to get lock; try restarting transaction"}
[stress] preparation failed (46/50), abort
```

这个结果说明：压测客户端把服务端的数据库死锁暴露出来了，这是一个真实且有价值的问题，不是测试脚本的 bug。
50 个线程在准备阶段同时调用 POST /api/friends/requests 创建好友申请，其中 4 个（线程 1、8、28、36）收到了 MySQL 的：
```
Deadlock found when trying to get lock; try restarting transaction
```
MySQL 检测到事务间出现循环等待锁，主动回滚了其中一个事务。这是 InnoDB 的自我保护机制，不是 MySQL 挂了。但它说明：服务端处理“创建好友申请”的代码在并发下存在锁竞争，且没有处理死锁重试。

为什么会在“准备阶段”就炸
准备阶段是 50 个线程同时执行同一套操作：注册 → 登录 → A 申请 B → B 通过。后两步会写 friend_requests 表，且 A、B 两个用户之间是双向写（A 申请写一条，B 通过又更新/写另一条）。当多对用户同时进入这个流程时，InnoDB 的行锁/间隙锁很容易形成循环等待：
```
线程 X 持有用户 A 相关行的锁，等用户 B 的锁；

线程 Y 持有用户 B 相关行的锁，等用户 A 的锁；

互相等待 → 死锁。
```
你的 test_stress\_01 没暴露这个问题，是因为它每轮串行全链路、并发度低、且每轮用户不同、时间错开。 test_stress\_02 把 50 个“建立好友关系”的操作压到一起，锁竞争瞬间放大，死锁就出现了。这恰恰说明新的压测客户端比旧的有效。

这是服务端 bug，不是客户端问题
客户端收到 HTTP 400 并计入失败是正确行为。真正的问题是服务端：

没有对死锁做重试。MySQL 死锁是瞬时、可恢复的错误（错误码 1213），标准做法是捕获它并自动重试整个事务（通常重试 1~3 次）。服务端直接把它当 400 返回给客户端，说明业务层没做这个处理。

事务可能过大或加锁顺序不一致。“创建好友申请”如果在一个事务里做了多张表、多行的读写，且不同请求的加锁顺序不同，就极易死锁。需要检查这个接口的事务边界和 SELECT ... FOR UPDATE 的加锁顺序。

唯一索引/间隙锁：如果 friend_requests 表有 (from_user, to_user) 之类的唯一约束，并发插入相同或相邻键值会触发间隙锁竞争，也容易死锁。

你现在应该做什么

第一步：确认这是服务端该修的，而不是调小客户端并发绕过。
把客户端并发降到 20 或 10 再跑一次，如果死锁消失，说明是并发度触发的锁竞争——这仍然是服务端要解决的问题，因为真实场景下并发不会因为测试调低就消失。压测的意义就是把它压出来。

第二步：去服务端看“创建好友申请”的代码。
重点看：
```
这个接口的事务里涉及哪些表、哪些行、加锁顺序是否固定；

是否有 SELECT ... FOR UPDATE，多个并发请求的加锁顺序是否可能相反；

是否在事务里做了“先查再插”且没有唯一约束兜底。
```
第三步：在服务端加死锁重试。
典型模式（伪代码）：
```
cpp
for (int attempt = 0; attempt < 3; ++attempt) {
    try {
        begin_transaction();
        // ... 业务读写 ...
        commit();
        break;
    } catch (const DbError& e) {
        rollback();
        if (e.code == 1213 /* deadlock */ || e.code == 1205 /* lock wait timeout */)
            continue;  // 重试
        throw;
    }
}
```
注意：重试必须整个事务重来，不能只重试失败的那条 SQL。

第四步：修完后重新压测。
死锁被重试吸收后，preparation failed 应该消失，客户端能进入施压阶段，你才会看到真正的 QPS 和延迟分位数。届时如果还有死锁，说明重试次数不够或锁设计仍有问题，继续修。

一句话总结
这个结果是压测成功的标志——它证明了你的新客户端确实能对服务端施加有效并发压力，并且立刻发现了“创建好友申请接口在并发下会死锁且不重试”这个真实缺陷。不要去改客户端绕过它，去服务端加死锁重试并检查事务加锁顺序，修完再压，才能得到可信的性能数据。

---

# 压测死锁分析与修复方案（追加）

## 一、Bug 原因分析（对应第 1~10 行）

### 1.1 结论

压测暴露的是服务端 `Db::createFriendRequest` 的 **间隙锁（gap lock）与插入意向锁（insert intention lock）循环等待**，不是压测客户端的问题。错误里的 `(conn=2418)` 是数据库连接 id，`1213` 是 InnoDB 检测到循环等待后主动回滚事务的标准错误码。

### 1.2 触发链路

压测准备阶段 `prepare_one`（`step2_test_client/test_stress_02.cpp:272-309`）对每个 worker 执行：

```
register → login → A 申请 B（POST /api/friends/requests）→ B 通过
```

死锁发生在“A 申请 B”这一步。核心代码在 `Db::createFriendRequest`（`step1_tcp_echo/db.cpp:788-912`）：

- `db.cpp:800`：`conn->setAutoCommit(false)` 开启事务；MariaDB 默认隔离级别是 **REPEATABLE READ**。
- `db.cpp:851-861`：先执行

  ```sql
  SELECT id, status FROM friend_requests
   WHERE requester_id=? AND target_id=? FOR UPDATE
  ```

- `db.cpp:877-885`：再执行

  ```sql
  INSERT INTO friend_requests(requester_id,target_id,message,status,result_notified)
  VALUES(?,?,?,0,0)
  ```

### 1.3 死锁为什么发生

1. 新申请的 `(requester_id, target_id)` 在唯一索引 `uk_friend_requests_pair(requester_id, target_id)`（`db/schema.sql`）中**不存在**。
2. 在 REPEATABLE READ 下，对唯一索引做锁定读（`FOR UPDATE`）即使**查不到行，也会加 gap lock**，锁住“该键应插入的间隙”。
3. 50 个 worker 的 requester/target 都是新注册、递增的 userId，新键几乎都落在索引末尾同一个 **supremum gap**，于是多个事务对**同一个间隙**持有 gap lock。**gap lock 之间互相兼容**，所以它们可以同时持有。
4. 接着每个事务执行 INSERT，需要申请 **插入意向锁**；插入意向锁与“其他事务持有的 gap lock”**不兼容**。
5. 于是：T1 的 INSERT 等 T2 的 gap lock，T2 的 INSERT 等 T1 的 gap lock → 循环等待 → InnoDB 回滚其中一个，返回 1213 `Deadlock found when trying to get lock`。

一句话：**`SELECT ... FOR UPDATE` 在“查不到行”时加的 gap lock，和后续并发 INSERT 需要的插入意向锁互相冲突，是本次死锁的直接原因。**

### 1.4 放大因素与相关线索

- DB 线程池只有 **2 个 worker、4 条连接**（`step1_tcp_echo/main.cpp:60`）。实际并发事务只有 2 个——**两个事务就足以触发上述死锁**，不需要 50 个真正并行；50 个客户端只是把这两个 worker 挤在一起，提高了撞上的概率。
- 死锁异常被 `db.cpp:906-908` 统一转换成字符串 `db error: ...`，HTTP 层 `http_server.cpp:557-558` 再统一按 **400** 返回。死锁属于瞬时可重试错误，却被当成永久业务错误返回，导致压测准备阶段直接失败。
- 潜在同类问题（本次未触发）：`Db::handleFriendRequest`（`db.cpp:923-1000`）通过时用一条语句写 friends 双向两行：

  ```sql
  INSERT INTO friends(user_id,friend_id,status)
  VALUES(?,?,1),(?,?,1) ON DUPLICATE KEY UPDATE status=1
  ```

  写入顺序固定为 `(userId, requesterId)`、`(requesterId, userId)`（`db.cpp:957-964`）。若 A→B 和 B→A 两个申请被同时通过，两个事务对 friends 主键的加锁顺序相反，可能死锁。本次压测每对用户不同，所以不是本次原因，但属于应一并修复的隐患。

---

## 二、第 11~78 行文档可行性分析

### 2.1 总评

**方向正确、整体可行（约八成正确），但有两点需要修正：**

1. 对死锁成因的描述不够精确：本次不是“双向写 / 加锁顺序不一致”，而是 `SELECT ... FOR UPDATE` 查不到行时加 gap lock，与并发 INSERT 的插入意向锁冲突。
2. 只按第 54~72 行“加重试”是**治标不治本**：重试能把死锁吸收掉让压测跑起来，但会带来额外延迟、连接占用和重试耗尽风险；应与“消除间隙锁来源”一起做。

### 2.2 逐条结论

| 文档位置 | 结论 | 说明 / 修正 |
| --- | --- | --- |
| 第 13 行：真实服务端问题，不是客户端 bug | ✅ 正确 | 客户端收到 400 计入失败是合理的。 |
| 第 14~18 行：MySQL 检测循环等待并回滚，1213 是自我保护 | ✅ 正确 | `conn=xxxx` 是连接 id；1213 是死锁，1205 是锁等待超时。 |
| 第 20~29 行：准备阶段双向写导致行锁/间隙锁循环 | ⚠️ 部分正确 | “间隙锁竞争”方向对；但“创建申请”只写一行 `friend_requests`，并不存在双向写，双向写发生在“通过申请”写 friends。真正的直接原因是 `FOR UPDATE` 对不存在的唯一键加 gap lock，再与并发 INSERT 的插入意向锁冲突。 |
| 第 31~34 行：服务端没做死锁重试 | ✅ 正确 | `db.cpp:906-908` 把 `sql::SQLException` 全部当普通 db error，未区分 1213/1205。 |
| 第 36 行：事务过大 / 加锁顺序不一致 / `SELECT ... FOR UPDATE` | ⚠️ 部分正确 | `FOR UPDATE` 确有问题，但事务本身很小、是单行操作；“加锁顺序相反”不是本次的触发条件。真正的毛病是“**在不存在的唯一键上做锁定读**”。 |
| 第 38 行：唯一索引 / 间隙锁，并发插入相邻键触发竞争 | ✅ 基本命中 | 最接近真相，但需补充“`SELECT ... FOR UPDATE` 先加 gap lock，随后 INSERT 需要插入意向锁”这一关键链条。 |
| 第 42~43 行：先降低客户端并发确认 | ⚠️ 可做但不可靠 | 由于服务端只有 2 个 DB worker，降到 10/20 仍可能死锁；该步骤只能辅助定位，不能作为修复。 |
| 第 45~53 行：检查事务涉及的表/行/加锁顺序 | ✅ 正确 | 建议重点看 `db.cpp:851-861` 的 `FOR UPDATE` 和 `db.cpp:877-885` 的 INSERT。 |
| 第 54~72 行：事务级死锁重试 | ✅ 正确但需补充 | 必须整事务重来、只重试 1213/1205、限制次数、回滚后再试、`BizError`/1062 不重试；伪代码思路没问题。 |
| 第 74~75 行：修完重新压测 | ✅ 正确 | 建议同时检查 `SHOW ENGINE INNODB STATUS` 的 LATEST DETECTED DEADLOCK。 |
| 第 77~78 行：一句话总结 | ✅ 方向正确 | 补充：先消除 gap lock 根源，再用重试兜底，不能只靠重试。 |

**可行性结论：第 11~78 行文档整体可行，可作为修复方向；但需按下面第三节修正实施顺序和具体做法。**

---

## 三、改善 / 解决压测死锁的方案

按优先级落地，**先治本（去掉 gap lock），再兜底（重试），最后调优**。

### P0-1 消除 `createFriendRequest` 的间隙锁根源（治本）

目标：不要在“可能不存在的唯一键”上做 `SELECT ... FOR UPDATE`。

**推荐做法（最小改动）：去掉 `FOR UPDATE`，用唯一键兜底并发**

```cpp
// 1) 原有的只读检查保持不变（target 存在、已是好友、被拉黑、反向 pending）
//    注意：这里全部用普通 SELECT，不要 FOR UPDATE。

// 2) 直接尝试插入，由 uk_friend_requests_pair 保证并发安全
std::uint64_t reqId = 0;
try {
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
        "INSERT INTO friend_requests"
        "(requester_id,target_id,message,status,result_notified) "
        "VALUES(?,?,?,0,0)"));
    st->setUInt64(1, requesterId);
    st->setUInt64(2, targetId);
    st->setString(3, message);
    st->executeUpdate();
    reqId = lastInsertId(conn);
} catch (const sql::SQLException& e) {
    if (e.getErrorCode() != 1062) throw;      // 1062 = duplicate key，其余继续抛

    // 3) 已存在：普通读状态，决定“重复申请 / 已是好友 / 重置为 pending”
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
        "SELECT id,status FROM friend_requests "
        "WHERE requester_id=? AND target_id=?"));
    st->setUInt64(1, requesterId);
    st->setUInt64(2, targetId);
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    if (!rs->next()) throw;                    // 极少数竞态，交给重试
    reqId = rs->getUInt64(1);
    const int oldStatus = rs->getInt(2);

    if (oldStatus == 0) throw BizError("request already pending");
    if (oldStatus == 1) throw BizError("already friends");

    // 4) 被拒绝/取消后重新申请：带状态条件更新，避免并发覆盖
    std::unique_ptr<sql::PreparedStatement> up(conn->prepareStatement(
        "UPDATE friend_requests "
        "SET message=?, status=0, result_notified=0, "
        "    created_at=CURRENT_TIMESTAMP(3) "
        "WHERE id=? AND status IN (2,3)"));
    up->setString(1, message);
    up->setUInt64(2, reqId);
    up->executeUpdate();
}
```

这样并发路径变成“直接 INSERT”，不再先加 gap lock；不同键的插入意向锁之间不冲突，可从根本上避免本次死锁。插入阶段仍可能出现的同键竞态由唯一键 + 重试兜底。

**备选方案：**

- **方案 A：`INSERT ... ON DUPLICATE KEY UPDATE`**
  ```sql
  INSERT INTO friend_requests(requester_id,target_id,message,status,result_notified)
  VALUES(?,?,?,0,0)
  ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id);
  ```
  然后用 `LAST_INSERT_ID()` 拿到 id，再普通读状态做业务判断。注意同键高并发时 `ON DUPLICATE KEY UPDATE` 本身也可能死锁，仍需 P0-2 重试兜底。

- **方案 C：改用 READ COMMITTED 隔离级别**
  ```cpp
  conn->setTransactionIsolation(sql::TRANSACTION_READ_COMMITTED);
  // 或执行：SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED
  ```
  READ COMMITTED 下，对不存在的行做锁定读**不会加 gap lock**，也能消除本次死锁。但它会改变其他事务的可见性/幻读语义，需回归测试全部 DB 用例后才可全局启用。建议作为 P0-1 的补充而不是唯一手段。

### P0-2 事务级死锁重试（兜底）

对 `createFriendRequest`、`handleFriendRequest`、`sendMessage` 等写事务统一加一层重试：

```cpp
constexpr int kMaxAttempts = 3;          // 1 次正常 + 2 次重试
for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    try {
        conn->setAutoCommit(false);
        // ... 整个事务的全部 SQL ...
        conn->commit();
        break;
    } catch (const BizError&) {
        try { conn->rollback(); } catch (...) {}
        throw;                            // 业务错误不重试
    } catch (const sql::SQLException& e) {
        try { conn->rollback(); } catch (...) {}
        const int code = e.getErrorCode();
        if ((code == 1213 || code == 1205) && attempt < kMaxAttempts) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5 * attempt + (std::rand() % 20)));
            continue;                     // 整个事务重来
        }
        throw;
    } catch (...) {
        try { conn->rollback(); } catch (...) {}
        throw;
    }
}
```

注意点：

- **必须整个事务重来**，不能只重试失败的单条 SQL。
- 只重试 `1213`（死锁）和 `1205`（锁等待超时）；`1062`、`BizError` 等不重试。
- 每次失败先 `rollback`，重试前可加少量随机退避，避免再次撞锁。
- 重试次数有上限，超过后返回可重试的服务端错误（见 P1-2）。
- 回调只允许触发一次；读回结果（`kRequestSelect`）建议也放在重试范围内。

### P1-1 修复 `handleFriendRequest` 的加锁顺序隐患

通过申请时，把 friends 的两行按 **userId 升序**写入，保证所有事务加锁顺序一致：

```cpp
std::uint64_t lo = std::min(userId, requesterId);
std::uint64_t hi = std::max(userId, requesterId);
// INSERT INTO friends VALUES(lo,hi,1),(hi,lo,1) ON DUPLICATE KEY UPDATE status=1
```

这样即使 A→B 与 B→A 被同时通过，也不会因为 friends 主键加锁顺序相反而死锁。

### P1-2 区分“业务错误”和“瞬时数据库错误”的 HTTP 状态码

- `BizError`（已是好友、重复申请、被拉黑等）继续返回 **400**。
- 死锁/锁等待/连接异常等瞬时错误，重试耗尽后返回 **503**（可带 `Retry-After`），而不是 400。
- 压测客户端可对 503 做有限次退避重试；但**修复的主体仍是服务端**，不能靠客户端绕过。

### P2 连接池与 DB 并发调优

当前 `main.cpp:60` 是 `pool_size=4, worker_count=2`。修复死锁后，建议按实际压测逐步上调（例如 8~16 连接、4~8 worker），否则 2 个 DB worker 会把 50 个客户端的所有 DB 操作串行化，QPS 上不去、延迟虚高。同时建议：

- 为事务增加合理的语句/事务超时，避免长事务持锁。
- 记录 1213/1205 次数、重试次数、重试成功率到日志或指标。

### 验证方法

1. 应用 P0-1 + P0-2 后，用 10 / 20 / 50 / 100 线程分别重跑 `test_stress_02`，准备阶段应全部成功并进入施压阶段。
2. 压测后执行 `SHOW ENGINE INNODB STATUS\G`，确认 `LATEST DETECTED DEADLOCK` 无新增；验证期间可临时开启 `innodb_print_all_deadlocks=ON`。
3. 统计日志中 1213/1205 出现次数与重试成功率；理想状态是死锁基本消失，重试只作为兜底很少触发。
4. 对比修复前后的 QPS、p95/p99 延迟和 prepare 成功率，再决定是否上调连接池/worker。

### 最终结论

**第 11~78 行文档可行，但需修正成因判断与实施顺序：** 本次死锁的直接原因是 `SELECT ... FOR UPDATE` 在唯一键不存在时加 gap lock，与并发 INSERT 的插入意向锁互相等待，并非“双向写 / 加锁顺序相反”。推荐按 **P0-1 去掉间隙锁 → P0-2 事务重试兜底 → P1 修 friends 写入顺序和错误码 → P2 调优并复压** 的顺序实施；只加重试会掩盖问题，只有两者结合才能既让压测跑通，又拿到真实可信的性能数据。

---

# 好友列表查询补索引记录（EXPLAIN + MariaDB 实测）

## 一、为什么补索引

“我的好友列表”查询在 `step1_tcp_echo/db.cpp:1105` 的 `Db::listFriends`：

```sql
SELECT u.id, u.username, u.nickname, u.avatar_url, f.remark, f.status
FROM friends f JOIN users u ON u.id=f.friend_id
WHERE f.user_id=? AND f.status=1
ORDER BY u.id ASC
```

`friends` 原有索引（`db/schema.sql`）：

```sql
PRIMARY KEY (user_id, friend_id)
KEY idx_friends_friend (friend_id)
```

存在三个问题：

1. **主键能按 `user_id` 过滤，但不包含 `status`**  
   `status=1` 只能在回表后过滤；如果某个用户存在大量拉黑行（status=2），会多扫描无用的行。
2. **`ORDER BY u.id` 是按 join 后的 users 列排序**  
   优化器不一定能识别它与 `f.friend_id` 等价，容易额外产生 filesort。
3. **friends 侧的 `remark` 需要回表读取**  
   查询只需要 friends 的 `friend_id/status/remark` 和 users 的公开资料；如果索引把 `remark` 也带上，friends 侧可完全由索引覆盖。

因此补一个**覆盖型复合索引**：

```sql
(user_id, status, friend_id, remark)
```

- `user_id=?` 等值 → `status=1` 等值 → `friend_id` 天然有序；
- 同时满足 `WHERE` 和 `ORDER BY f.friend_id`，避免 filesort；
- `status` 和 `remark` 都在索引里，friends 侧无需回表（`Using index`）。

---

## 二、用 EXPLAIN 查好友列表 SQL（已实测）

实测环境：MariaDB 11.8.6，`friends` 初始 892 行。

### 2.1 查前先看索引和真实 userId

```sql
SHOW INDEX FROM friends;

SELECT user_id, COUNT(*) AS c
FROM friends WHERE status=1
GROUP BY user_id ORDER BY c DESC LIMIT 3;
-- 本次取到 user_id=1388
```

### 2.2 EXPLAIN 原 SQL

```sql
EXPLAIN
SELECT u.id, u.username, u.nickname, u.avatar_url, f.remark, f.status
FROM friends f JOIN users u ON u.id=f.friend_id
WHERE f.user_id=1388 AND f.status=1
ORDER BY u.id ASC;
```

补索引前、初始小数据（每个用户只有 1 个好友）时结果：

```text
table  type    possible_keys                          key      rows  Extra
f      ref     PRIMARY,idx_friends_friend             PRIMARY  1     Using where
u      eq_ref  PRIMARY                                PRIMARY  1
```

因为数据太少，优化器直接走 PRIMARY，暂时看不到 filesort。

### 2.3 用测试数据放大后对比

为了验证索引效果，临时给 user_id=1388 造了 651 行测试数据（21 条 `status=1`，630 条 `status=2`），`ANALYZE TABLE friends` 后对比：

**只建 3 列索引 `(user_id,status,friend_id)` 时：**

```text
table  type    possible_keys                                                key      rows  Extra
f      ref     PRIMARY,idx_friends_friend,idx_friends_user_status           PRIMARY  651   Using where
u      eq_ref  PRIMARY                                                      PRIMARY  1
```

优化器仍然选择 PRIMARY：3 列索引不覆盖 `remark`，需要回表，而 PRIMARY 是聚簇索引、顺序读更便宜。

**升级为覆盖索引 `idx_friends_user_status(user_id,status,friend_id,remark)` 后：**

```text
table  type    possible_keys                                                          key                      key_len  ref          rows  Extra
f      ref     PRIMARY,idx_friends_friend,idx_friends_user_status                     idx_friends_user_status  9        const,const  21    Using where; Using index
u      eq_ref  PRIMARY                                                                PRIMARY                  8        CXIMDB.f...  1
```

结果：

- `key = idx_friends_user_status`；
- `rows` 从 651 降到 21；
- `Extra = Using where; Using index`，friends 侧无回表；
- 没有 `Using filesort`。

测试数据随后已清理，`friends` 恢复为 892 行，索引保留。

### 2.4 最终实测（清理测试数据后，真实查询）

清理测试数据、保留最终覆盖索引后，对真实用户 1388 再次 EXPLAIN：

```text
table  type    possible_keys                                                       key                      key_len  ref          rows  Extra
f      ref     PRIMARY,idx_friends_friend,idx_friends_user_status                  idx_friends_user_status  9        const,const  1     Using where; Using index
u      eq_ref  PRIMARY                                                             PRIMARY                  8        CXIMDB.f...  1
```

即使当前每个用户只有 1 个好友，优化器也选择了覆盖索引，并出现 `Using index`；说明索引与改写后的 `ORDER BY f.friend_id` 完全匹配，且 friends 侧无需回表。

---

## 三、补索引的具体解决方案

### 3.1 新库：修改 `db/schema.sql`

在 `friends` 建表语句的索引区域加入（已修改）：

```sql
    PRIMARY KEY (user_id, friend_id),
    KEY idx_friends_friend (friend_id),
    -- /*************补索引增强MariaDB查询性能********************/
    -- 覆盖「我的好友列表」查询：WHERE user_id=? AND status=1 ORDER BY friend_id。
    -- remark 也放进索引，避免回表取备注，EXPLAIN 可出现 Using index。
    KEY idx_friends_user_status (user_id, status, friend_id, remark),
    -- /*************补索引增强MariaDB查询性能********************/
```

### 3.2 已有库：执行迁移 SQL

已新增 `db/migrate_add_friends_index.sql`，核心语句：

```sql
-- /*************补索引增强MariaDB查询性能********************/
ALTER TABLE friends
  ADD INDEX idx_friends_user_status (user_id, status, friend_id, remark);
-- /*************补索引增强MariaDB查询性能********************/

ANALYZE TABLE friends;
```

本次已在目标库实际执行成功，`SHOW INDEX FROM friends` 确认存在：

```text
idx_friends_user_status  (user_id, status, friend_id, remark)
```

> 若索引已存在，重复执行会报 1061 Duplicate key name，可忽略。

### 3.3 C++：调整 `Db::listFriends` 的排序

`step1_tcp_echo/db.cpp` 中把 `ORDER BY u.id ASC` 改为 `ORDER BY f.friend_id ASC`（已修改）：

```cpp
std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
    /*************补索引增强MariaDB查询性能********************/
    // ORDER BY 改为 friends.friend_id：与索引
    // idx_friends_user_status(user_id,status,friend_id,remark) 的列顺序对齐，
    // 且 u.id == f.friend_id，返回顺序不变；避免额外 filesort。
    "SELECT u.id, u.username, u.nickname, u.avatar_url, f.remark, f.status "
    "FROM friends f JOIN users u ON u.id=f.friend_id "
    "WHERE f.user_id=? AND f.status=1 "
    "ORDER BY f.friend_id ASC"
    /*************补索引增强MariaDB查询性能********************/
    ));
```

`u.id = f.friend_id`，所以返回顺序完全等价；但这样排序条件能直接落在 friends 的索引列上。

### 3.4 验证

```sql
SHOW INDEX FROM friends;
ANALYZE TABLE friends;

EXPLAIN
SELECT u.id, u.username, u.nickname, u.avatar_url, f.remark, f.status
FROM friends f JOIN users u ON u.id=f.friend_id
WHERE f.user_id=1388 AND f.status=1
ORDER BY f.friend_id ASC;
```

验收标准：

- `friends` 侧 `type=ref`、`key=idx_friends_user_status`；
- `users` 侧 `type=eq_ref`、`key=PRIMARY`；
- `Extra` 不出现 `Using filesort`；
- 数据量大且 `status=2` 较多时，`rows` 明显小于主键扫描行数，并出现 `Using index`。

## 四、结论

好友列表的瓶颈不在 `users` 表（主键 eq_ref 已经很好），而在 `friends` 的“按 user_id + status 过滤并按 friend_id 排序”。补 `(user_id, status, friend_id, remark)` 覆盖索引，并把 C++ 的 `ORDER BY u.id` 改为 `ORDER BY f.friend_id` 后，实测 EXPLAIN 可使用该索引、扫描行数从 651 降到 21，并出现 `Using index`、无 filesort。索引已在目标库生效，新库会由 `db/schema.sql` 自动创建。



./build/bin/Release/test_stress_03.exe --host 110.40.206.253 --http-port 10002 --tcp-port 10001 --friends 100 --timeou
t 240
[stress03] host=110.40.206.253 http=10002 tcp=10001 friends=100 runId=977315726 hb=30s timeout=240s
[stress03] main userId=5267, starting 100 friend workers...
[stress03] fatal: friends not ready: 47/100
[stress03] friend 0 error: aborted before accept
[stress03] friend 1 error: register s3F_977315726_1 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 2 error: aborted before accept
[stress03] friend 3 error: aborted before accept
[stress03] friend 4 error: aborted before accept
[stress03] friend 5 error: aborted before accept
[stress03] friend 6 error: aborted before accept
[stress03] friend 7 error: aborted before accept
[stress03] friend 8 error: aborted before accept
[stress03] friend 9 error: aborted before accept
[stress03] friend 10 error: aborted before accept
[stress03] friend 11 error: register s3F_977315726_11 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 12 error: aborted before accept
[stress03] friend 13 error: register s3F_977315726_13 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 14 error: aborted before accept
[stress03] friend 15 error: aborted before accept
[stress03] friend 16 error: aborted before accept
[stress03] friend 17 error: register s3F_977315726_17 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 18 error: aborted before accept
[stress03] friend 19 error: aborted before accept
[stress03] friend 20 error: aborted before accept
[stress03] friend 21 error: aborted before accept
[stress03] friend 22 error: aborted before accept
[stress03] friend 23 error: aborted before accept
[stress03] friend 24 error: aborted before accept
[stress03] friend 25 error: register s3F_977315726_25 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 26 error: aborted before accept
[stress03] friend 27 error: aborted before accept
[stress03] friend 28 error: register s3F_977315726_28 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 29 error: register s3F_977315726_29 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 30 error: aborted before accept
[stress03] friend 31 error: register s3F_977315726_31 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 32 error: aborted before accept
[stress03] friend 33 error: aborted before accept
[stress03] friend 34 error: register s3F_977315726_34 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 35 error: aborted before accept
[stress03] friend 36 error: aborted before accept
[stress03] friend 37 error: aborted before accept
[stress03] friend 38 error: aborted before accept
[stress03] friend 39 error: aborted before accept
[stress03] friend 40 error: aborted before accept
[stress03] friend 41 error: register s3F_977315726_41 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 42 error: aborted before accept
[stress03] friend 43 error: aborted before accept
[stress03] friend 44 error: aborted before accept
[stress03] friend 45 error: aborted before accept
[stress03] friend 46 error: aborted before accept
[stress03] friend 47 error: register s3F_977315726_47 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 48 error: aborted before accept
[stress03] friend 49 error: aborted before accept
[stress03] friend 50 error: register s3F_977315726_50 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 51 error: aborted before accept
[stress03] friend 52 error: aborted before accept
[stress03] friend 53 error: aborted before accept
[stress03] friend 54 error: aborted before accept
[stress03] friend 55 error: aborted before accept
[stress03] friend 56 error: aborted before accept
[stress03] friend 57 error: register s3F_977315726_57 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 58 error: aborted before accept
[stress03] friend 59 error: aborted before accept
[stress03] friend 60 error: register s3F_977315726_60 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 61 error: aborted before accept
[stress03] friend 62 error: aborted before accept
[stress03] friend 63 error: register s3F_977315726_63 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 64 error: register s3F_977315726_64 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 65 error: register s3F_977315726_65 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 66 error: aborted before accept
[stress03] friend 67 error: aborted before accept
[stress03] friend 68 error: register s3F_977315726_68 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 69 error: aborted before accept
[stress03] friend 70 error: aborted before accept
[stress03] friend 71 error: aborted before accept
[stress03] friend 72 error: register s3F_977315726_72 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 73 error: register s3F_977315726_73 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 74 error: aborted before accept
[stress03] friend 75 error: aborted before accept
[stress03] friend 76 error: aborted before accept
[stress03] friend 77 error: register s3F_977315726_77 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 78 error: register s3F_977315726_78 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 79 error: aborted before accept
[stress03] friend 80 error: register s3F_977315726_80 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 81 error: aborted before accept
[stress03] friend 82 error: aborted before accept
[stress03] friend 83 error: aborted before accept
[stress03] friend 84 error: register s3F_977315726_84 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 85 error: aborted before accept
[stress03] friend 86 error: aborted before accept
[stress03] friend 87 error: aborted before accept
[stress03] friend 88 error: register s3F_977315726_88 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 89 error: aborted before accept
[stress03] friend 90 error: aborted before accept
[stress03] friend 91 error: aborted before accept
[stress03] friend 92 error: aborted before accept
[stress03] friend 93 error: register s3F_977315726_93 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 94 error: register s3F_977315726_94 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 95 error: register s3F_977315726_95 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 96 error: aborted before accept
[stress03] friend 97 error: aborted before accept
[stress03] friend 98 error: register s3F_977315726_98 failed: HTTP 429 {"code":1,"message":"too many auth requests"}
[stress03] friend 99 error: register s3F_977315726_99 failed: HTTP 429 {"code":1,"message":"too many auth requests"}

===== Stress03 Aborted =====
friends ready   : 70/100
accepted        : 0/100
messages        : 0/100
listed          : 0/100
deleted         : 0/100
failed          : 101
============================

● 这个 429 是我上一轮加的认证限流在挡你，不是功能坏了。含义和数字如下。

  它在说什么

  服务端默认给 /api/register、/api/login、/api/token/refresh 套了两层令牌桶（http_server.cpp 的 overloadConfig()）：

  ┌──────┬─────────────────────────────────────────────────────────────────────┬────────────────────────────────────┐
  │  桶  │                                默认                                 │                说明                │
  ├──────┼─────────────────────────────────────────────────────────────────────┼────────────────────────────────────┤
  │ 全局 │ AUTH_RATE_PER_SEC=20，突发 AUTH_RATE_BURST=40                       │ 2 核 + cost=10 的容量约 28         │
  │      │                                                                     │ 次哈希/s，取 70% 留余量            │
  ├──────┼─────────────────────────────────────────────────────────────────────┼────────────────────────────────────┤
  │ 每   │ AUTH_IP_PER_SEC=30，突发 = 它的 2 倍 = 60（代码里 ipBurst =         │ 反滥用                             │
  │ IP   │ ipPerSec * 2，不是结构体里那个 120）                                │                                    │
  └──────┴─────────────────────────────────────────────────────────────────────┴────────────────────────────────────┘

  真正卡住你的是全局桶 20/s。你的 --friends 100 一轮会打 101 次注册 + 101 次登录 = 202
  次认证，几乎同时到达。令牌桶开局只有 40 个令牌、每秒补 20：

  排空 202 个请求需要 ≈ (202 - 40) / 20 ≈ 8.1 秒

  而压测客户端的重试预算是 4 次、每次等约 1 秒（test_stress_03.cpp:186 int attempts = 4）——只覆盖约 3 秒，撑不到 8
  秒，于是超过预算的请求就抛出你看到的那句 HTTP 429 {"code":1,"message":"too many auth requests"}。

  所以这是设计意图的实现：原来没有这道闸门时，同样这 202 次请求的表现是"登录 p50 = 13.3
  秒、并且把发消息查好友一起拖到秒级"。现在变成"先放 40 个，之后按 20/s 放行，其余明确告诉客户端 1 秒后再来"。

  怎么改，取决于你要测什么

  A. 想保留保护、让 100 并发压测跑完（推荐） — 不动服务端，加大客户端重试预算。8 秒排空需要约 10 次退避：

  # step2_test_client/test_stress_03.cpp:186
                         int attempts = 4) {     →     int attempts = 12) {

  这样测到的是"过载时客户端的真实体验"（延迟里包含退避等待，这是对的，readme §11.7 第 1 条写了）。

  B. 想要能和 11-17 那批数据对照的"无闸门"基线 — 把限流抬到远高于机器真实容量，让它不参与这轮测量：

  AUTH_RATE_PER_SEC=500 AUTH_RATE_BURST=500 AUTH_IP_PER_SEC=500 ./build/bin/Release/cxim-server

  这时你会重新看到纯服务端曲线（cost=10 下 N=100 的 login 排空约 4
  秒）。但这组数据必须在报告里注明"关闭过载保护下取的"，否则以后没法解释为什么 429 消失了。

  C. 想真正抬高容量（而不是绕开）：
  1. BCRYPT_COST=8 — 单次哈希再降 4 倍，容量约 115/s，可把 AUTH_RATE_PER_SEC 提到
     60~80。代价是离线爆破强度继续下降，需要安全侧点头。
  2. 升 4 核 — 容量与核数成正比。
  3. 重连走
     /api/token/refresh（P0-3）。注册不可避免要哈希，但登录/重连不该哈希；现在压测脚本每个用户都做密码登录，所以是 202
     次哈希。真实客户端接上 refresh 后，稳态登录风暴的哈希次数接近
     0，这条限流也就不会被触发。这是唯一"既不改安全性也不加硬件"的解。

  在哪里改、怎么确认生效

  - 本地临时验证：export 后启动，如上。
  - 部署环境：这些值被写进了 CXIM.service 的 Environment= 行，源头是 build.sh 顶部的 ${VAR:-默认}，所以改 build.sh
    里的默认值 → 重新 bash build.sh 打包 → 重装 .deb。不想重打包就 systemctl edit CXIM 加 override，然后 systemctl
    restart CXIM。
  - 一个诚实的缺口：[db] pool max=... bcryptCost=... 这行启动日志能验证 DB 侧参数，但HTTP
    限流参数目前不在启动日志里自述，所以现在只能靠行为反推。要我加一行 [http] auth rate=20/s burst=40, ip=30/s,
    inflight=512, body=64KB 吗？这正好补上 readme §11.6 第 3 条检查项的盲区。

  一个提醒：AUTH_IP_PER_SEC 的语义是"按 socket 对端地址计数"。你现在从一台机器打 101 个用户，所以每 IP
  桶其实和全局桶几乎等价；将来若挂到 nginx/CLB 后面，所有用户会共享一个来源 IP，届时必须改成"只信任可信代理的
  X-Forwarded-For"，否则客户端自己塞一个 XFF 就能绕过每 IP 限制——这点我在 http_server.cpp 的注释和 readme §2.2
  里标过，不能顺手加。

./build/bin/Release/test_stress_03.exe --host 110.40.206.253 --http-port 10002 --tcp-port 10001 --friends 100 --timeout 240
[stress03] host=110.40.206.253 http=10002 tcp=10001 friends=100 runId=978103664 hb=30s timeout=240s
[stress03] main userId=5340, starting 100 friend workers...
[stress03] all friends registered + TCP logged in; creating requests...
[stress03] friend requests created; waiting accepts...
[stress03] friends accepted; exchanging messages...
[stress03] replies acked=100/100, send retries=0
[stress03] messages exchanged; querying friend lists...
[stress03] lists queried; deleting friends...
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit:
recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit:
recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]




[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]


[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]



[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2][stress03] recv exit:
[stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2][stress03] recv exit:
recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]recv header: End of file [asio.misc:2]


[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] fatal: delete timeout: 0/100
[stress03] friend 0 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 1 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 2 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 3 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 4 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 6 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 7 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 8 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 9 error: bad http response: no header end
[stress03] friend 10 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 11 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 12 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 16 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 17 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 18 error: connect: ����Ŀ�����������ܾ����޷����ӡ� [system:10061 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 19 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 20 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 21 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 22 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 23 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 24 error: bad http response: no header end
[stress03] friend 25 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 26 error: connect: ����Ŀ�����������ܾ����޷����ӡ� [system:10061 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 27 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 28 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 29 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 30 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 31 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 32 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 33 error: bad http response: no header end
[stress03] friend 34 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 35 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 36 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 37 error: connect: ����Ŀ�����������ܾ����޷����ӡ� [system:10061 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 38 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 39 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 40 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 41 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 42 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 43 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 44 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 45 error: connect: ����Ŀ�����������ܾ����޷����ӡ� [system:10061 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 46 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 47 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 48 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 49 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 50 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 52 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 53 error: bad http response: no header end
[stress03] friend 54 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 55 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 56 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 57 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 58 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 59 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 60 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 61 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 62 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 63 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 64 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 65 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 66 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 67 error: bad http response: no header end
[stress03] friend 68 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 69 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 70 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 71 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 72 error: connect: �������ӷ���һ��ʱ���û����ȷ�𸴻����ӵ�����û�з�Ӧ�����ӳ���ʧ�ܡ� [system:10060 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 73 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 74 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 75 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 76 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 77 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 78 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 79 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 80 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 81 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 82 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 83 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 84 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 85 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 86 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 87 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 88 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 90 error: connect: �������ӷ���һ��ʱ���û����ȷ�𸴻����ӵ�����û�з�Ӧ�����ӳ���ʧ�ܡ� [system:10060 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 91 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 92 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 94 error: connect: �������ӷ���һ��ʱ���û����ȷ�𸴻����ӵ�����û�з�Ӧ�����ӳ���ʧ�ܡ� [system:10060 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 95 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 96 error: bad http response: no header end
[stress03] friend 97 error: bad http response: no header end
[stress03] friend 98 error: bad http response: no header end
[stress03] friend 99 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]

===== Stress03 Aborted =====
friends ready   : 100/100
accepted        : 100/100
messages        : 100/100
listed          : 100/100
deleted         : 7/100
failed          : 94
============================


---

# test_stress_03 阶段4（删好友）全员掉线分析与解决方案（追加）

> 现象来源：本文件第 706~887 行，`./build/bin/Release/test_stress_03.exe --friends 100 --timeout 240`
> 环境：`110.40.206.253`（2 核 / 2GB / 3Mbps，mariadbd 同机），`OPEN_STRESS_TEST_DEV_MODE=True`
> 结论先行：**这一轮已经不是 429 问题了，也不是"慢"问题——是服务端进程在阶段4 消失，而且 systemd 没有把它拉起来。**

## 一、结论

1. **阶段 1~3 全部通过**：`friends ready 100/100`、`accepted 100/100`、`messages 100/100`、`listed 100/100`。
   上一轮那个"注册/登录 13 秒 + 429 挡住"的问题在这份数据里已经不存在了。这是有效的进展，先记下来。
2. **失败点在阶段4 的第一秒**：`deleted 7/100`、`failed 94`，且 94 个错误里绝大多数是 socket 层错误，不是业务错误。
3. **三种错误码同时出现**（这是定性关键）：
   - `10054`（WSAECONNRESET）：已建立的连接被对端强杀 → **服务端进程没了**（内核替它发 RST）；
   - `10061`（WSAECONNREFUSED）：新连接被拒 → **端口上已无人监听**；
   - `bad http response: no header end`：HTTP 读到 0 字节 → 请求被"连响应都没写"就断了。
   再加上 `recv exit: recv header: End of file` 的**批量同时出现**（几十条挤在同一时刻），
   这是"一次性死掉全部连接"的形状，**不是**限流（429 有正常响应头），**也不是**慢（慢只会超时，不会 refused）。
4. 因此排查方向应该是"**服务端为什么退出 + 退出后为什么没被重启**"，而不是继续调优性能参数。

## 二、证据不该这样读（避免下一步走偏）

| 错误 | 容易被误读成 | 实际含义 |
| --- | --- | --- |
| `10054 远程主机强迫关闭…` | "TCP 协议栈/网络问题" | 对端进程异常终止或服务端主动带数据关闭；配合下面两条才有意义 |
| `10061 拒绝连接` | "限流把连接挡了" | 服务端限流走的是"握手成功后立刻 FIN"（`server.cpp:maxSessions`），**不会**产生 refused；refused 只能是没有监听者 |
| `bad http response: no header end` | "HTTP 解析 bug" | 客户端读到 0 字节。keep-alive 改造后服务端只在写完响应后才决定关闭；读到空 = 对端在写之前就死了 |
| `10060 连接超时` | "带宽打满" | 可能伴随出现，但在 refused 已经出现的时间段里，它更可能是 SYN 无人应答/连接队列异常 |

补充一条自我修正：**`OPEN_STRESS_TEST_DEV_MODE=True` 只关掉了认证限流**（`AUTH_RATE_*` 全 10000），
但 `HTTP_MAX_INFLIGHT=512`、SQL 队列深度准入、`TCP_MAX_SESSIONS=2000`、`DB_QUEUE_LIMIT=2000` 都还在。
所以这一轮的数据是"闸门半开"状态——就算跑通，也不能和 §11 里任何一组预测直接对照（见第七节）。

## 三、根因链（代码可支撑的部分）

### 链路 A：io 线程被打死 → main 正常返回 → 退出码 0 → systemd 不重启

`main.cpp` 的结构是：

```cpp
ioThreads.emplace_back([&io, i] {
    try { io.run(); }
    catch (const std::exception& e) { std::cerr << "[io] thread " << i << " exception: " << e.what() << std::endl; }
});
for (auto& t : ioThreads) { if (t.joinable()) t.join(); }   // ← 全部线程回来后，main 继续往下走
```

Boost.Asio 的语义是：**completion handler 抛出的异常会从 `io_context::run()` 里逃逸出来**。
也就是说，任何一个 handler（accept / 读 / 写 / DB 回调）抛异常，代价是**杀掉一个 io 线程**，
而不是只断开那条连接。

于是：

1. 2 核机器上 `io threads=2`（readme §11.3：现在按核数给，不再是"至少 4"）；
2. 阶段4 是整轮里 handler 最密集、内存最紧的时刻（100 个 DELETE 事务 + 每个删除向两端各推一条
   `FriendListChanged` = 200 次投递 + 100 次列表查询的响应仍在飞）；
3. 两个 io 线程先后被异常打断 → `join()` 全部返回 → **`main()` 正常走到 `return 0`**；
4. 进程以**退出码 0** 结束 → 所有 fd 被内核关闭 → 已建立连接收到 RST（=10054/EOF），
   新连接收到 refused（=10061），在途 HTTP 收到 0 字节（=no header end）——**与观测完全一致**。

结构性问题就在这里：

- `CXIM.service` 用的是 `Restart=on-failure`（`build.sh` 第 104 行），
  **而 systemd 的 `on-failure` 不包含"退出码 0"** → 进程死后服务永久 inactive，除非人工拉起。
  这解释了为什么后半段错误从 reset 变成 refused（先死、后无人监听）。
- 主程序在"事件循环全灭"这种明显故障下**没有理由报告成功**。

### 链路 B：2GB 机器上的内存预算本来就是超卖的

这是我最怀疑触发链路 A 里那个异常的东西。当前参数隐含的最坏情况内存：

```
每连接写队列上限  kMaxWriteBytes = 4MB      （session.h:122，我自己加的）
并发连接上限      TCP_MAX_SESSIONS = 2000    （build.sh:56）
→ 理论下限 8GB 的"合法堆积"，而机器只有 2GB，还要和 innodb_buffer_pool_size=384M 的 mariadbd 分
```

单连接 4MB 这个值本身不算错（对端读得太慢时值得缓冲），但**它没有全局预算**：
500 条慢连接各堆 1MB 就是 500MB，压测里"客户端阻塞在 HTTP DELETE、TCP 侧仍在被推事件"正是这种形态。
内存耗尽时 `new`/`std::string` 抛 `bad_alloc` → 走的就是链路 A（或者干脆被 OOM killer 直接 SIGKILL，
客户端表象一模一样）。

**链路 A 与 B 不互斥，且都需要 journal 证据来定二选一**，见第四节。

## 四、先花 10 分钟定成因（不改任何代码）

在服务端上依次执行：

```bash
# 1) 服务现在的状态与退出历史
systemctl status CXIM --no-pager | head -12
systemctl show CXIM -p NRestarts -p ExecMainStatus -p ExecMainCode -p ActiveState -p InactiveExitTimestamp

# 2) 掉线时刻附近到底发生了什么（关键）
journalctl -u CXIM --since "-30 min" --no-pager | tail -60

# 3) 是否被内核 OOM 杀
sudo dmesg -T | grep -iE "out of memory|oom-kill|killed process" | tail -20
journalctl -k --since "-30 min" | grep -iE "oom|kill" | tail -20

# 4) 有没有 core 可看
coredumpctl list cxim-server 2>/dev/null | tail -5

# 5) 复跑时盯内存与线程（另开终端）
watch -n1 'free -m; ps -o pid,rss,nlwp,etime,cmd -C cxim-server'
```

判读表（**这一步做完才有资格改配置**，否则是在猜）：

| 看到 | 结论 | 处置 |
| --- | --- | --- |
| `ExecMainStatus=0`、`NRestarts=0`、ActiveState=inactive | 链路 A：干净退出且没重启 | 第六节 1、2 项 |
| `dmesg` 有 `Out of memory: Killed process … cxim-server` | 链路 B：OOM | 第六节 3 项（内存配平）优先 |
| `ExecMainStatus=134/139`（SIGABRT/SIGSEGV） | 崩溃，需要 core | `coredumpctl dump` + `bt` |
| 日志里出现 `[io] thread N exception: …` | 链路 A 的确切抛出点 | 把该行内容当第一手证据留着 |

⚠️ **必须配合 `QUIET_LOG=0` 复跑一次**：`build.sh:61` 默认 `QUIET_LOG=1`，
而 `quiet_log.h` 的实现是把 **`std::cout` 和 `std::cerr` 一起丢掉**。
也就是说链路 A 里唯一能定罪的 `[io] thread N exception: <原因>` 恰好被我们自己吞了。
这是当前可观测性的真实缺口（第七节第 3 项修它）。

```bash
# 复现取证用这一套（只改日志，不改性能参数）
sudo systemctl stop CXIM
cd /opt/CXIM && QUIET_LOG=0 HTTP_PORT=10002 TCP_PORT=10001 ./bin/cxim-server 2>&1 | tee /tmp/cxim_foreground.log
#   另开窗口跑 test_stress_03 --friends 100，看 /tmp/cxim_foreground.log 里有没有 exception
```

## 五、还有一个必须点名的锁风暴（阶段4 为什么偏偏是它）

`DELETE /api/friends/{mainUid}` 的实现是一条 `OR` 型删除（`db.cpp:deleteFriend`）：

```sql
DELETE FROM friends WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)
DELETE FROM friend_requests WHERE (requester_id=? AND target_id=?) OR (requester_id=? AND target_id=?)
```

阶段4 是 **100 个不同用户同时删同一个 mainUid**。两条语句都要命中"mainUid 那一侧"的行，
100 个事务在同一批索引区间上排队，`runTransactionWithRetry` 遇到 1213/1205 会在 **worker 线程里 sleep 退避**，
于是 `DB_WORKER_COUNT=4` 的队列瞬间积压——**这才是阶段4 的真实压力形状**（也是它比阶段1~3 更容易把内存/线程推到极限的原因）。

需要澄清一个容易做错的结论：**这里不能用 P1-7 的批量接口解决**。
`/api/friends/batch-delete` 是"同一个用户删多个好友"，而压测阶段4 是"多个用户各删同一个好友"，方向相反，
批量接口帮不上。可行的缓解只有三条：

1. 缩小锁粒度：把 `OR` 拆成两条**单行主键** DELETE，并按固定顺序（`min(uid,fuid)` 先）执行，
   让并发事务的加锁顺序一致，从源头减少 1213；
2. 降低 `innodb_lock_wait_timeout`（默认 50s）到 5~10s，让"等不到锁"快速失败并交给重试，
   而不是让 worker 线程被一条 SQL 卡住 50 秒；
3. 压测侧把阶段4 的并发从 100 降到 20~30 做梯度，确认它是"并发度触发"而不是"逻辑错误触发"。

## 六、解决方案（配置层，马上能生效）

按优先级：

**1. `Restart=always` + 让主程序在故障时以非 0 退出**（治"死了没人拉"）

```ini
# build.sh 生成 service 的部分
Restart=always          # 原来的 on-failure 不覆盖退出码 0 的"干净退出"
RestartSec=3
```

只做这一条，故障还会发生，但不再演变成 240 秒的 refused 风暴，且能自动恢复。

**2. 给阶段4 这类"等锁"场景留出正确的容量关系**

```bash
DB_WORKER_COUNT=6            # 4 太容易被 100 个等锁事务占满；6 给 SQL 更多并发（仍 ≤ 核数*3）
DB_POOL_SIZE=6               # 与 worker 一致（P0-7 的约定）
innodb_lock_wait_timeout=8   # mariadbd 侧，单位秒；默认 50 太久
```

**3. 把内存预算配平到 2GB（治"超卖"）**

```bash
TCP_MAX_SESSIONS=500         # 压测最大 500 用户，2000 是纯粹的内存风险敞口
HTTP_MAX_INFLIGHT=128        # 512 个在途响应 × 每个可能捕获 content，2GB 上偏大
# 代码侧配套：kMaxWriteBytes 4MB/连接 → 512KB/连接 + 进程级总预算（见第七节 2）
```

**4. dev mode 要么全开、要么别开（治"半开门数据"）**

`OPEN_STRESS_TEST_DEV_MODE=True` 应当是一组**一致**的开关，而不是只动 `AUTH_RATE_*`：

```bash
if [[ "${OPEN_STRESS_TEST_DEV_MODE}" = "True" ]]; then
    AUTH_RATE_BURST=10000; AUTH_RATE_PER_SEC=10000; AUTH_IP_PER_SEC=10000
    HTTP_MAX_INFLIGHT=20000        # 否则准入仍然会在这里把请求挡掉/关连接
    TCP_MAX_SESSIONS=20000         # 注意：这是"测性能上限"模式，不是生产配置！
    DB_QUEUE_LIMIT=200000          # 让过载保护彻底不参与其中
    QUIET_LOG=0                    # 取证模式必须留日志（见第四节警告）
fi
# 生产档必须是另一组保守值；两套档不要混用同一份 .deb
```

## 七、解决方案（代码层，三处小改动，按此顺序做）

1. **`main.cpp`：事件循环全灭必须算失败**
   现在两个 io 线程都退出后 `main()` 会 `return 0`。应该：统计"因异常退出的 io 线程数"，
   当**全部** io 线程都已返回时，打印致命日志并以非 0 退出（`_exit(70)` 这类），
   让 `Restart=on-failure/always` 能正确识别并重启。同时在单个 io 线程死亡时也记一条日志——
   "还剩几个在跑"是判断级联故障的关键指标。

2. **`session.{h,cpp}`：写队列加全局预算**
   `kMaxWriteBytes` 从 4MB/连接降到 512KB/连接，再加一个进程级
   `std::atomic<size_t> g_writeQueueBytes` 总量上限（建议 64MB）。
   超过总量时按"最老的连接优先断开"或"新写入直接断开该连接"处理。
   这一条把"最坏合法内存"从 8GB 拉到与 2GB 相称的量级，是我上一轮引入的设计错误，应当由我修。

3. **致命日志不受 `QUIET_LOG` 摆布**
   `quiet_log.h` 现在把 `cout` 和 `cerr` 一起丢到 `NullStreambuf`。
   应改成：`QUIET_LOG=1` 只静音常规业务流水（`std::cout`），
   **异常/致命路径单独走一条不被重定向的输出**（直接写 `syslog`/journald，或保留 `std::cerr`）。
   否则最需要的证据（`[io] thread N exception: …`、`[server] fatal: …`）在生产环境里永远看不到。

> 另外仍然建议补上第四节里提到的可观测缺口：HTTP 侧启动时自述一行
> `[http] auth rate=…/s burst=…, ip=…/s, inflight=…, body=…`，
> 与已有的 `[db] pool max=… bcryptCost=…` 对齐（readme §11.6 第 3 条检查项目前只覆盖 DB 侧）。

## 八、压测侧：把"可用性"和"性能"分开测

现在的输出混在了一起：阶段4 一崩，整份报告连延迟分位数都没有（脚本走的是 fatal 分支）。建议：

1. **加健康探针**：压测期间每 2~5 秒探一次 `tcp:10001` + 一次廉价 HTTP（例如带 token 的 `GET /api/friends`）。
   一旦发现"端口从可连变成 refused"，立刻在报告里打出**掉线时刻**（`server down at t=XX.Xs`）并终止本轮。
   性能报告和可用性事故必须在第一行就区分开，否则每次都要靠人肉读 94 行错误码。
2. **梯度二分**：`--friends 20 → 50 → 100 → 200`，每档都跑并记录 `free -m` 与服务端 RSS。
   哪一档开始 refused，就说明那档是资源拐点；比一次跑到 500 然后看 94 个 socket 错误有用得多。
3. **失败不中止统计**：脚本目前任一 friend 抛异常就 `g_stop`，导致"已经完成的 7 个删除"和"94 个 socket 错误"
   混成一个 `failed` 计数。性能压测应当统计错误率并照常输出延迟分位数（`--fail-fast` 作为可选项保留）。

## 九、验收口径（什么叫做完）

按顺序卡，不要跳：

1. `journalctl -u CXIM` 里**没有** `[io] thread … exception`、`NRestarts` 不增长（或增长后能自动恢复）；
   `dmesg` 里**没有** `cxim-server` 被 OOM kill。
2. `--friends 100` 跑完：**`deleted 100/100`、`failed 0`**，输出里不出现 `10054` / `10061` /
   `bad http response: no header end`。
3. 上一条达成后，才有资格读延迟分位数并按 readme §11.7 的"斜率"验收
   （目标：`login` 斜率 < 5ms/用户；`create request`/`list friends` p50 与登录风暴解耦）。
4. 最后再谈 N=200/500 的容量：那之前，阶段4 的锁风暴（第五节）与内存预算（第六节第 3 项）必须已经解决，
   否则测到的仍然是"崩溃边界"而不是"性能边界"。

## 十、一句话总结

**上一轮的 429 是"保护生效"，这一轮的 reset/refused 是"进程没了而且没人拉"**：
`Restart=on-failure` 不覆盖退出码 0 的干净退出，而 io 线程被 handler 异常打断后 `main()` 正好会以 0 退出；
最可能的诱因是我上一轮设的内存预算本身超卖（每连接 4MB 写队列 × 2000 连接 = 8GB，机器只有 2GB），
叠加阶段4 那种"100 个事务在同一批索引区间上排队等锁"的压力形状。
先用第四节的 journal/dmesg 把"链路 A / 链路 B"定死，再按第六、七节逐项修，最后用第九节的口径验收——
**在此之前，这份压测数据里没有一条性能结论可用。**

Everything is done. Let me write the final report:

  Code changes made (方案第七节 + 第五/六节里属于代码的部分):
  1. quiet_log.h: logFatal — 永不被 QUIET_LOG 静音，直写 fd 2，内置每秒 20 条限频 + suppressed
     计数。常规流水仍静音（不破坏"防 journald 阻塞"这条纪律）。
  2. main.cpp: io 线程存活/故障计数；每个线程退出都 logFatal 并带上剩余存活数；正常返回 run() 也算事件；只要发生过异常 →
     exit 70（让重启策略识别）。
  3. session.h/.cpp: 写队列两级预算 — 每连接 4MB→512KB + 进程级 64MB 全局计数；do_write 出队归还；do_close
     一次性归还残留字节（否则预算随连接 churn 单向漂移）；越线走 logFatal。
  4. db.cpp: deleteFriend 的 OR 型删除拆成两条按 (user_id,friend_id) / (requester_id,target_id) 升序的单行主键
     DELETE（固定加锁顺序，从源头减 1213，语义不变）；5 处诊断改 logFatal（去掉自己那套 %100 限频，避免双重限频）。
  5. server.cpp / http_server.cpp: 启动自述 — [server] ... max_sessions=、[http] ...
     inflight_cap/body_max/auth_rate/per_ip，补上 §11.6 检查清单只覆盖 DB 侧的盲区；拒绝新连接改 logFatal。
  6. build.sh: DEV 档成套（原来只放开 AUTH_*，现在把
     HTTP_MAX_INFLIGHT/DB_QUEUE_LIMIT/TCP_MAX_SESSIONS/TCP_MAX_OUTSTANDING 一起抬，并 QUIET_LOG=0）；支持
     OPEN_STRESS_TEST_DEV_MODE=False 覆盖；修掉 QUIET_LOG=1 无条件赋值会吃掉 DEV 档设置的 bug。Restart=always
     你已经改了，我没动。
  7. prepare_env_V0.sh: innodb_lock_wait_timeout = 10（默认 50s 会把 4 个 worker 全卡在等锁上）。

  Verification: build exit=0 / 0 errors; three acceptance suites all pass on local ports (tcp_http, friend, group) —
  其中 friend 覆盖我重写的 deleteFriend，group 覆盖写队列/补推/回执路径。no stray processes.

  What to watch on their next run:
  - 现在日志里会出现 [fatal] ... 前缀的行，即使 QUIET_LOG=1 也在。具体三类：[io] thread N DIED on handler exception:
    <原因> / [db] worker exception (DAO 未自处理) / [write] queue over budget。
  - 判定规则：看到 [io] thread 0 DIED ... alive=1 → 事件循环正在级联失败，异常原因就在同一行；看到 NRestarts 增长 →
    说明现在能自动恢复了（以前是躺着不动）。
  - 如果这轮 deleted 仍然不满 100 但没有 10054/10061 → 那就是性能问题不是可用性崩溃，可以继续读延迟。
  - 一个诚实的未做项：全局写预算的"超总量时优先踢谁"我实现成"踢这次要写入的那条连接"，没有实现"按最老连接优先"那档（后者
    需要全局 LRU，收益不明显）；文档里写的是两种可选。

  Also flag: kGlobalWriteBudgetBytes=64MB + 512KB/conn are first-cut numbers to validate on their box; if [write] queue
  over budget appears in a normal run, it's too tight, not a functional bug.

  Also note: the report file test_stress_03_20261117.txt shows as modified — that's their own appended run; I didn't
  touch it. Worth a brief mention so they know I noticed and didn't do it.

  Keep it organized and reasonably brief.

                                                                                                          01:35 PM auto
● 代码已按方案改完，编译全绿，三套验收全过。已停在这里，没有部署、没有再动任何东西。

  改了什么

  1. quiet_log.h — 新增 logFatal 致命通道（核心）
  常规流水照旧被 QUIET_LOG=1 静音（这条纪律不能破：每条消息都带 std::endl 刷 journald，在 2 核机上会直接挤占 epoll
  时间片，压测数据被日志本身污染）。但致命事件走一条独立通道：直写 stderr 的 fd 2，绕开被替换掉的 std::cerr
  rdbuf，因此永不被静音；内置每秒 20 条限频，超出的只累计、下次输出时带 (+N suppressed)。用一把 mutex
  而不是无锁窗口——致命日志按定义低频，而"CAS 赢了才重置计数器"这种细节写错的后果是限流失效。

  2. main.cpp — 事件循环全灭不再"成功退出"
  io.run() 返回（无论正常还是异常）都算事件：logFatal 记下是哪条线程、原因、以及还剩几条在跑。只要发生过异常，进程以 70
  退出，让重启策略能识别。这修掉了那条最阴的路径：handler 抛异常 → 杀掉 io 线程 → 全部返回 → main() 走到 return 0 →
  Restart=on-failure 不管退出码 0 → 服务躺着，客户端看到的就是 reset→refused。

  3. session.h/.cpp — 写队列两级预算
  每连接 4MB → 512KB，另加进程级 64MB 全局计数。并且 do_close()
  会一次性归还该连接残留的字节——这一步不加的话，全局预算会随连接 churn
  单向漂移，最后把好端端的连接也踢掉，正是我要修的"记账泄漏"。归还分工是明确的：写成功由写回调按帧归还，其余一切路径由
  do_close 清空；出错分支不 pop 不扣减，所以两条路不会重复扣。

  4. db.cpp — 阶段4 的锁风暴从源头治
  deleteFriend 的 WHERE (a=? AND b=?) OR (c=? AND d=?) 拆成两条主键单行 DELETE，按 (user_id,friend_id) /
  (requester_id,target_id) 升序执行。全局同一顺序 → 循环等待不成立，重试退化成真偶发，语义完全不变（userId==friendId
  时第二条自然 0 行，幂等）。另外 5 处诊断改走 logFatal，并删掉了我原来手写的 %100
  限频——两套限频同时存在时，真正该看到的那几条反而会被自己压掉。

  5. server.cpp / http_server.cpp — 启动自述补齐
  [server] ... max_sessions=、[http] ... inflight_cap= / body_max= / auth_rate= / per_ip=。之前 §11.6 检查清单第 3
  条只能验证 DB 侧参数，HTTP 侧的闸门是黑盒。

  6. build.sh — DEV 档成套
  原来只放开 AUTH_RATE_*，而 HTTP_MAX_INFLIGHT、SQL 队列深度准入、DB_QUEUE_LIMIT、TCP_MAX_SESSIONS
  都还开着——测到的是"混合闸门"数据，没法和任何预测对照。现在 DEV 档一起抬，并置 QUIET_LOG=0（取证档必须留日志）；支持
  OPEN_STRESS_TEST_DEV_MODE=False bash build.sh 覆盖。顺带修了一个真 bug：下面那行 QUIET_LOG=1 是无条件赋值，会把 DEV
  档的 QUIET_LOG=0 吃掉，改成 ${QUIET_LOG:-1}。Restart=always 你已经自己改了（build.sh:130），我没碰。

  7. prepare_env_V0.sh：innodb_lock_wait_timeout = 10。默认 50s 意味着一条等锁 SQL 能把 4 个 worker 里的一个钉死 50
  秒；重试兜底的前提是"等锁要快"。

  验证结果

  ./build.sh                                   exit=0，0 error
  verify_tcp_http.sh   →  [verify] ALL PASS
  verify_friend.sh     →  [verify-friend] ALL PASS   （覆盖我重写的 deleteFriend/block）
  verify_group.sh      →  [verify-group] ALL PASS    （33 项，覆盖写队列/补推/回执路径）
  本地无残留 cxim-server 进程，无临时脚本残留。

  你压测时该看什么（这次日志不再是空的）

  [fatal] 前缀的行即使 QUIET_LOG=1 也会出现，重点三类：

  - [io] thread N DIED on handler exception: <原因>, alive=M →
    事件循环正在级联失败，原因就在同一行，这就是上一轮缺失的那条证据；
  - [db] worker exception (DAO 未自处理): … → 某个 DAO 没兜住自己的异常；
  - [write] queue over budget, kick user=… conn=…B/cap=… global=…B/cap=… → 慢读端把预算压满了。此时看是 conn
    越线（单条连接堆太深）还是 global 越线（很多条一起堆），两者处置不同。

  判定口径：这轮如果 deleted 仍不满 100 但不再出现 10054/10061/no header
  end，那问题就回到性能而不是可用性崩溃，可以开始读延迟分位数；配合 systemctl show CXIM -p NRestarts
  看是否发生了重启（以前是死了也不计 restart）。

  两个需要你用真实压测校准的数值（不是我测出来的，是按 2GB 算的初值）：512KB/连接 和 64MB/进程。如果正常功能压测就开始刷
  [write] queue over budget，说明太紧，放宽即可——那不是功能 bug。

  另外提一句：stress_report/test_stress_03_20261117.txt 在 git
  里显示被修改（应该是你自己追加了新一轮输出），不是我改的，我没有动过 stress_report。


./build/bin/Release/test_stress_03.exe --host 110.40.206.253 --http-port 10002 --tcp-port 10001 --friends 100 --timeout 240
[stress03] host=110.40.206.253 http=10002 tcp=10001 friends=100 runId=980258212 hb=30s timeout=240s
[stress03] main userId=5449, starting 100 friend workers...
[stress03] all friends registered + TCP logged in; creating requests...
[stress03] friend requests created; waiting accepts...
[stress03] friends accepted; exchanging messages...
[stress03] replies acked=100/100, send retries=0
[stress03] messages exchanged; querying friend lists...
[stress03] lists queried; deleting friends...
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]

[stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2][stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]


[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
recv header: End of file [asio.misc:2]
[stress03] recv exit: [stress03] recv exit: recv header: End of file [asio.misc:2]
[stress03] recv exit: recv header: End of file [asio.misc:2]recv header: End of file [asio.misc:2]

[stress03] fatal: delete timeout: 0/100
[stress03] friend 1 error: bad http response: no header end
[stress03] friend 2 error: bad http response: no header end
[stress03] friend 3 error: connect: �������ӷ���һ��ʱ���û����ȷ�𸴻����ӵ�����û�з�Ӧ�����ӳ���ʧ�ܡ� [system:10060 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 4 error: bad http response: no header end
[stress03] friend 5 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 7 error: bad http response: no header end
[stress03] friend 8 error: bad http response: no header end
[stress03] friend 9 error: bad http response: no header end
[stress03] friend 10 error: bad http response: no header end
[stress03] friend 11 error: bad http response: no header end
[stress03] friend 12 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 13 error: connect: �������ӷ���һ��ʱ���û����ȷ�𸴻����ӵ�����û�з�Ӧ�����ӳ���ʧ�ܡ� [system:10060 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 14 error: bad http response: no header end
[stress03] friend 15 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 16 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 17 error: bad http response: no header end
[stress03] friend 18 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 19 error: bad http response: no header end
[stress03] friend 20 error: bad http response: no header end
[stress03] friend 21 error: bad http response: no header end
[stress03] friend 22 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 23 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 24 error: connect: �������ӷ���һ��ʱ���û����ȷ�𸴻����ӵ�����û�з�Ӧ�����ӳ���ʧ�ܡ� [system:10060 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 25 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 26 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 27 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 28 error: bad http response: no header end
[stress03] friend 29 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 30 error: connect: ����Ŀ�����������ܾ����޷����ӡ� [system:10061 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 31 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 32 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 33 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 34 error: bad http response: no header end
[stress03] friend 35 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 36 error: bad http response: no header end
[stress03] friend 37 error: bad http response: no header end
[stress03] friend 38 error: bad http response: no header end
[stress03] friend 39 error: bad http response: no header end
[stress03] friend 40 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 41 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 42 error: connect: ����Ŀ�����������ܾ����޷����ӡ� [system:10061 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 43 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 44 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 45 error: bad http response: no header end
[stress03] friend 46 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 47 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 49 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 50 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 51 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 52 error: bad http response: no header end
[stress03] friend 54 error: bad http response: no header end
[stress03] friend 55 error: bad http response: no header end
[stress03] friend 56 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 57 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 58 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 59 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 61 error: bad http response: no header end
[stress03] friend 62 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 64 error: bad http response: no header end
[stress03] friend 65 error: bad http response: no header end
[stress03] friend 68 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 69 error: bad http response: no header end
[stress03] friend 70 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 71 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 72 error: bad http response: no header end
[stress03] friend 73 error: bad http response: no header end
[stress03] friend 74 error: bad http response: no header end
[stress03] friend 75 error: connect: ����Ŀ�����������ܾ����޷����ӡ� [system:10061 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 76 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 77 error: bad http response: no header end
[stress03] friend 78 error: bad http response: no header end
[stress03] friend 79 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 80 error: bad http response: no header end
[stress03] friend 82 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 83 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 84 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 85 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 86 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 87 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 88 error: bad http response: no header end
[stress03] friend 89 error: bad http response: no header end
[stress03] friend 90 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 91 error: bad http response: no header end
[stress03] friend 92 error: bad http response: no header end
[stress03] friend 93 error: bad http response: no header end
[stress03] friend 94 error: bad http response: no header end
[stress03] friend 95 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 97 error: connect: �������ӷ���һ��ʱ���û����ȷ�𸴻����ӵ�����û�з�Ӧ�����ӳ���ʧ�ܡ� [system:10060 at D:\FKC\KC-CXIM\sdk\boost_1_92_0-bin-msvc-all-32-64\boost_1_92_0\boost/asio/detail/win_iocp_socket_service.hpp:630:5 in function 'class boost::system::error_code __cdecl boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::connect(struct boost::asio::detail::win_iocp_socket_service<class boost::asio::ip::tcp>::implementation_type &,const class boost::asio::ip::basic_endpoint<class boost::asio::ip::tcp> &,class boost::system::error_code &)']
[stress03] friend 98 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]
[stress03] friend 99 error: Զ������ǿ�ȹر���һ�����е����ӡ� [system:10054]

===== Stress03 Aborted =====
friends ready   : 100/100
accepted        : 100/100
messages        : 100/100
listed          : 100/100
deleted         : 10/100
failed          : 91
============================


---

# test_stress_03 阶段4 core-dump（status=11/SEGV）：定位方案 + 本轮已实施的修复（追加）

时间：2026-11-18。样本：`logs/CXIM.log`（systemd/journal，14:22:25~14:23:09）
+ 本文件 1254~1425 行的 `test_stress_03 --friends 100` 输出
+ `stress_report/test_stress_03_20261117.txt`（**同一支脚本、同一台机器，200 好友全绿**）。

## 一、这份日志证明了什么，又排除了什么

| 观测 | 能得出的结论 |
| --- | --- |
| `Main process exited, code=dumped, status=11/SEGV` + `Failed with result 'core-dump'.` | **真·段错误**。不是退出码问题，不是被信号杀，也不是超时 |
| 上一节（§三 链路A）猜的"io 线程异常 -> main 走到 return 0" | **被排除**。那种死法是 `code=exited, status=0`，而且不会打 core |
| 上一节猜的"链路B：内存超卖 OOM" | **被排除**。`4M memory peak`，且没有 `oom-kill`/`Killed process`；真 OOM 是 SIGKILL(9)，不会是 SEGV |
| 崩溃前**没有任何** `[fatal] ...` 行 | **没走**预算踢人分支（`[write] queue over budget`），也**没有** io 线程死亡。512KB/64MB 这一档本轮不是元凶 |
| 崩溃前**没有任何** `[logout]` 行 | 100 条 TCP 连接**一条都还没正常关闭**，进程就死了。也就是说：死在"删除刚开始成批返回"的那一刻，不是死在"连接集体关闭"之后 |
| `Scheduled restart job, restart counter is at 1` + 3 秒后重新监听 | §六.1 提的 `Restart=always` **已生效**，"死了没人拉"这一项闭环了 |
| 14:22:40~46 登录、14:22:50~59 路由、14:23:06 崩 | 前 3 个阶段全绿，死在阶段3->4（查列表 / 删好友）交界 |

再叠上最关键的一条对照：**11/17 用同一支脚本、同样打公网、`--friends 200` 是 `deleted 200/200、failed 0`**。
今天 100 好友必崩 => 这是**回归**，不是"压力还不够所以没暴露的老问题"。
回归窗口就是今天 13:2x 那一批改动（P0-4 两级写预算、P1-8 合并写、P1-3 登录一次性补推、HTTP 批量接口）。
对照 `git show HEAD:dev/IMS/KC-CXIM-Svc/basic_service_20261107/step1_tcp_echo/session.cpp`：
旧版 `do_write()` 是"一帧一次 async_write"，旧版 `do_close()` 里**根本没有** `write_queue_.clear()`。
**新旧代码在 `do_write/do_close` 这一对函数上的差异，正是本轮唯一新增的内存生命周期风险**（见三.1）。

## 二、先修"查不到"：现在既没有 core，也没有栈

这一步不是可选的。当前的部署**从设计上就无法回答"SEGV 在哪一行"**：

1. `build.sh` 生成的 unit 里写死了 `LimitCORE=0` -> 内核**不落盘 core**。
   于是 journal 报 `result 'core-dump'`（它只表示"死因是会落 core 的信号"），而 `coredumpctl list` 是空的。
2. `CMAKE_BUILD_TYPE=Release` 且没有任何 `-g` -> 即使有 core，栈里也只有裸地址。
3. 上一轮加的 `logFatal` 只能记"我们自己想记的事"，段错误是**没有机会记**的。

已按这三条补上（详见第四节清单）：`LimitCORE=infinity`、服务端加 `-g -fno-omit-frame-pointer`
（**不动优化级别**，压测数据仍可与 §11 对照）、以及 `main.cpp` 里的信号处理器 ——
后者不依赖 core：SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT 时先把
`[fatal] crash signal=... addr=... + 原生栈帧`直写 fd 2（journald，绕开 QUIET_LOG），
再恢复默认处理并重新触发同一个信号，所以 `status=11/SEGV` 的记账与 `Restart=always` 行为**完全不变**。

下一轮如果还崩，journal 里就会是这种形状（这才叫可定位）：

```
Nov 18 15:04:11 VM-0-11-ubuntu cxim-server[2714933]: [fatal] crash signal=SIGSEGV addr=0x0
Nov 18 15:04:11 VM-0-11-ubuntu cxim-server[2714933]: [fatal] backtrace:
/opt/CXIM/bin/cxim-server(+0x1f3a2c)[0x55e8a1c03a2c]
/lib/x86_64-linux-gnu/libc.so.6(+0x9eb75)[0x7f3c1c2beb75]
# 有 -g 时形如： #0 0x55e8... in kc::session::do_write() at .../session.cpp:842
```

## 三、代码里能证实的内存安全缺陷（按"是不是这次 SEGV"排序）

前两条都能独立解释"只在压测阶段4 出现的一次性 SEGV"。它们本身是**代码可证实的缺陷**；
"哪一条真正打了这次"要靠第二节的现场来定 —— 这一点我不替代码作保。

### 1.【最可能】TCP 写路径：`do_close()` 清了正在被 `async_write` 引用的内存，且写回调可能在空 deque 上 pop

位置：`session.cpp:852`（do_write）、`session.cpp:920-946`（do_close）、`session.h:143`

- P1-8 把 <=32 帧 / <=64KB 一次性交给 `async_write`，交出去的是**指向 `write_queue_` 里各个 `std::string` 的
  `const_buffer`**。当时的注释断言"deque 尾追加不会让已有元素引用失效，所以安全"—— 前半句对，**结论错**：
  同一版还给 `do_close()` 加了 `write_queue_.clear()`（不还这一步，进程级预算会单向漂死）。
- 于是任何"写入未完成就关闭连接"（空闲踢人 / 对端断开 / 预算越线 / 重复登录踢旧连接）都会把
  **op 对象仍在引用的那批 string 释放掉** —— 教科书级 use-after-free。
- 更致命的是回调顺序：内核在 T0 写完、asio 在 T1 才把"写成功"回调送进 strand，
  T0~T1 之间排进来的任何 `close()` 都会先执行。结果就是
  **`ec == success` 的写回调在一个已被 `clear()` 的 deque 上跑 `front()/pop_front()`**：
  libstdc++ 的 `deque::clear()` 之后 map 槽为空指针，`front()` 即解引用 `nullptr`
  -> **`status=11/SEGV`**。而 `do_close()` 与 `send_message()` 里的预算踢人是**同 strand 内联**执行的，
  这条交错在压测里是常规时序，不需要什么极端运气。
- 连带的两处放大器（同一批代码，全部已修）：
  - `do_close()` 扣了一次 `write_queue_bytes_`，写回调又按 `frames` 再扣一遍 ->
    `std::atomic<std::size_t>` 下溢到 ~2^64 -> 之后**每条连接发第一帧就被判"超预算"踢掉**，
    把一次崩溃升级成全屏蔽踢人风暴（而踢人又制造更多"写入中途关闭"，自我放大）；
  - `send_message()` 不查 `closed_`，关闭后仍可入队 -> `idle=true` 再开**第二个** `async_write`
    （asio 明确禁止同一条流同方向两个在途写），两个回调都会去 pop 同一批帧。
- 为什么"只有压测才崩"：多帧成批（P1-8）**只**在成批时才存在"第二个回调排在 close 之后"的窗口，
  单帧路径永远碰不到。阶段4 恰好是本机最陡的一次成批推送：100 次删除 x 每次向两端各推一条
  `FriendListChanged`，主用户那条连接瞬间堆约 100 帧，同时其它连接正在关。

**修法**：本批字节拷进一个由该次写操作独占持有的连续缓冲（`shared_ptr<std::string>` 进回调）；
新增 `writing_` 标志保证"一次只有一个写在途"；写回调开头 `if (closed_) return;`（不重复扣账）；
pop 循环加 `&& !write_queue_.empty()` 上界；`send_message()` 开头 `if (closed_) return;`。
代价是每次一批 <=64KB 的 memcpy，换掉的是"写路径悬垂指针"这一整类故障。

### 2.【并列嫌疑】HTTP 连接没有 strand，而 `io_context` 由 2 个线程 run

位置：`http_server.cpp:656/680/713/722/1850/1881`（修复前这个类**一个 strand 都没有**；TCP 侧一直有）

- `Db::postToIo` = `boost::asio::post(io_, fn)`，回调落在**任意** io 线程；
  `http_session` 的读完成 / 写完成 / 60s keep-alive 计时器 / DB 回调里的 `send_json` 都直接挂在裸 io_context 上，
  于是同一个对象的两个 handler 可能在两个线程上**同时**碰 `socket_` / `buffer_` / `parser_` / `req_` / `idle_timer_`。
- 本轮压力刚好制造这个窗口：`deleteFriend` 走 `runTransactionWithRetry`，1213/1205 时**在 worker 线程里 sleep 退避**，
  100 个事务抢同一批行；`innodb_lock_wait_timeout` 默认 50s，而连接器 `socketTimeout=30000`
  -> 单个 DELETE 可以挂几十秒 -> **60s 计时器到点执行 `close_socket()` 时，这条连接的 DB 回调还在飞**。
  `close_socket()`（`socket_.shutdown` + `close`）与 `http::async_write`/`async_read` 重叠，
  踩的是 asio reactor 的 `descriptor_state`（引用计数非原子）—— 同样直接对应随机 SEGV。
- Beast 的前提是"同一条流、同一方向，同时只有一个异步操作"。违反它的表现不是"偶尔错响应"，就是随机 core dump。
- **修法**：给 `http_session` 一条自己的 strand（`run()` 入口 post 进去，读/写/计时器回调全部 `bind_executor`），
  并在 `send_json()` 入口判断 `strand_.running_in_this_thread()`，从 DB 回调进来时先切回本连接 strand。
  顺带 `req_` 也不会再被并发的 `req_ = parser_->release()` 改写。

### 3.【不是 SEGV，但会误导下一轮】`make_deleted_ok` 多写了一个 `EndObject()`

位置：`http_server.cpp:151-172`（两个 `StartObject`、三个 `EndObject`）。触发：`POST /api/friends/batch-delete` 成功返回。

- RapidJSON 的越界兜底是 `RAPIDJSON_ASSERT(level_stack_.GetSize() >= sizeof(Level))`，而 Release 带 `-DNDEBUG`：
  - Debug / RelWithDebInfo -> `level_stack_` 下溢 -> assert 失败 -> **SIGABRT，即 `status=134/ABORT`**
    （正是 §四判读表里"崩溃，需要 core"的另一种码。也就是说：**下一轮为了看栈改用 Debug 复跑时，
    很可能把这条误当成 SEGV 的根因** —— 先在这里写明：它已修，且它与本轮 SEGV 无关）；
  - Release -> 少弹一层，吐出 `{"code":0,...,"data":{...}}}` 这种多一个右花括号的非法 JSON，客户端解析直接失败。

### 4.【逻辑 bug，不崩】在线表注销不认身份

`do_close()` -> `manager_.remove(user_id_)` 无条件按 uid 删；重复登录时那一格**已经被新连接占用**
（`do_login` 是先 `add` 覆盖、再踢旧连接），旧连接随后注销就把新连接的登记删了：
新连接 socket 正常、事件循环在跑，但已不可路由，之后发给它的消息全部判成"对方离线"走落库 ——
客户端症状是"我在线却收不到消息，重连就好"。**修法**：`remove(uid, owner)` 比对表里是否仍是自己
（`session_manager.cpp:24-49`）。

### 5.【另一条"服务消失"路径】历史分页 `limit` 取自不可信输入且无上界

`do_history` / `do_history_group` 里 `limit = std::stoi(msg.content)` 只兜了转换失败，**没有上限**。
`{"type":7,"to":X,"content":"100000000"}` 会让 DB 拉回一亿行、再由 `serialize_history` 在内存里整段拼成 JSON
（写预算是在拼完之后再判的）-> `bad_alloc` 从 strand handler 逃逸 -> 杀一个 io 线程 ->
按 `main.cpp` 的新逻辑以 **70** 退出。它不是 SEGV，但同样表现为"服务没了"。已钳到 `[1,200]`（与 `kLoginOfflineBatch` 同档）。

### 6.【潜在空指针】`makeConn()` 不查 `driver` / `connect()` 返回值

Connector/C++ 1.1.9 的失败路径是"多数抛异常、少数返回空指针"混着来的。原代码直接
`driver->connect(props)` 构造 unique_ptr 后立刻 `conn->setAutoCommit(true)`。
调用点恰好是**连接池首次按需扩容**与**坏连接重建** —— 都是上量最陡的瞬间。已改成抛异常，走 `acquire()` 既有 catch。

### 7. 本轮明确排除的方向（免得下一轮重复查）

以下是**逐条读过并确认干净**的：

- **DB 回调捕获**：37 处 `postToIo([cb, ...])` 全部按值捕获，没有任何 `const T&` / 裸指针穿过 post 跳板；
  消费侧（`session.cpp` 各回调、`http_server.cpp` 各 handler）都是同步使用或再拷贝。这一整类 UAF 不存在。
- **语句/结果集生命周期**：所有 DAO 里 `ResultSet` 都声明在其 `Statement` **之后**（析构顺序 rs -> st 正确）；
  没有任何 job 把 `conn` / `pc` 捕获进跨线程 lambda；一个连接不会同时被两个线程看见。
- **关系缓存三张表**（`relMap_` / `relLru_` / `relLruIters_`）：增删改路径一致，TTL 过期用 `find()` 而不是
  `operator[]`，不存在"拿裸 list::iterator 去 erase"的构造；全程 `relMu_` 保护，`relEpoch_` 双重检查正确。
- **跨线程投递**：`push_event -> find()（lock 出 shared_ptr）-> deliver()（只 post 到目标 strand）` 是对的；
  `SessionManager` 全程 mutex + weak_ptr。
- **编解码边界**：`frame::encode/decode` 无越界；未信任长度在 `resize` 之前就被
  `len == 0 || len > MAX_BODY(4096)` 挡掉；`parse_message` 用的是**带长度**的 `doc.Parse(data, size)`
  （没有对无终止符的 `vector<char>` 越界读）；所有 rapidjson 取值都是 `HasMember` + `Is*` 双保险，
  不存在"缺字段时 `operator[]` 返回空引用再 `.GetString()`"这种解引用点。
- `RateLimiter`（mutex）、`logFatal`（mutex + 限频）、`g_liveSessions` / `g_writeQueueTotalBytes`（atomic）、
  `shared_from_this` 时机（`make_shared` 之后，且每个 handler 都持 `self`）都没问题。
- 没有自递归的 submit -> 回调 -> submit 链；`Db` 析构先 join worker 再销毁 pool，不存在 job 打在已销毁对象上。

## 四、本轮改动清单与验证状态

| 位置 | 改动 | 对应缺陷 |
| --- | --- | --- |
| `session.h:143-150` | 新增 `bool writing_`：同一条流只允许一个 `async_write` 在途 | 三.1 |
| `session.cpp:830-900` | `do_write()`：本批帧拷进 `shared_ptr<std::string>` 独占缓冲；回调先复位 `writing_`，`ec` 走 close，`closed_` 直接返回；pop 循环加空队列上界 | 三.1 |
| `session.cpp:783-787` | `send_message()` 开头 `if (closed_) return;`（关闭后不再记账、不再开第二个写） | 三.1 |
| `session.cpp:907-946` | `do_close()`：注释更正（现在 clear 是安全的，因为引用已不在 deque 上）；注销改为带身份比对 | 三.1 / 三.4 |
| `session_manager.h:30-38`、`session_manager.cpp:24-49` | `remove(uid, owner)`：只有这一格仍是自己才删 | 三.4 |
| `http_server.cpp:642-682, 704-727, 1837-1885, 1899-1904` | `http_session` 加 strand；`run()`/读/写/计时器回调全部串行其上；`send_json()` 从 DB 回调进来时先切回 strand | 三.2 |
| `http_server.cpp:151-172` | `make_deleted_ok` 去掉多余的 `EndObject()` | 三.3 |
| `session.cpp:387-401, 536-550` | 历史 `limit` 钳制到 `[1,200]` | 三.5 |
| `db.cpp:336-362` | `makeConn()` 查 `driver` / `connect()` 空指针，改抛异常 | 三.6 |
| `main.cpp:19-121, 155` | SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT 处理器：信号名 + `si_addr` + 原生栈写进 fd 2，再恢复默认并重新触发 | 二 |
| `build.sh:129-137` | `LimitCORE=0` 改为 `${LIMIT_CORE:-infinity}` | 二 |
| `step1_tcp_echo/CMakeLists.txt:24-34` | 服务端加 `-g -fno-omit-frame-pointer`（不改优化级别；`-DCXIM_NO_DEBUG_INFO=ON` 可关） | 二 |

**验证状态（如实）**：

- 已做：4 个被改的 TU 全部通过 `g++ -fsyntax-only -std=c++17`（boost 1.92 头 + 仓库内 RapidJSON + MariaDB Connector 头）；
  另外对 `http_server.cpp` / `friend_types.h` / `group_types.h` / `message_codec.h` 里所有 `make_*` / `serialize_*`
  的 Start/End 配对做了脚本级核对，**现在全部配平**。
- `main.cpp` 的处理器在 `#if defined(__linux__) || defined(__APPLE__)` 里；本机 MinGW 没有 `sigaction`，
  只能"强制打开分支 + 打桩"做类型检查，残余的一条报错来自我的桩（桩里的 `struct sigaction` 没有 `sa_mask`），不是代码。
  **它在 Ubuntu/glibc 上是否编译通过，仍以下面的真机构建为准。**
- **未做**：Linux 构建、部署、复跑压测。这些必须在 V0 上执行，我没有碰服务器。

## 五、下一轮怎么跑（含验收口径）

```bash
# 1) 带符号构建 + 允许落 core，装完先自证闸门真的生效
cd .../basic_service_20261107 && bash build.sh
sudo dpkg -i CXIM*.deb && sudo systemctl daemon-reload && sudo systemctl restart CXIM
systemctl show CXIM -p LimitCORE -p Restart -p NRestarts   # 期望 LimitCORE=infinity（不是 0）

# 2) 复跑同一支压测，另开窗口盯 journal（QUIET_LOG 保持 0；本轮日志已经是 0 了）
journalctl -u CXIM -f
./build/bin/Release/test_stress_03.exe --host 110.40.206.253 --http-port 10002 \
    --tcp-port 10001 --friends 100 --timeout 240

# 3) 若仍崩：现场这次就在 journal 里（没有 core 也能看到帧）
journalctl -u CXIM --since "-15 min" | grep -A40 "crash signal="

# 4) 有 core 就看全栈（务必 all-thread：SEGV 常发生在另一条线程刚 free 之后）
coredumpctl list cxim-server
coredumpctl dump cxim-server --output=/tmp/cxim.core
gdb -q /opt/CXIM/bin/cxim-server /tmp/cxim.core \
    -ex "thread apply all bt" -ex "p $_siginfo._sifields._sigfault.si_addr" -ex quit | head -120
# PIE 只剩地址时： addr2line -f -C -e /opt/CXIM/bin/cxim-server 0x<offset>

# 5) 终极一档（本地/预发跑，别在 2G 生产机上跑）：ASan 会把
#    "分配点 / 释放点 / 踩点"三段全打出来，use-after-free 一眼定案
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
      -DBOOST_ROOT="${BOOST_ROOT}" -DMariaDB_include="${MariaDB_include}" -DMariaDB_lib="${MariaDB_lib}"
cmake --build build-asan -j2
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:log_path=/tmp/asan ./build-asan/bin/cxim-server
```

验收口径，按顺序卡，不要跳：

1. `--friends 100` 跑完 **`deleted 100/100`、`failed 0`**，输出里没有 `10054` / `10061` / `no header end`；
   `systemctl show CXIM -p NRestarts` 不增长，journal 里**没有** `crash signal=`。
2. journal 里若出现 `[write] queue over budget`，那是 512KB/64MB **配得太紧**（配平问题，见本文件 1247 行的提醒），
   不要当 bug 去追；但本轮这条本来就不该出现。
3. 回归基线：再跑 `--friends 200`（11/17 它是绿的）。先确认回归修掉了，再谈性能。
4. 如果仍然 SEGV —— 这次至少**有帧可看**：把 `crash signal=` 后面的前 3 帧原文贴进本文件，
   就能直接定到函数与行，不需要再靠读代码猜。

## 六、一句话总结

上一轮死的是"死了没人拉"，这一轮死的是"真踩内存"（`status=11/SEGV`，非 OOM、非干净退出，
而且 `Restart=always` 已经把它拉起来了）。这份日志真正暴露的是：**部署层根本没打算让你知道死在哪一行**
（`LimitCORE=0` + Release 无符号）。本轮已把删除风暴里**唯一两条能证实的内存不安全路径**
（TCP 写队列与 `do_close()` 的生命周期竞争、HTTP 连接缺 strand）修掉，并给下一次崩溃配好了现场
（信号处理器 + core + `-g`）。


sudo systemctl show CXIM -p LimitCORE -p Restart -p NRestarts
Restart=no
NRestarts=0
LimitCORE=infinity
journalctl -u CXIM --since "-15 min" | grep -A40 "crash signal="
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: [fatal] crash signal=SIGSEGV addr=0x0000000000000033
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: [fatal] backtrace:
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libstdc++.so.6(_ZNSt8__detail15_List_node_base9_M_unhookEv+0xb) [0x77de25cd90eb]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xd4cd0) [0x5df94709ecd0]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xcf113) [0x5df947099113]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x72dce) [0x5df94703cdce]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x82129) [0x5df94704c129]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xbf53c) [0x5df94708953c]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xb622a) [0x5df94708022a]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xad3c8) [0x5df9470773c8]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xcee1a) [0x5df947098e1a]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x71b59) [0x5df94703bb59]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x7113f) [0x5df94703b13f]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9df6) [0x5df947093df6]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9d7c) [0x5df947093d7c]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9cf2) [0x5df947093cf2]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9c9a) [0x5df947093c9a]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9c62) [0x5df947093c62]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libstdc++.so.6(+0xf5ef9) [0x77de25cf5ef9]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libc.so.6(+0xa40da) [0x77de258a40da]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libc.so.6(+0x1377ac) [0x77de259377ac]
Nov 18 15:15:09 VM-0-11-ubuntu systemd[1]: CXIM.service: Main process exited, code=exited, status=139/n/a
Nov 18 15:15:09 VM-0-11-ubuntu systemd[1]: CXIM.service: Failed with result 'exit-code'.
Nov 18 15:15:09 VM-0-11-ubuntu systemd[1]: CXIM.service: Consumed 13.122s CPU time over 1min 57.345s wall clock time, 4M memory peak.

Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5764 : reply-10
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5846 : reply-79
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5842 : reply-86
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5821 : reply-41
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5815 : reply-2
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5848 : reply-88
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5772 : reply-24
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5840 : reply-74
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5807 : reply-64
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5790 : reply-23
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5829 : reply-15
Nov 18 15:15:06 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5768 : reply-32
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5781 : reply-40
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5843 : reply-69
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5775 : reply-22
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5757 : reply-5
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5810 : reply-78
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5773 : reply-14
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5800 : reply-61
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5834 : reply-98
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5836 : reply-75
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5816 : reply-54
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5812 : reply-92
Nov 18 15:15:07 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5799 : reply-36
Nov 18 15:15:08 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5813 : reply-93
Nov 18 15:15:08 VM-0-11-ubuntu cxim-server[2931881]: [route] 5756 -> 5851 : reply-85
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: [fatal] crash signal=SIGSEGV addr=0x0000000000000033
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: [fatal] backtrace:
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libstdc++.so.6(_ZNSt8__detail15_List_node_base9_M_unhookEv+0xb) [0x77de25cd90eb]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xd4cd0) [0x5df94709ecd0]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xcf113) [0x5df947099113]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x72dce) [0x5df94703cdce]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x82129) [0x5df94704c129]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xbf53c) [0x5df94708953c]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xb622a) [0x5df94708022a]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xad3c8) [0x5df9470773c8]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xcee1a) [0x5df947098e1a]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x71b59) [0x5df94703bb59]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0x7113f) [0x5df94703b13f]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9df6) [0x5df947093df6]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9d7c) [0x5df947093d7c]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9cf2) [0x5df947093cf2]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9c9a) [0x5df947093c9a]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /opt/CXIM/bin/cxim-server(+0xc9c62) [0x5df947093c62]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libstdc++.so.6(+0xf5ef9) [0x77de25cf5ef9]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libc.so.6(+0xa40da) [0x77de258a40da]
Nov 18 15:15:09 VM-0-11-ubuntu cxim-server[2931881]: /usr/lib/x86_64-linux-gnu/libc.so.6(+0x1377ac) [0x77de259377ac]
Nov 18 15:15:09 VM-0-11-ubuntu systemd[1]: CXIM.service: Main process exited, code=exited, status=139/n/a
Nov 18 15:15:09 VM-0-11-ubuntu systemd[1]: CXIM.service: Failed with result 'exit-code'.
Nov 18 15:15:09 VM-0-11-ubuntu systemd[1]: CXIM.service: Consumed 13.122s CPU time over 1min 57.345s wall clock time, 4M memory peak.


---

# 第二轮 core-dump：这次有栈了 —— `_List_node_base::_M_unhook`，即好友关系缓存的 LRU 节点（追加）

样本：本文件 1661~1740 行（`systemctl show` + `journalctl | grep -A40 "crash signal="`，
PID 2931881，15:13:12 启动、15:15:09 崩，跑了 **1min57s**）。
这份样本本身就是上一轮那套取证改造的产物：**第一次**拿到了原生栈与 `si_addr`。

## 一、先把我上一轮的两个 own goal 认掉（它们让这次的现场少了一半）

### 1. core 还是没落盘 —— 因为我的处理器"重新触发信号"是无效的

journal 实际打的是：

```
Main process exited, code=exited, status=139/n/a
Failed with result 'exit-code'.
```

不是 `code=dumped, status=11/SEGV`。原因是我上一轮在 handler 末尾写的：

```cpp
::signal(sig, SIG_DFL);
::raise(sig);
::_exit(128 + sig);
```

**`sigaction` 注册的处理函数在执行期间，内核默认阻塞被捕获的那个信号本身**（除非带 `SA_NODEFER`）。
于是 `raise(sig)` 只是把信号挂起、没有终止进程，控制流继续走到 `_exit(139)`：

* 进程变成"以退出码 139 正常结束" → **内核不落 core**，`coredumpctl` 依然是空的；
* `Restart=always` 也仍然会重启（exit-code 也算失败），但**这次连累下面第 2 条**，谁都没拉。

已修（修复⑫）：装处理器时带 `SA_NODEFER`（+ `SA_ONSTACK` 与 `sigaltstack` 备一段独立栈，
防"栈被踩/栈溢出时 handler 自己二次 fault"），这样 `raise()` 才真按信号致死、core 才落盘。
（我一度还加了 `pthread_sigmask(SIG_UNBLOCK,...)` 兜底，后来删了：`SA_NODEFER` 已经在
"进 handler 时不阻塞该信号"这一步解决问题，崩溃路径上少一个依赖就少一样可能出错的东西。）

### 2. `Restart=always` 从来没生效过：unit 文件不支持行尾注释

```
sudo systemctl show CXIM -p Restart  →  Restart=no
```

而生成的 unit 里明明写着：

```ini
Restart=always          # 原来的 on-failure 不覆盖退出码 0 的"干净退出"
```

**systemd 的解析器只认"行首 `#`"，`值 # 注释` 里的 `#` 及其后面全部属于值**，
所以这条被解析成 `always          # 原来的…` → 取值非法 → **静默回落到默认 `no`**（只在 load 时 warn 一句）。
对照也很干净：14:23:06 那次崩之后有 `Scheduled restart job`（当时机器上装的是旧 unit，`Restart=on-failure`，SEGV 算 failure）；
15:13 重装了带这行注释的新 unit 之后，15:15:09 崩完**journal 里再没有任何重启记录** —— 服务就这么躺着。

已修（修复⑬）：注释移到上一行独占，`Restart=always` 单独成行；并把"用 `systemctl show` 实测、
不要相信 unit 文件写了什么"写进验收口径（同一类坑：`LimitNOFILE` 也是这么要求的）。

## 二、这条栈说了什么

```
#0  libstdc++  std::__detail::_List_node_base::_M_unhook()      ← 摘双向链表节点
#1  cxim-server(+0xd4cd0)
#2  cxim-server(+0xcf113)
#3..#8  cxim-server(+0x72dce / 0x82129 / 0xbf53c / 0xb622a / 0xad3c8 / 0xcee1a)
#9,#10  cxim-server(+0x71b59 / 0x7113f)
#11..#15 cxim-server(+0xc9df6 / 0xc9d7c / 0xc9cf2 / 0xc9c9a / 0xc9c62)   ← 集中在同一段：线程入口 tramp
#16 libstdc++(+0xf5ef9)   execute_native_thread_routine
#17,#18 libc              start_thread / clone
```

能确定的四件事：

1. **被踩的是一块 `std::list` 节点。** `grep -rn "std::list" step1_tcp_echo/*.{h,cpp}` 只有一个结果：
   `db.h` 里好友关系缓存的 **`relLru_`**（LRU 表）。全工程没有第二个双向链表。
2. **调用方在我们自己的二进制里**（#1/#2 都是 `cxim-server`，不是 `libmariadbcpp.so`），
   也就是我们内联展开的 `list::erase()/pop_back()` —— 即 `relationCacheGet` 的过期删除
   或 `relationCacheErase` 这两处 `relLru_.erase(it)`。
3. **`addr=0x33`（而不是 0x0）排除了"对空 list/空 iterator 动手"**：
   `_M_unhook()` 干的是 `prev->_M_next = next`，出错地址 0x33 说明 `prev` 是个小整数（~0x2b）——
   这块节点内存**早已被 free 并被别的分配复用**（复用后的小整数字段正好落在那里）。
   这就是 `erase(<悬垂 iterator>)` 的标准形状，不是"忘了判空"。
4. **崩在 SQL worker 线程，不是 io 线程**（#16 是 `execute_native_thread_routine`，
   #11~#15 是 `std::thread` 入口 tramp + `Db::workerLoop` 那一小段）。
   这与"发消息热路径每条都要过关系缓存"完全自洽，也解释了为什么 journal 的最后几行是 `[route]`。

**时间点也对得上**：15:15:06~08 是 100+ 条 `hello/reply` 的路由风暴（`sendChecked` 每条调一次
`relationCacheGet`，未命中再 `relationCachePut`）；`RELATION_CACHE_TTL=10000` 意味着登录/加好友阶段
写进缓存的那批表项**此刻正好成批过期** → 本轮唯一一段"多线程并发 erase LRU 节点"的窗口。

另外要说清进展：**上一轮修的两条路（阶段4 写队列生命周期、HTTP 无 strand）这次过去了** ——
进程从 41s 活到 117s，崩溃点从阶段4 移到了阶段2 的路由风暴，换了另一个现场。

## 三、本轮改动：让"三张表手工同步"这件事在类型上不存在，并且在结构可疑时拒绝动手

先说清结论的边界：`relMap_ / relLru_ / relLruIters_` 的每条增删路径我都逐行审过 ——
全在 `relMu_` 内、key 一律 `{min,max}`、TTL 过期用 `find()` 不用 `operator[]`、
三张表同增同删、`splice` 不会失效被移动元素的 iterator —— **静态上没有一条我能指认的失步路径**。
所以有两种可能，而它们要求**同样的处置**：

* (a) 存在我没看出来的失步（第三张索引表 + `lit->second = relLru_.begin()` 这种"手动补一次同步"
  正是最容易写错的地方）→ 就让它**不可能**发生；
* (b) 是别处的野写踩了这个节点（上一轮修掉的写队列 UAF 就是典型"先踩坏、稍后在无关位置爆"；
  不能保证没有第三处）→ 那就**每次进锁先自检**，可疑时绝不再拿 iterator 去 erase（那正是这次的死法）。

| 修复 | 位置 | 做了什么 |
| --- | --- | --- |
| ⑩ | `db.h:449-491`、`db.cpp:639-716` | 删掉第三张表 `relLruIters_`：LRU 的 iterator 直接存进 `RelationEntry`（+ `lruLinked` 标记）。`splice` 本来就不失效被移动元素的 iterator，所以旧版那句"提队首后回写 begin()"整类操作被取消 —— **少一处必须靠纪律维持的同步** |
| ⑪ | `db.cpp:595-637` | 删除只有**一个出口** `relationCacheDropLocked()`；每次进锁先跑 `relationCacheSaneLocked()`：`relMap_.size() != relLru_.size()` 即认定结构已坏 → **打一条 `[db] relation cache inconsistent at <op>: map=N lru=M`（永不被 QUIET_LOG 静音）并把整张缓存丢弃**，而不是继续 erase。丢弃用 `swap` 到一张**故意泄漏**的表（O(1)，不遍历已不可信的链）+ `relMap_.clear()`（unordered_map 节点之间无链，可安全释放）。取舍很明确：缓存是纯延迟优化，丢了只慢一条带索引的 SELECT；踩内存会死人 |
| ⑫ | `main.cpp:19-150` | `SA_NODEFER + SA_ONSTACK + sigaltstack(64KB)`：让重新触发的信号真能致死并落 core（见一.1） |
| ⑬ | `build.sh:138-147` | `Restart=always` 的行尾注释移到上一行独占（见一.2） |

**验证状态（如实）**：`db.cpp / session.cpp / http_server.cpp / session_manager.cpp / main.cpp`
五个 TU 全部通过 `g++ -fsyntax-only -std=c++17`（仓库内 boost/RapidJSON/MariaDB 头）；
`main.cpp` 的 Linux 分支用桩强制打开后过了类型检查。真机构建与复跑仍需在 V0 上做。

## 四、30 秒的判决性实验（不用改代码、不用重新编译）

关系缓存有现成的总开关（`RELATION_CACHE_TTL=0` 时 `Get/Put/Erase` 第一行就 return，
LRU 一个节点都不会碰）：

```bash
sudo systemctl edit CXIM        # 写入两行：
#   [Service]
#   Environment=RELATION_CACHE_TTL=0
sudo systemctl daemon-reload && sudo systemctl restart CXIM
systemctl show CXIM -p Environment   # 确认已生效，再复跑 100 好友
```

* **不崩了** → 现场就锁死在这套 LRU 里；本轮 ⑩⑪ 已经把它变成"要么不可能失步、要么大声报告"。
  然后再把 TTL 恢复成 10000 复跑，看自检日志有没有出现。
* **还崩** → 说明是**外部野写**在踩 LRU 节点（缓存只是最近的受害者）。此时
  `[db] relation cache inconsistent ...` 会先出现在 journal 里，而有了修复⑫也会有 core，
  `gdb` 直接看那个节点被复用成了什么 —— 那才是元凶的指纹。

## 五、这条已经拿到的栈，现在就能变成行号（不必等下一次崩溃）

`backtrace_symbols_fd` 已经把**镜像内偏移**打在括号里（`cxim-server(+0xd4cd0)`），
配合上一轮加的 `-g`，在服务器上跑一条命令就出函数名与行号：

```bash
addr2line -f -C -i -e /opt/CXIM/bin/cxim-server \
  0xd4cd0 0xcf113 0x72dce 0x82129 0xbf53c 0xb622a 0xad3c8 0xcee1a \
  0x71b59 0x7113f 0xc9df6 0xc9d7c 0xc9cf2 0xc9c9a 0xc9c62
```

判读：#1/#2 若落在 `db.cpp` 的 `relationCacheGet` / `relationCacheErase`（或其内联的 `list::_M_erase`），
本节的定位成立；若落在别处，把输出贴进本文件再判。**这一步是本轮最便宜的一条信息，建议先做它。**

## 六、验收口径（按顺序卡）

1. **`systemctl show CXIM -p Restart` 必须是 `always`** —— 这一条现在是**已知不达标**，
   而且它比崩溃本身更要命：`Restart=no` + 公网服务 = 一次 SEGV 就永久停摆。
2. `journalctl -u CXIM -p 3` 里没有 `crash signal=`；`NRestarts` 不涨。
3. `--friends 100` 与 `--friends 200`（11/17 的回归基线）都跑完 `deleted N/N、failed 0`。
4. 若再崩，修复⑫的验收点：journal 里必须是 `code=dumped, status=11/SEGV` 且
   `coredumpctl list cxim-server` **有记录**；然后
   `coredumpctl gdb` → `thread apply all bt` + `p $_siginfo._sifields._sigfault.si_addr`。
5. 出现 `[db] relation cache inconsistent at ...` 不要当噪音：那是自检真的挡住了第二次踩空，
   同时也是一条"存在外部野写"的证据，按新线索继续查。

## 七、一句话总结

上一轮的取证改造**起作用了**（第一次拿到 `si_addr` 与 15 帧原生栈），但同时暴露我自己埋的两颗雷：
`SA_NODEFER` 缺失导致 core 依然没落盘、unit 里的行尾注释导致 `Restart=always` 一直是 `no`。
这两条已修。栈顶把范围收窄到全工程唯一的 `std::list` —— 好友关系缓存的 LRU 节点被摘时指针是垃圾，
即 `erase(悬垂 iterator)`；本轮把"三张表手工同步"改成"iterator 存在 entry 里 + 唯一删除出口 + 进锁先自检"，
可疑时丢缓存不动手，并给了一个不用重编译的 TTL=0 判决实验与一条把现有栈直接翻成行号的 addr2line 命令。
