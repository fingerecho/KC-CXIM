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
