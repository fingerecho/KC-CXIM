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
