# test_stress_03 IOCP 版（Windows 11）编译与运行说明

好友全生命周期分阶段压测客户端，从 Boost.Asio 多线程阻塞版改写为 **Windows IOCP 高并发长连接版**。

> 交付说明（请先看）：本代码按要求**未做编译验证**（沙箱里没有 Windows 工具链）。
> 我逐段做了静态复核，过程中发现并修掉 10 处缺陷，其中 6 处会直接编译失败
> （无类型的成员声明、调用不存在的方法、跨类访问 protected 成员、函数签名不匹配、
> 非法友元声明、依赖未包含的 SDK 头文件），另有 4 处是运行期错误
> （定时器延迟参数被吞导致重试/超时全部瞬间触发、IOCP 循环等待计算写坏、
> 传空缓冲调用 SIO_KEEPALIVE_VALS、心跳打散对主用户算出负偏移）。
> 但静态复核不等于编译通过，首次在 VS2022 上仍可能报错——把报错整段贴回来，我直接改。

---

## 1. 文件清单

| 文件 | 作用 |
| --- | --- |
| `test_stress_03_iocp.cpp` | 压测客户端主体（单文件，不依赖 Boost） |
| `build_msvc.bat` | VS2022 一键编译脚本 |
| `README_IOCP编译与运行说明.md` | 本说明 |

## 2. 依赖

只需要两类头文件，**不再需要 Boost.Asio**：

- 你项目里的 `message.h`、`message_codec.h`（协议结构与编解码，和原版一致）
- `rapidjson`（只用它解析 HTTP 响应 JSON）

链接库：`ws2_32.lib`（`mswsock.lib` 不需要，ConnectEx 通过 `WSAIoctl` 动态取函数指针，GUID 也自带一份，避免不同 SDK 的声明差异）。

## 3. 编译

### 方式 A：脚本

把 `test_stress_03_iocp.cpp` 和 `build_msvc.bat` 放到能看见 `message.h` / `message_codec.h` 的目录（或改脚本里的 `/I` 路径），在 **Developer Command Prompt for VS 2022** 里执行：

```bat
build_msvc.bat
```

产物：`test_stress_03_iocp.exe`。

### 方式 B：手工命令

```bat
cl /std:c++17 /O2 /EHsc /W3 /utf-8 test_stress_03_iocp.cpp ^
   /I"." /I"..\..\third_party\rapidjson\include" ^
   /link ws2_32.lib
```

关键开关：`/std:c++17`（用了结构化绑定以外的 C++17 特性：`std::make_unique`、`if` 初始化、`clamp` 替代写法等）、`/EHsc`（异常）、`/utf-8`（源码里的中文注释与控制台输出）。

## 4. 运行

```bat
:: 小规模冒烟（行为与原版一致，逐条打印进度）
test_stress_03_iocp.exe --host 10.0.0.21 --friends 100

:: 5000 条长连接，带启动爬坡
test_stress_03_iocp.exe --host 10.0.0.21 --friends 5000 --ramp-up 300 --hb-interval 30

:: 只压建连与生命周期，静默模式
test_stress_03_iocp.exe --host 10.0.0.21 --friends 20000 --io-threads 8 --http-threads 64
```

参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--host` | `127.0.0.1` | 服务端地址，只解析一次并复用 |
| `--http-port` / `--tcp-port` | `8080` / `8888` | 与原版一致 |
| `--friends N` | `100` | 好友数 = 长连接数，可到万级 |
| `--io-threads N` | `0` | IOCP 工作线程数，`0` = `max(2, CPU 核数)` |
| `--http-threads N` | `0` | HTTP 线程池，`0` = `clamp(N/16, 16, 256)` |
| `--hb-interval SEC` | `30` | 应用层心跳间隔，**必须小于服务端 `IDLE_TIMEOUT_SEC`** |
| `--connect-attempts N` | `10` | TCP 建连重试次数，间隔 300ms |
| `--ramp-up N` | `500` | 每秒启动好友数，`0` = 立即全启动 |
| `--timeout SEC` | `120` | 单阶段超时 |
| `--verbose` | 关 | 大规模时也打印逐连接进度 |

退出码：全部阶段成功且主用户好友列表清空 → `0`；任一失败或中止 → `1`。

## 5. 线程与并发模型（这版的核心变化）

```
主线程        只做阶段编排，等待一律用原子计数器轮询，不持有连接
IOCP 线程 xN  GetQueuedCompletionStatus -> 只读 overlapped 的操作类型 -> 转投逻辑队列
逻辑线程 x1   全部状态机、帧编解码、心跳扫描、定时器、阶段门
HTTP 池 xM    短连接 REST，结果回投逻辑线程
```

- N 条长连接 = N 个 socket 句柄，**不再是 N 个线程 + N 个收线程 + N 个心跳线程**。
- 逻辑线程单线程化，因此业务代码里几乎没有锁，不会出现数据竞争。
- 代价：帧解析/序列化被串行化，经验量级约 10~30 万帧/秒，压 5000~20000 连接足够。
  要再往上冲，正确做法是**把连接 hash 分片到 2~4 个独立逻辑环**（每环自己的 IOCP +
  定时器 + 心跳扫描），而不是给每条连接加锁。

## 6. 压测前的 Windows 侧注意项

1. **临时端口**：TCP 长连接本身只占一次端口，但 HTTP 用的是 `Connection: close`
   短连接，一次生命周期要打 5~6 个 HTTP 请求，N=5000 时端口消耗与 `TIME_WAIT`
   积压会比 TCP 更先成为瓶颈。先看现状再决定要不要调：
   ```bat
   netstat -s -p tcp | findstr /i "failed connects"
   ```
   需要时调 `TcpTimedWaitDelay` / `MaxUserPort`（改注册表并重启生效，属机器级变更，
   建议只在专用压测机上做）。
2. **接收自动调优**：`netsh int tcp show global` 确认 `Autotuning level` 不是
   `disabled`，否则大流量下窗口会限制吞吐。
3. **别把瓶颈误判给服务端**：客户端自己也要看 CPU。逻辑线程是单线程，跑满 100%
   一个核时，延迟分位数里就含了客户端排队时间——这不是服务端慢。任务管理器看
   `test_stress_03_iocp.exe` 的 CPU 是否≈单核满载即可判断。
4. **`--ramp-up` 不是可选项**：5000 个好友瞬间并发注册，几乎必然触发服务端认证
   限流（429 风暴），此时测出来的是限流器不是业务。默认 500/s，可按服务端容量调。

## 7. 与 Asio 原版的行为差异（有意为之，不是漏改）

| 项 | 原版（Asio） | 本版（IOCP） | 原因 |
| --- | --- | --- | --- |
| 每好友线程 | 3 个线程 | 0 个 | 万级连接不可行 |
| 心跳 | 每连接独立线程 100ms 轮询 | 全局 1s 扫描 + 按连接序号打散 | 减少唤醒；避免同一秒集中心跳打出尖峰 |
| 阶段等待 | 线程阻塞 `wait_phase` | 阶段门 + 续延投递 | 不占线程 |
| 429/503 退避 | `sleep` 当前线程 | 定时器延迟重试 | 不占线程；退避仍计入延迟采样（口径与原版一致） |
| 主用户回复 | 攒齐 N 条文本再逐条回复 | 边收边回，串行等 `SendAck` | 事件模型下更自然，延迟更低 |
| 投递 ACK 顺序 | 回复全部落地后统一补发 | **保持不变** | 避免 ACK 与 Reply 同时写同一行 `conversation_members` 触发 MariaDB 1020 |
| 乱序回复 | 早于 `SendAck` 到达的回复被当无关帧丢弃 | 暂存并在收到 `SendAck` 后立即消费 | 原版这里会死等到阶段超时 |
| 好友启动 | 一次性全部启动 | 爬坡限流（`--ramp-up`） | 避免 429 风暴 |

阶段划分、校验规则（回复正文必须等于 `reply-<idx>`、主用户列表最终为空等）、
延迟采样点位、重试次数与 `msg_id` 复用去重策略，均与原版一致。

## 8. 报告怎么读

```
===== Stress03 Report (IOCP) =====
...
--- iocp runtime ---
io worker threads / http pool threads     线程配置是否符合预期
tcp connections peak / open              峰值连接数与收尾是否归零
frames sent / recv, bytes sent / recv     实际流量规模，用来反推吞吐
throughput                               每秒完成多少个好友全生命周期
latency (ms) p50/p95/p99/avg/max         八个阶段的分位数
```

看结果时优先关注三件事：

1. `full lifecycle ok` 是否等于 `N`，`failed` 是否为 0；不为 0 时先看
   `friend i error:` 的第一条——那才是根因，`Aborted` 报告里的 `reason`
   现在已经会带首个真实失败点，不再只报一个 timeout。
2. `tcp send` / `tcp reply` 的 p99 是否明显大于 HTTP 各阶段——若是，问题在
   长连接链路（转发、推送、锁），不在 REST/DB。
3. `create request` 与 `accept` 的 p99 差距——申请创建是主用户单点写、
   通过是 N 个好友并发写，两者差距直接暴露服务端在并发写上的争用。

## 9. 已知边界

- 只支持 Windows；非 Windows 编译直接 `#error`，防止误用。
- HTTP 解析沿用原版的"取响应头之后的全部内容为 body"，未处理 chunked；
  若服务端改为分块传输，需另行适配。
- 协议帧长度上限 `maxFrameBytes = 4MB`，发送积压上限 `maxQueuedBytes = 8MB`
  （超限判失败，避免静默丢帧导致整轮超时难定位）。
- 逻辑线程单点：极端消息风暴下客户端自身可能成为瓶颈，已在第 6 节给出判断方法。
