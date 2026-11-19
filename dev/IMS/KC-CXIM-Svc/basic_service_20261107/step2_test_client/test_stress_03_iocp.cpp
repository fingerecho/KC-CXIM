// ============================================================================
// test_stress_03_iocp.cpp —— 好友全生命周期分阶段压测客户端
//                            Windows 11 / IOCP 高并发长连接版
// ----------------------------------------------------------------------------
// 由 test_stress_03.cpp（Boost.Asio + 每好友一线程 + 阻塞收发）改写。
// 业务场景、阶段划分、校验规则、延迟口径与原版保持一致：
//   注册主用户/好友 -> 全部 TCP 登录（长连接全程不断开）
//     -> 主用户向 N 个好友发好友申请
//     -> N 个好友并发通过申请
//     -> 好友发文本 + 主用户回复 + 双方 ACK
//     -> N 个好友并发查好友列表（主用户也查一次）
//     -> N 个好友并发删除主用户（主用户列表最终应为空）
//
// 用法：
//   test_stress_03_iocp [--host H] [--friends 5000] [--http-port 8080]
//                       [--tcp-port 8888] [--io-threads 0] [--http-threads 0]
//                       [--hb-interval 30] [--connect-attempts 10]
//                       [--ramp-up 500] [--timeout 120] [--verbose] [--help]
//
// 编译（VS2022 开发者命令提示符，或直接用随附 build_msvc.bat）：
//   cl /std:c++17 /O2 /EHsc /utf-8 test_stress_03_iocp.cpp ^
//      /I"项目头文件目录" /I"rapidjson include 目录" ^
//      /link ws2_32.lib mswsock.lib
//   依赖：项目内 message.h / message_codec.h + rapidjson（只用 JSON 解析）。
//
// ---------------------------------------------------------------------------
// 相对 Asio 版改了什么（以及为什么）
// ---------------------------------------------------------------------------
// 1) 线程模型。原版：每个好友 = 1 个 std::thread + 1 个 io_context + 收线程 +
//    心跳线程，N=100 已经约 300 线程，N=5000 直接失控。现在：
//      · IOCP 工作线程 = max(2, CPU 核数)（可用 --io-threads 覆盖）
//        只做"取完成包 + 转投逻辑队列"，不碰业务、不碰缓冲区
//      · 逻辑线程 = 1（Reactor）：全部状态机、帧编解码、心跳、定时器
//      · HTTP 线程池 = 16~256：REST 短连接，不占用逻辑线程
//    N 条长连接只消耗 N 个 socket 句柄，不再消耗 N 个线程。
// 2) 解除 select 时代的 1024 socket 上限；IOCP 只看非分页内存与端口资源。
// 3) 收发改为 ConnectEx / WSARecv / WSASend 全重叠，4 字节大端长度前缀的
//    半包/粘包由每连接增量解析器处理（不再需要 recv 线程 + 阻塞 read）。
// 4) 心跳从"每连接一个 100ms 轮询线程"改成"每 1s 一次全局扫描"，并按连接
//    序号打散到期时间，避免 N 条连接同一秒集中发心跳打出服务端尖峰。
// 5) 阶段推进从"线程阻塞在 wait_phase"改成"阶段门 + 续延投递"，等待阶段
//    不占用任何线程。
// 6) 429/503 退避不再 sleep 线程，改为定时器延迟重试；退避时间依旧计入延迟
//    采样（与原版口径一致：客户端真实感受的延迟就该包含退避）。
// 7) 新增启动爬坡（--ramp-up，默认 500 个/秒）。5000 个好友瞬间并发注册会把
//    认证限流直接打满（429 风暴），爬坡后延迟分位数才有意义。
// 8) 主用户回复由"攒齐 N 条再逐条回复"改为"边收边回、串行等 SendAck"，但保留
//    原版落库顺序约束：所有回复落地后才统一补发主用户对好友文本的投递 ACK，
//    避免 ACK 与 Reply 同时写同一行 conversation_members 触发 MariaDB 1020。
//
// 关于"单线程逻辑环"的取舍：所有业务状态机跑在一个线程上，所以本文件几乎没
// 有锁，也不存在数据竞争；代价是帧编解码被串行化。经验量级：单线程做 protobuf
// 编解码 + 帧解析大约 10~30 万帧/秒，压 5000~20000 条长连接足够。真要冲更高吞
// 吐，正确做法是把连接按 hash 分片到 2~4 个独立逻辑环（每环自己的 IOCP + 定时
// 器 + 心跳扫描），而不是给每条连接加锁。
//
// 生命周期不变式（改 TcpConn 之前务必先读）：
//   · 状态迁移全部发生在逻辑线程；IOCP 线程只读 ConnOverlapped::op，然后转投
//     完成事件，绝不引用 TcpConn 内部状态。
//   · pendingOps_ 在 post 前 +1，在完成事件被逻辑线程处理时 -1；pendingOps_>0
//     期间 selfHold_ 持强引用，对象必定存活，IOCP 线程持有的裸 TcpConn* 安全。
//   · 每个 handle() 入口先复制一份 selfHold_，即使处理过程中对象被 reap，析构
//     也推迟到本函数返回之后。
//   · 只有逻辑线程调用 closesocket，且只在 pendingOps_==0 时。
// ============================================================================





// ============================================================================
// test_stress_03_iocp.cpp —— 好友全生命周期分阶段压测客户端
//                            Windows 11 / IOCP 高并发长连接版
// ----------------------------------------------------------------------------
// 由 test_stress_03.cpp（Boost.Asio + 每好友一线程 + 阻塞收发）改写。
// 业务场景、阶段划分、校验规则、延迟口径与原版保持一致：
//   注册主用户/好友 -> 全部 TCP 登录（长连接全程不断开）
//     -> 主用户向 N 个好友发好友申请
//     -> N 个好友并发通过申请
//     -> 好友发文本 + 主用户回复 + 双方 ACK
//     -> N 个好友并发查好友列表（主用户也查一次）
//     -> N 个好友并发删除主用户（主用户列表最终应为空）
//
// 用法：
//   test_stress_03_iocp [--host H] [--friends 5000] [--http-port 8080]
//                       [--tcp-port 8888] [--io-threads 0] [--http-threads 0]
//                       [--hb-interval 30] [--connect-attempts 10]
//                       [--ramp-up 500] [--timeout 120] [--verbose] [--help]
//
// 编译（VS2022 开发者命令提示符，或直接用随附 build_msvc.bat）：
//   cl /std:c++17 /O2 /EHsc /utf-8 test_stress_03_iocp.cpp ^
//      /I"项目头文件目录" /I"rapidjson include 目录" ^
//      /link ws2_32.lib
//   （mswsock.h 只提供 SO_UPDATE_CONNECT_CONTEXT / ConnectEx 的声明，
//     函数指针靠 WSAIoctl 运行时取，不需要链接 mswsock.lib）
//   依赖：项目内 message.h / message_codec.h + rapidjson（只用 JSON 解析）。
//
// ---------------------------------------------------------------------------
// 相对 Asio 版改了什么（以及为什么）
// ---------------------------------------------------------------------------
// 1) 线程模型。原版：每个好友 = 1 个 std::thread + 1 个 io_context + 收线程 +
//    心跳线程，N=100 已经约 300 线程，N=5000 直接失控。现在：
//      · IOCP 工作线程 = max(2, CPU 核数)（可用 --io-threads 覆盖）
//        只做"取完成包 + 转投逻辑队列"，不碰业务、不碰缓冲区
//      · 逻辑线程 = 1（Reactor）：全部状态机、帧编解码、心跳、定时器
//      · HTTP 线程池 = 16~256：REST 短连接，不占用逻辑线程
//    N 条长连接只消耗 N 个 socket 句柄，不再消耗 N 个线程。
// 2) 解除 select 时代的 1024 socket 上限；IOCP 只看非分页内存与端口资源。
// 3) 收发改为 ConnectEx / WSARecv / WSASend 全重叠，4 字节大端长度前缀的
//    半包/粘包由每连接增量解析器处理（不再需要 recv 线程 + 阻塞 read）。
// 4) 心跳从"每连接一个 100ms 轮询线程"改成"每 1s 一次全局扫描"，并按连接
//    序号打散到期时间，避免 N 条连接同一秒集中发心跳打出服务端尖峰。
// 5) 阶段推进从"线程阻塞在 wait_phase"改成"阶段门 + 续延投递"，等待阶段
//    不占用任何线程。
// 6) 429/503 退避不再 sleep 线程，改为定时器延迟重试；退避时间依旧计入延迟
//    采样（与原版口径一致：客户端真实感受的延迟就该包含退避）。
// 7) 新增启动爬坡（--ramp-up，默认 500 个/秒）。5000 个好友瞬间并发注册会把
//    认证限流直接打满（429 风暴），爬坡后延迟分位数才有意义。
// 8) 主用户回复由"攒齐 N 条再逐条回复"改为"边收边回、串行等 SendAck"，但保留
//    原版落库顺序约束：所有回复落地后才统一补发主用户对好友文本的投递 ACK，
//    避免 ACK 与 Reply 同时写同一行 conversation_members 触发 MariaDB 1020。
//
// 关于"单线程逻辑环"的取舍：所有业务状态机跑在一个线程上，所以本文件几乎没
// 有锁，也不存在数据竞争；代价是帧编解码被串行化。经验量级：单线程做 protobuf
// 编解码 + 帧解析大约 10~30 万帧/秒，压 5000~20000 条长连接足够。真要冲更高吞
// 吐，正确做法是把连接按 hash 分片到 2~4 个独立逻辑环（每环自己的 IOCP + 定时
// 器 + 心跳扫描），而不是给每条连接加锁。
//
// 生命周期不变式（改 TcpConn 之前务必先读）：
//   · 状态迁移全部发生在逻辑线程；IOCP 线程只读 ConnOverlapped::op，然后转投
//     完成事件，绝不引用 TcpConn 内部状态。
//   · pendingOps_ 在 post 前 +1，在完成事件被逻辑线程处理时 -1；pendingOps_>0
//     期间 selfHold_ 持强引用，对象必定存活，IOCP 线程持有的裸 TcpConn* 安全。
//   · 每个 handle() 入口先复制一份 selfHold_，即使处理过程中对象被 reap，析构
//     也推迟到本函数返回之后。
//   · 只有逻辑线程调用 closesocket，且只在 pendingOps_==0 时。
// ============================================================================

#ifndef _WIN32
#error "test_stress_03_iocp.cpp 仅支持 Windows（依赖 IOCP / ConnectEx）"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>   // SO_UPDATE_CONNECT_CONTEXT 真正的定义处

// 某些 SDK 布局下 mswsock.h 不会被间接包含，这里自带兜底定义（值取自 mswsock.h：0x7010），
// 避免 C2065 未声明标识符；已定义时不做任何事。
#ifndef SO_UPDATE_CONNECT_CONTEXT
#define SO_UPDATE_CONNECT_CONTEXT 0x7010
#endif

// 自带链接指示：换编译器入口（bat / VS 工程 / CMake）都不会漏链 Winsock2
#pragma comment(lib, "ws2_32.lib")

// ConnectEx 原型自带一份，避免依赖 mswsock.h 的声明与 mswsock.lib 的链接差异
typedef BOOL(WINAPI* Pfn_ConnectEx)(SOCKET, const struct sockaddr*, int, PVOID, DWORD,
                                                  LPDWORD, LPOVERLAPPED);

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "message.h"
#include "message_codec.h"
#include "rapidjson/document.h"

using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch()).count();
}
int64_t tick_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

std::mutex g_logMu;
void logline(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMu);
    std::cout << "[stress03iocp] " << s << "\n" << std::flush;
}
#define LOGX(expr)                          \
    do {                                    \
        std::ostringstream _os;             \
        _os << expr;                        \
        logline(_os.str());                 \
    } while (0)

std::atomic<bool> g_verbose{false};
std::atomic<int> g_totalFriends{0};
// 小规模逐条打印进度；大规模静默（除非 --verbose）
inline bool trace_on() { return g_verbose.load() || g_totalFriends.load() <= 10; }
#define TRACE(expr)                         \
    do {                                    \
        if (trace_on()) LOGX(expr);         \
    } while (0)

std::string wsa_err_str(int err) {
    char* buf = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, (DWORD)err, 0, (LPSTR)&buf, 0, nullptr);
    std::string s = (n && buf) ? buf : ("WSA error " + std::to_string(err));
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
struct Config {
    std::string host = "127.0.0.1";
    unsigned short httpPort = 8080;
    unsigned short tcpPort = 8888;
    int friends = 100;
    int hbIntervalSec = 30;      // 需小于服务端 IDLE_TIMEOUT_SEC（默认 60）
    int timeoutSec = 120;
    int ioThreads = 0;           // 0 => max(2, CPU 核数)
    int httpThreads = 0;         // 0 => clamp(friends/16, 16, 256)
    int connectAttempts = 10;
    int rampUpPerSec = 500;      // 每秒启动好友数，0 = 立即全部启动
    uint32_t maxFrameBytes = 4u * 1024u * 1024u;
    uint64_t maxQueuedBytes = 8ull * 1024ull * 1024ull;
};
Config g_cfg;

// 目标地址只解析一次，所有连接复用（换端口只改 port 字段）
SOCKADDR_STORAGE g_targetAddr{};

void set_port(SOCKADDR_STORAGE& ss, unsigned short port) {
    if (ss.ss_family == AF_INET)
        reinterpret_cast<sockaddr_in*>(&ss)->sin_port = htons(port);
    else if (ss.ss_family == AF_INET6)
        reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port = htons(port);
}

void resolve_host() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(g_cfg.host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res)
        throw std::runtime_error("getaddrinfo " + g_cfg.host + ": " + gai_strerrorA(rc));
    std::memset(&g_targetAddr, 0, sizeof(g_targetAddr));
    const size_t n = (std::min)(res->ai_addrlen, sizeof(g_targetAddr));
    std::memcpy(&g_targetAddr, res->ai_addr, n);
    freeaddrinfo(res);
}

// ---------------------------------------------------------------------------
// 阶段号、计数器（语义与原版一致）
// ---------------------------------------------------------------------------
std::atomic<int> g_phase{0};
std::atomic<int> g_ready{0};
std::atomic<int> g_accepted{0};
std::atomic<int> g_messaged{0};
std::atomic<int> g_listed{0};
std::atomic<int> g_deleted{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_sendRetries{0};
std::atomic<int> g_reqCreated{0};
std::atomic<int> g_mainReady{0};
std::atomic<int> g_mainFlushed{0};
std::atomic<int> g_replyAcks{0};

// IOCP 运行期指标
std::atomic<int> g_socketsOpen{0};
std::atomic<int> g_socketsPeak{0};
std::atomic<uint64_t> g_framesSent{0};
std::atomic<uint64_t> g_framesRecv{0};
std::atomic<uint64_t> g_bytesSent{0};
std::atomic<uint64_t> g_bytesRecv{0};
std::atomic<uint64_t> g_httpCalls{0};

std::mutex g_fatalMu;
std::string g_fatalErr;  // 首个失败原因
void mark_failed(const std::string& what) {
    {
        std::lock_guard<std::mutex> lk(g_fatalMu);
        if (g_fatalErr.empty()) g_fatalErr = what;
    }
    g_failed.fetch_add(1);
    g_stop.store(true);
}

bool wait_counter(std::atomic<int>& c, int target, int timeoutSec) {
    const int64_t deadline = tick_ms() + (int64_t)timeoutSec * 1000;
    while (tick_ms() < deadline) {
        if (g_stop.load()) return c.load() >= target;
        if (c.load() >= target) return true;
        Sleep(20);
    }
    return c.load() >= target;
}

// 心跳接口：逻辑线程每 1s 扫描一次。g_hbList 在逻辑线程启动前构建完毕，
// 之后只读，因此扫描不需要任何锁。
struct IHb {
    virtual ~IHb() = default;
    virtual void hb_tick(int64_t nowMs) = 0;
};
std::vector<IHb*> g_hbList;

class Logic;
void enter_phase(int p);

// ---------------------------------------------------------------------------
// 帧编解码：线上格式与原版一致（4 字节大端长度 + 消息体）
// ---------------------------------------------------------------------------
std::vector<char> encode_frame(const std::string& payload) {
    std::vector<char> out(4 + payload.size());
    const u_long be = htonl((u_long)payload.size());
    std::memcpy(out.data(), &be, 4);
    std::memcpy(out.data() + 4, payload.data(), payload.size());
    return out;
}

// ---------------------------------------------------------------------------
// 逻辑线程（Reactor）：任务队列 + 最小堆定时器 + 阶段门 + 心跳扫描
// ---------------------------------------------------------------------------
class Logic {
public:
    static Logic& I() {
        static Logic s;
        return s;
    }

    void start() { thread_ = std::thread([this] { run(); }); }

    void begin_stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        cv_.notify_all();
    }

    // 任意线程可调用
    void post(std::function<void()> fn) {
        if (!fn) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_) return;
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    // 延迟执行。非逻辑线程调用会被转投到逻辑线程，保证定时器堆只被单线程访问。
    void later(int64_t delayMs, std::function<void()> fn) {
        later_abs(tick_ms() + (delayMs > 0 ? delayMs : 0), std::move(fn));
    }
    void later_abs(int64_t due, std::function<void()> fn) {
        if (std::this_thread::get_id() == logicTid_) {
            arm(due, std::move(fn));
            return;
        }
        post([due, fn]() { Logic::I().later_abs(due, fn); });
    }

    // --- 阶段门 ---
    void wait_phase(int target, std::function<void()> resume,
                    std::function<void()> onTimeout, int64_t timeoutMs) {
        if (g_phase.load() >= target) {
            post(std::move(resume));
            return;
        }
        phaseWaiters_[target].push_back(PhaseItem{std::move(resume), std::move(onTimeout)});
        if (timeoutMs > 0)
            arm(tick_ms() + timeoutMs, [this, target] { expire_phase(target); });
    }
    void release_phase(int target) {
        auto it = phaseWaiters_.find(target);
        if (it == phaseWaiters_.end()) return;
        auto items = std::move(it->second);
        phaseWaiters_.erase(it);
        for (auto& x : items)
            if (x.resume) x.resume();
    }
    void abort_all_waiters() {
        for (auto& kv : phaseWaiters_)
            for (auto& x : kv.second)
                if (x.onTimeout) x.onTimeout();
        phaseWaiters_.clear();
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }
    bool running() const { return running_; }

private:
    Logic() = default;

    struct TimerEntry {
        int64_t due;
        uint64_t seq;
        std::function<void()> fn;
    };
    struct TimerCmp {
        bool operator()(const TimerEntry& a, const TimerEntry& b) const {
            if (a.due != b.due) return a.due > b.due;  // 小顶堆
            return a.seq > b.seq;
        }
    };
    struct PhaseItem {
        std::function<void()> resume;
        std::function<void()> onTimeout;
    };

    void arm(int64_t due, std::function<void()> fn) {
        timers_.push(TimerEntry{due, ++seq_, std::move(fn)});
        cv_.notify_all();  // 让等待中的循环重算下次到期时间
    }

    void run() {
        logicTid_ = std::this_thread::get_id();
        arm(tick_ms() + 1000, [this] { hb_sweep(); });
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                if (q_.empty()) {
                    // 定时器堆只被逻辑线程增删，持锁期间读堆顶是安全的
                    int64_t waitMs = 5000;
                    if (!timers_.empty())
                        waitMs = (std::max)(timers_.top().due - tick_ms(), (int64_t)0);
                    cv_.wait_for(lk, std::chrono::milliseconds(waitMs));
                }
                if (!q_.empty()) {
                    task = std::move(q_.front());
                    q_.pop_front();
                } else if (!running_) {
                    return;  // 已下线且队列排空
                }
            }
            if (task) task();
            drain_timers();
        }
    }

    void drain_timers() {
        const int64_t now = tick_ms();
        // 单次最多跑 20000 个定时器，避免某个定时器回调无限自我重投把线程饿死
        for (int guard = 0; guard < 20000 && !timers_.empty() && timers_.top().due <= now;
             ++guard) {
            auto fn = std::move(timers_.top().fn);
            timers_.pop();
            fn();
        }
    }

    void expire_phase(int target) {
        if (g_phase.load() >= target) return;  // 已放行，超时作废
        auto it = phaseWaiters_.find(target);
        if (it == phaseWaiters_.end()) return;
        auto items = std::move(it->second);
        phaseWaiters_.erase(it);
        for (auto& x : items)
            if (x.onTimeout) x.onTimeout();
    }

    void hb_sweep() {
        const int64_t now = tick_ms();
        for (auto* h : g_hbList)
            if (h) h->hb_tick(now);
        if (running_ && !g_stop.load()) arm(now + 1000, [this] { hb_sweep(); });
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCmp> timers_;
    std::unordered_map<int, std::vector<PhaseItem>> phaseWaiters_;
    uint64_t seq_ = 0;
    bool running_ = true;
    std::thread thread_;
    std::thread::id logicTid_;

};

void enter_phase(int p) {
    g_phase.store(p);
    Logic::I().post([p] { Logic::I().release_phase(p); });
}

// ---------------------------------------------------------------------------
// IOCP 事件循环
// ---------------------------------------------------------------------------
enum class IoOp : uint32_t { Connect = 1, Recv = 2, Send = 3 };

struct ConnOverlapped {
    OVERLAPPED ov;  // 必须为首字段
    IoOp op;
};

class TcpConn;

class IoLoop {
public:
    HANDLE Port() const { return port_; }

    void start(unsigned threads) {
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, threads);
        if (!port_)
            throw std::runtime_error("CreateIoCompletionPort: " +
                                     wsa_err_str((int)GetLastError()));
        const unsigned hw = std::thread::hardware_concurrency();
        workers_ = threads ? threads : (std::max)(2u, hw ? hw : 2u);
        for (unsigned i = 0; i < workers_; ++i) threads_.emplace_back([this] { worker(); });
    }

    void stop() {
        if (!port_) return;
        for (unsigned i = 0; i < workers_; ++i)
            PostQueuedCompletionStatus(port_, 0, 0, nullptr);  // key==0 && ov==null => 退出
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
        CloseHandle(port_);
        port_ = nullptr;
    }
    unsigned workers() const { return workers_; }

    void track() {
        const int cur = ++g_socketsOpen;
        int peak = g_socketsPeak.load();
        while (cur > peak && !g_socketsPeak.compare_exchange_weak(peak, cur)) {}
    }
    void untrack() { g_socketsOpen.fetch_sub(1); }

private:
    void worker();
    HANDLE port_ = nullptr;
    unsigned workers_ = 0;
    std::vector<std::thread> threads_;
};
IoLoop g_io;

// ---------------------------------------------------------------------------
// TcpConn：一条长连接，ConnectEx / WSARecv / WSASend 全重叠
// ---------------------------------------------------------------------------
class TcpConn : public std::enable_shared_from_this<TcpConn> {
public:
    using FrameCb = std::function<void(std::vector<char>& body)>;
    using ErrorCb = std::function<void(int wsaErr, const char* stage)>;
    using ConnectCb = std::function<void(int wsaErr)>;

    explicit TcpConn(SOCKADDR_STORAGE addr) : addr_(addr) {
        connectOv_.op = IoOp::Connect;
        recvOv_.op = IoOp::Recv;
        sendOv_.op = IoOp::Send;
        clear_ov(&connectOv_.ov);
        clear_ov(&recvOv_.ov);
        clear_ov(&sendOv_.ov);
    }

    ~TcpConn() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }
    TcpConn(const TcpConn&) = delete;
    TcpConn& operator=(const TcpConn&) = delete;

    void set_callbacks(FrameCb f, ErrorCb e, ConnectCb c) {
        frameCb_ = std::move(f);
        errorCb_ = std::move(e);
        connectCb_ = std::move(c);
    }

    bool is_open() const { return open_; }

    // ---- 逻辑线程：发起连接 ----
    bool start_connect() {
        if (started_) return true;
        started_ = true;

        SOCKADDR_STORAGE local = addr_;
        if (local.ss_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(&local);
            sin->sin_addr.s_addr = INADDR_ANY;
            sin->sin_port = 0;
        } else {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(&local);
            sin6->sin6_addr = in6addr_any;
            sin6->sin6_port = 0;
        }

        sock_ = WSASocketW(local.ss_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                           WSA_FLAG_OVERLAPPED);
        if (sock_ == INVALID_SOCKET) {
            sync_fail(WSAGetLastError(), "WSASocket");
            return false;
        }
        g_io.track();

        // 关键：把 socket 关联到完成端口，key 用 this。没有这一步，重叠操作
        // 不会向端口投递任何完成包，ConnectEx 之后整条连接静默挂死。
        // SOCKET 在 x64 下是 UINT_PTR（整型），HANDLE 是指针，C++ 模式不做隐式转换；
        // 先规到 uintptr_t 再 reinterpret_cast，避免 C4311/C4302 截断警告。
        const HANDLE sock_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sock_));
        if (!CreateIoCompletionPort(sock_handle, g_io.Port(), (ULONG_PTR)this, 0)) {
            const int e = (int)GetLastError();
            g_io.untrack();
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            started_ = false;
            sync_fail(e, "CreateIoCompletionPort");
            return false;
        }

        // ConnectEx 要求 socket 已 bind
        if (bind(sock_, (SOCKADDR*)&local, (int)sizeof(local)) == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            g_io.untrack();
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            started_ = false;
            sync_fail(e, "bind");
            return false;
        }
        BOOL nodelay = TRUE;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, (int)sizeof(nodelay));
        // 保活交给应用层心跳（--hb-interval），这里不再叠加 TCP keepalive，
        // 否则两套超时互相干扰，定位断连原因时会看不出是谁先踢的。

        Pfn_ConnectEx fn = connect_ex();
        if (!fn) {
            const int e = WSAGetLastError();
            g_io.untrack();
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            started_ = false;
            sync_fail(e ? e : WSAEPROTONOSUPPORT, "ConnectEx");
            return false;
        }

        selfHold_ = shared_from_this();  // 有悬挂 IO 期间保住自己
        pendingOps_ = 1;
        SOCKADDR_STORAGE remote = addr_;
        set_port(remote, g_cfg.tcpPort);
        DWORD sent = 0;
        const BOOL ok = fn(sock_, (SOCKADDR*)&remote, (int)sizeof(remote), nullptr, 0, &sent,
                           &connectOv_.ov);
        if (!ok) {
            const int e = WSAGetLastError();
            if (e != WSA_IO_PENDING) {
                pendingOps_ = 0;
                closeRequested_ = true;
                open_ = false;
                notify_error(e, "ConnectEx");
                reap_if_idle();
                return false;
            }
        }
        return true;
    }

    // ---- 逻辑线程：投递一帧（含 4 字节长度头）----
    void send_raw(std::vector<char>&& framed) {
        if (!open_ || reaped_) {
            notify_error(WSAENOTCONN, "send on closed socket");
            return;
        }
        if (queuedBytes_ + framed.size() > g_cfg.maxQueuedBytes) {
            notify_error(WSAENOBUFS, "send queue overflow");
            return;
        }
        queuedBytes_ += framed.size();
        outq_.push_back(std::move(framed));
        if (!sending_) pump_send();
    }

    // ---- 逻辑线程：主动正常关闭 ----
    void request_close() {
        if (reaped_) return;
        if (!open_ && closeRequested_) return;
        open_ = false;
        closeRequested_ = true;
        if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_BOTH);
        reap_if_idle();
    }

    // ---- 完成事件入口（由 IOCP 线程转投，运行在逻辑线程）----
    void handle(IoOp op, DWORD bytes, int wsaErr) {
        // 处理过程中可能触发 reap，这里先拿一份强引用保证 this 存活
        const std::shared_ptr<TcpConn> keep = selfHold_;
        switch (op) {
            case IoOp::Connect: done_connect(wsaErr); break;
            case IoOp::Recv:    done_recv(bytes, wsaErr); break;
            case IoOp::Send:    done_send(bytes, wsaErr); break;
        }
        (void)keep;  // 仅用于延长生命期到本函数返回
    }

private:
    static void clear_ov(LPOVERLAPPED o) { std::memset(o, 0, sizeof(OVERLAPPED)); }

    static Pfn_ConnectEx connect_ex() {
        static Pfn_ConnectEx cached = nullptr;
        static bool tried = false;
        if (tried) return cached;
        tried = true;
        // 自带 GUID，避免不同 SDK 下 WSAID_CONNECTEX 的声明/链接差异
        static const GUID kGuid = {0x25a207b9, 0xddf3, 0x466c,
                                   {0x8e, 0xe9, 0x76, 0xe8, 0xb8, 0xc5, 0x70, 0x01}};
        SOCKET probe = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                  WSA_FLAG_OVERLAPPED);
        if (probe == INVALID_SOCKET) return nullptr;
        GUID guid = kGuid;
        Pfn_ConnectEx fn = nullptr;
        DWORD returned = 0;
        const DWORD rc = WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid,
                                  (DWORD)sizeof(guid), &fn, (DWORD)sizeof(fn), &returned,
                                  nullptr, nullptr);
        closesocket(probe);
        cached = (rc == 0) ? fn : nullptr;
        return cached;
    }

    void post_recv() {
        ULONG want = 0;
        if (readPhase_ == ReadPhase::Header) {
            want = 4 - hdrGot_;
            recvBuf_.buf = hdr_ + hdrGot_;
        } else {
            want = bodyNeed_ - bodyGot_;
            recvBuf_.buf = body_.data() + bodyGot_;
        }
        recvBuf_.len = want;
        recvFlags_ = 0;
        clear_ov(&recvOv_.ov);
        pendingOps_ += 1;
        const int rc = WSARecv(sock_, &recvBuf_, 1, nullptr, &recvFlags_, &recvOv_.ov, nullptr);
        if (rc == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e != WSA_IO_PENDING) {
                pendingOps_ -= 1;
                open_ = false;
                closeRequested_ = true;
                notify_error(e, "WSARecv");
            }
        }
    }

    void pump_send() {
        while (!outq_.empty() && !sending_) {
            sendBuf_ = std::move(outq_.front());
            outq_.pop_front();
            queuedBytes_ -= sendBuf_.size();
            sendWsa_.buf = sendBuf_.data();
            sendWsa_.len = (ULONG)sendBuf_.size();
            clear_ov(&sendOv_.ov);
            pendingOps_ += 1;
            sending_ = true;
            const int rc = WSASend(sock_, &sendWsa_, 1, nullptr, 0, &sendOv_.ov, nullptr);
            if (rc == SOCKET_ERROR) {
                const int e = WSAGetLastError();
                if (e != WSA_IO_PENDING) {
                    pendingOps_ -= 1;
                    sending_ = false;
                    open_ = false;
                    closeRequested_ = true;
                    notify_error(e, "WSASend");
                    return;
                }
            }
            g_framesSent.fetch_add(1);
            g_bytesSent.fetch_add(sendBuf_.size());
            return;  // 一次只挂一个发送，保序
        }
    }

    void done_connect(int wsaErr) {
        pendingOps_ -= 1;
        if (wsaErr != 0) {
            open_ = false;
            closeRequested_ = true;
            notify_error(wsaErr, "connect");
            reap_if_idle();
            return;
        }
        setsockopt(sock_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
        open_ = true;
        post_recv();
        if (connectCb_) connectCb_(0);
        reap_if_idle();
    }

    void done_recv(DWORD bytes, int wsaErr) {
        pendingOps_ -= 1;
        if (!open_) {  // 关闭后的收尾完成，忽略
            reap_if_idle();
            return;
        }
        if (wsaErr != 0) {
            open_ = false;
            closeRequested_ = true;
            notify_error(wsaErr, "recv");
            reap_if_idle();
            return;
        }
        if (bytes == 0) {  // 对端 FIN：长连接被服务端断开
            open_ = false;
            closeRequested_ = true;
            notify_error(0, "peer closed");
            reap_if_idle();
            return;
        }
        g_bytesRecv.fetch_add(bytes);
        feed_input(bytes);
        if (open_) post_recv();
        reap_if_idle();
    }

    void done_send(DWORD bytes, int wsaErr) {
        pendingOps_ -= 1;
        sending_ = false;
        sendBuf_.clear();
        (void)bytes;
        if (wsaErr != 0) {
            if (open_) {
                open_ = false;
                closeRequested_ = true;
                notify_error(wsaErr, "send");
            }
            reap_if_idle();
            return;
        }
        if (open_) pump_send();
        reap_if_idle();
    }

    // 增量解析：4 字节头 -> 定长体 -> 交付 -> 复位
    void feed_input(DWORD bytes) {
        if (readPhase_ == ReadPhase::Header) {
            hdrGot_ += bytes;
            if (hdrGot_ < 4) return;
            u_long be = 0;
            std::memcpy(&be, hdr_, 4);
            const uint32_t len = (uint32_t)ntohl(be);
            if (len == 0 || len > g_cfg.maxFrameBytes) {
                open_ = false;
                closeRequested_ = true;
                notify_error(WSAEMSGSIZE, "bad frame length");
                return;
            }
            bodyNeed_ = len;
            bodyGot_ = 0;
            if (body_.size() != len) body_.resize(len);
            readPhase_ = ReadPhase::Body;
            if (len == 0) return;  // 上面已拦
            // 头与体可能在同一次完成里？不会：这里只收满了头，需要再 post 一次
            return;
        }
        bodyGot_ += bytes;
        if (bodyGot_ < bodyNeed_) return;
        readPhase_ = ReadPhase::Header;
        hdrGot_ = 0;
        bodyNeed_ = 0;
        bodyGot_ = 0;
        g_framesRecv.fetch_add(1);
        if (frameCb_) frameCb_(body_);  // 就地解析，零拷贝
    }

    void sync_fail(int err, const char* stage) {
        started_ = false;
        notify_error(err, stage);
    }

    void notify_error(int err, const char* stage) {
        auto cb = errorCb_;
        if (cb) cb(err, stage);
    }

    void reap_if_idle() {
        if (reaped_) return;
        if (pendingOps_ > 0) return;
        if (!closeRequested_ && open_) return;
        reaped_ = true;
        open_ = false;
        sending_ = false;
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            g_io.untrack();
        }
        outq_.clear();
        sendBuf_.clear();
        body_.clear();
        queuedBytes_ = 0;
        selfHold_.reset();
    }

    enum class ReadPhase : uint32_t { Header = 0, Body = 1 };

    SOCKET sock_ = INVALID_SOCKET;
    SOCKADDR_STORAGE addr_{};
    bool started_ = false;
    bool open_ = false;
    bool closeRequested_ = false;
    bool reaped_ = false;
    int pendingOps_ = 0;

    ConnOverlapped connectOv_{}, recvOv_{}, sendOv_;
    WSABUF recvBuf_{};
    ULONG recvFlags_ = 0;

    WSABUF sendWsa_{};
    bool sending_ = false;
    std::vector<char> sendBuf_;
    std::deque<std::vector<char>> outq_;
    uint64_t queuedBytes_ = 0;

    char hdr_[4]{};
    uint32_t hdrGot_ = 0;
    std::vector<char> body_;
    uint32_t bodyNeed_ = 0, bodyGot_ = 0;
    ReadPhase readPhase_ = ReadPhase::Header;

    std::shared_ptr<TcpConn> selfHold_;
    FrameCb frameCb_;
    ErrorCb errorCb_;
    ConnectCb connectCb_;
};

// IOCP 工作线程：只取完成包，把 (conn, op, bytes, err) 转投逻辑线程
void IoLoop::worker() {
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(port_, &bytes, &key, &ov, 1000);
        const DWORD err = ok ? 0 : GetLastError();
        if (!ok && err == WAIT_TIMEOUT && ov == nullptr && key == 0) continue;
        if (ov == nullptr && key == 0) break;  // 停机信号
        if (!ov || !key) {
            if (!ok && err == WAIT_TIMEOUT) continue;
            break;
        }
        const IoOp op = reinterpret_cast<ConnOverlapped*>(ov)->op;
        auto* conn = reinterpret_cast<TcpConn*>(key);
        const int wsaErr = ok ? 0 : (int)err;
        const DWORD n = bytes;
        Logic::I().post([conn, op, n, wsaErr] { conn->handle(op, n, wsaErr); });
    }
}

// ---------------------------------------------------------------------------
// HTTP：短连接（Connection: close），跑独立线程池，回调回到逻辑线程
// ---------------------------------------------------------------------------
struct HttpResponse {
    int status = 0;
    std::string body;
    int retryAfter = 0;
    std::string err;  // 非空表示传输层失败（status==0）
};

HttpResponse http_call_once(const std::string& method, const std::string& path,
                            const std::string& token, const std::string& body) {
    HttpResponse out;
    g_httpCalls.fetch_add(1);
    SOCKADDR_STORAGE addr = g_targetAddr;
    set_port(addr, g_cfg.httpPort);
    const SOCKET s = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        out.err = "socket: " + wsa_err_str(WSAGetLastError());
        return out;
    }
    DWORD tv = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, (int)sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, (int)sizeof(tv));
    if (connect(s, (SOCKADDR*)&addr, (int)sizeof(addr)) == SOCKET_ERROR) {
        out.err = "connect http: " + wsa_err_str(WSAGetLastError());
        closesocket(s);
        return out;
    }
    std::string req;
    req += method;
    req += " ";
    req += path;
    req += " HTTP/1.1\r\n";
    req += "Host: " + g_cfg.host + ":" + std::to_string(g_cfg.httpPort) + "\r\n";
    req += "Connection: close\r\n";
    req += "Content-Type: application/json\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    req += body;

    size_t off = 0;
    bool failed = false;
    while (off < req.size()) {
        const int n = send(s, req.data() + off, (int)(req.size() - off), 0);
        if (n <= 0) {
            out.err = "send http: " + wsa_err_str(WSAGetLastError());
            failed = true;
            break;
        }
        off += (size_t)n;
    }
    std::string raw;
    if (!failed) {
        char buf[8192];
        for (;;) {
            const int n = recv(s, buf, (int)sizeof(buf), 0);
            if (n > 0) {
                raw.append(buf, (size_t)n);
                if (raw.size() > 64u * 1024u * 1024u) break;  // 防御
                continue;
            }
            if (n == 0) break;
            const int e = WSAGetLastError();
            out.err = (e == WSAETIMEDOUT) ? "http recv timeout" : ("recv http: " + wsa_err_str(e));
            break;
        }
    }
    closesocket(s);
    if (raw.empty()) {
        if (out.err.empty()) out.err = "empty http response";
        return out;
    }
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        out.err = "bad http response: no header end";
        return out;
    }
    out.body = raw.substr(headerEnd + 4);
    const size_t sp = raw.find(' ');
    if (sp != std::string::npos) {
        try {
            out.status = std::stoi(raw.substr(sp + 1, 3));
        } catch (...) {}
    }
    // Retry-After 字段名大小写不敏感，不能依赖服务端拼写
    std::string lower;
    lower.reserve(headerEnd);
    for (size_t i = 0; i < headerEnd; ++i)
        lower.push_back((char)tolower((unsigned char)raw[i]));
    const size_t p = lower.find("retry-after:");
    if (p != std::string::npos) {
        try {
            out.retryAfter = std::stoi(raw.substr(p + 12, 6));
        } catch (...) {}
    }
    return out;
}

using HttpCb = std::function<void(HttpResponse)>;

class HttpPool {
public:
    void start(unsigned threads) {
        unsigned n = threads;
        if (!n) {
            int v = g_cfg.friends / 16;
            v = (std::max)(16, (std::min)(256, v));
            n = (unsigned)v;
        }
        workers_ = n;
        for (unsigned i = 0; i < workers_; ++i) threads_.emplace_back([this] { worker(); });
    }
    void submit(std::string method, std::string path, std::string token, std::string body,
                HttpCb cb) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_) return;
            q_.push(Job{std::move(method), std::move(path), std::move(token), std::move(body),
                        std::move(cb)});
        }
        cv_.notify_one();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
        }
        cv_.notify_all();
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
    }
    unsigned workers() const { return workers_; }

private:
    struct Job {
        std::string method, path, token, body;
        HttpCb cb;
    };
    void worker() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return !running_ || !q_.empty(); });
                if (!running_ && q_.empty()) return;
                j = std::move(q_.front());
                q_.pop();
            }
            HttpResponse r = http_call_once(j.method, j.path, j.token, j.body);
            Logic::I().post([cb = std::move(j.cb), r = std::move(r)]() mutable {
                if (cb) cb(std::move(r));
            });
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<Job> q_;
    bool running_ = true;
    unsigned workers_ = 0;
    std::vector<std::thread> threads_;
};
HttpPool g_http;

// 429/503 是"稍后再试"，不是业务失败：按 Retry-After 退避（缺省 1s），最多 attempts 次。
// 退避等待由定时器承担（不阻塞任何线程），但延迟采样仍包含退避时间——这是对的。
void http_retry(std::string method, std::string path, std::string token, std::string body,
                std::function<void(HttpResponse)> done, int attempts = 4, int attempt = 1) {
    g_http.submit(method, path, token, body,
                  [method, path, token, body, done, attempts,
                   attempt](HttpResponse&& rr) mutable {
                      HttpResponse r = std::move(rr);
                      const bool retriable = (r.status == 429 || r.status == 503) &&
                                             (attempt < attempts) && !g_stop.load();
                      if (!retriable) {
                          if (done) done(std::move(r));
                          return;
                      }
                      const int waitSec =
                          r.retryAfter > 0 ? (std::min)(r.retryAfter, 5) : 1;
                      const int64_t delay = (int64_t)waitSec * 1000 + (attempt * 137) % 400;
                      Logic::I().later(delay, [method, path, token, body, done, attempts,
                                               attempt]() mutable {
                          http_retry(method, path, token, body, done, attempts, attempt + 1);
                      });
                  });
}

// 主线程编排用的同步版本（只跑在 main 上，不参与并发模型）
HttpResponse http_sync(const std::string& method, const std::string& path,
                       const std::string& token, const std::string& body) {
    HttpResponse r = http_call_once(method, path, token, body);
    for (int a = 1; a < 4 && (r.status == 429 || r.status == 503); ++a) {
        const int waitSec = r.retryAfter > 0 ? (std::min)(r.retryAfter, 5) : 1;
        Sleep((DWORD)(waitSec * 1000 + (a * 137) % 400));
        r = http_call_once(method, path, token, body);
    }
    return r;
}

// ---------------------------------------------------------------------------
// JSON 小封装（与原版一致）
// ---------------------------------------------------------------------------
rapidjson::Document parse_json(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject())
        throw std::runtime_error("bad json: " + text);
    return doc;
}
std::string json_string(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key) || !v[key].IsString()) return {};
    return std::string(v[key].GetString(), v[key].GetStringLength());
}
std::uint64_t json_uint(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    if (v[key].IsUint64()) return v[key].GetUint64();
    if (v[key].IsInt64() && v[key].GetInt64() > 0)
        return (std::uint64_t)v[key].GetInt64();
    return 0;
}

// ---------------------------------------------------------------------------
// 延迟采样与分位数
// ---------------------------------------------------------------------------
using LatencySamples = std::vector<std::int64_t>;  // 微秒
struct LatGroup {
    LatencySamples reg, login, request, accept, send, recv, list, del;
};
void merge_append(LatencySamples& dst, const LatencySamples& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
struct Percentiles {
    double p50 = 0, p95 = 0, p99 = 0, avg = 0;
    std::int64_t max = 0;
};
Percentiles calc(const LatencySamples& in) {
    if (in.empty()) return {};
    LatencySamples v = in;
    std::sort(v.begin(), v.end());
    auto at = [&](double p) {
        size_t i = (size_t)std::llround((p / 100.0) * (v.size() - 1));
        return (double)v[i] / 1000.0;
    };
    double sum = 0;
    for (auto x : v) sum += (double)x;
    return {at(50), at(95), at(99), sum / (double)v.size() / 1000.0, v.back() / 1000};
}
void print_latency(const char* name, const LatencySamples& s) {
    const Percentiles p = calc(s);
    std::cout << std::left << std::setw(18) << name << " : " << std::fixed
              << std::setprecision(2) << p.p50 << " / " << p.p95 << " / " << p.p99 << " / "
              << p.avg << " / " << (double)p.max << " ms  (n=" << s.size() << ")\n";
}

// ---------------------------------------------------------------------------
// 会话基类：拥有一条 TcpConn、负责帧的解析派发与应用层心跳
// ---------------------------------------------------------------------------
class Session : public IHb {
public:
    ~Session() override {
        if (conn_) conn_->request_close();
    }

    void connect_tcp() {
        conn_ = std::make_shared<TcpConn>(g_targetAddr);
        conn_->set_callbacks(
            [this](std::vector<char>& body) { on_body(body); },
            [this](int err, const char* stage) { on_conn_error(err, stage); },
            [this](int err) { on_connected(err); });
        conn_->start_connect();
    }

protected:
    virtual void on_message(const kc::Message& m) = 0;
    virtual void on_conn_error(int wsaErr, const char* stage) = 0;
    virtual void on_connected(int wsaErr) = 0;

    // 就地解析（零拷贝）后派发给子类；解析失败视为协议错误
    void on_body(std::vector<char>& body) {
        kc::Message m;
        std::string err;
        if (!kc::parse_message(body, m, err)) {
            protocol_error("bad server message: " + err);
            return;
        }
        on_message(m);
    }

    void send_msg(const kc::Message& m) {
        if (!conn_) {
            protocol_error("send without connection");
            return;
        }
        std::string payload = kc::serialize_message(m);
        conn_->send_raw(encode_frame(payload));
    }

    void start_hb() {
        hbActive_ = true;
        // 按 idx 打散到期时间，避免 N 条连接在同一秒集中发心跳
        hbDueMs_ = tick_ms() + (int64_t)g_cfg.hbIntervalSec * 1000 +
                   (int64_t)((idx_ % 1000 + 1000) % 1000);
    }
    void stop_hb() { hbActive_ = false; }

    void hb_tick(int64_t nowMs) override {
        if (!hbActive_ || finished_) return;
        if (nowMs < hbDueMs_) return;
        hbDueMs_ = nowMs + (int64_t)g_cfg.hbIntervalSec * 1000;
        kc::Message hb;
        hb.from = uid_;
        hb.type = kc::MsgType::Heartbeat;
        hb.content = "";
        send_msg(hb);  // 失败会走 on_conn_error
    }

    virtual void protocol_error(const std::string& what) = 0;

    std::shared_ptr<TcpConn> conn_;
    uint64_t uid_ = 0;
    std::string token_;
    int idx_ = 0;
    bool hbActive_ = false;
    bool finished_ = false;  // 生命周期已结束：忽略后续连接事件与定时器
    int64_t hbDueMs_ = 0;
};

// ---------------------------------------------------------------------------
// 好友会话状态机（替代原版的 friend_worker 线程）
// ---------------------------------------------------------------------------
class FriendAgent : public Session {
public:
    enum class St : uint8_t {
        Init, Register, Login, TcpConnect, WaitLoginAck,
        WaitP1, Accept, WaitP2, WaitSendAck, WaitReply,
        WaitP3, List, WaitP4, Delete, Done, Failed
    };

    FriendAgent(int idx, uint64_t runId, uint64_t mainUid) {
        idx_ = idx;
        mainUid_ = mainUid;
        user_ = "s3F_" + std::to_string(runId) + "_" + std::to_string(idx);
    }

    // ---- 由主线程/逻辑线程读取的发布值 ----
    std::atomic<uint64_t> uidPub_{0};
    std::atomic<uint64_t> reqId_{0};
    LatGroup lat;
    bool ok = false;
    std::string err;

    void start() {
        if (g_stop.load()) return;
        setState(St::Register);
        t0_ = now_us();
        const std::string body = "{\"username\":\"" + user_ + "\",\"password\":\"secret123\"}";
        http_retry("POST", "/api/register", "", body,
                   [this](HttpResponse r) { after_register(std::move(r)); });
    }

    // 好友申请由主用户创建，主线程拿到 requestId 后调用（逻辑线程）
    void accept_request() {
        if (finished_) return;
        setState(St::Accept);
        t0_ = now_us();
        const std::string path =
            "/api/friends/requests/" + std::to_string(reqId_.load()) + "/accept";
        http_retry("POST", path, token_, "", [this](HttpResponse r) {
            lat.accept.push_back(now_us() - t0_);
            if (!check_http(r, "accept friend request")) return;
            g_accepted.fetch_add(1);
            TRACE("friend " << idx_ << " accepted");
            wait_phase(2, St::WaitP2, [this] { send_text(); }, "phase 2 (messaging)");
        });
    }

    void fail(const std::string& why) {
        if (finished_) return;
        finished_ = true;
        stop_hb();
        ok = false;
        err = why;
        setState(St::Failed);
        mark_failed(why);
        TRACE("friend " << idx_ << " FAILED: " << why);
        if (conn_) conn_->request_close();
    }

    void close_now() {
        if (conn_) conn_->request_close();
    }

private:
    uint64_t mainUid_ = 0;
    std::string user_;
    St st_ = St::Init;
    uint32_t gen_ = 0;
    int64_t t0_ = 0;
    int connectTry_ = 0;
    int sendTry_ = 0;
    kc::Message pendingText_;  // 重试用同一 msg_id
    kc::Message earlyReply_;   // 早于 SendAck 到达的回复
    bool earlyReplyValid_ = false;

    void setState(St s) {
        st_ = s;
        ++gen_;  // 作废上一状态挂的超时
    }

    void arm_timeout(int64_t ms, St expect, const char* what) {
        const uint32_t g = gen_;
        Logic::I().later(ms, [this, g, expect, what] {
            if (finished_ || g != gen_ || st_ != expect) return;
            on_wait_timeout(expect, what);
        });
    }

    void on_wait_timeout(St expect, const char* what) {
        if (expect == St::WaitSendAck) {
            retry_send(std::string(what) + " timeout");
            return;
        }
        fail(std::string(what) + " timeout");
    }

    bool check_http(const HttpResponse& r, const std::string& what) {
        if (!r.err.empty() && r.status == 0) {
            fail(std::string(what) + " failed: " + r.err);
            return false;
        }
        if (r.status != 200) {
            fail(std::string(what) + " failed: HTTP " + std::to_string(r.status) + " " +
                 r.body);
            return false;
        }
        return true;
    }

    void wait_phase(int target, St waitSt, std::function<void()> next, const char* name) {
        setState(waitSt);
        Logic::I().wait_phase(
            target, [this, next, waitSt]() {
                if (finished_ || g_stop.load()) return;
                if (st_ != waitSt) return;
                next();
            },
            [this, waitSt, name]() {
                if (finished_ || st_ != waitSt) return;
                fail(std::string("aborted before ") + name);
            },
            (int64_t)g_cfg.timeoutSec * 1000);
    }

    // ---- HTTP 回调 ----
    void after_register(HttpResponse r) {
        lat.reg.push_back(now_us() - t0_);
        if (!check_http(r, "register " + user_)) return;
        setState(St::Login);
        t0_ = now_us();
        const std::string body = "{\"username\":\"" + user_ + "\",\"password\":\"secret123\"}";
        http_retry("POST", "/api/login", "", body,
                   [this](HttpResponse rr) { after_login(std::move(rr)); });
    }

    void after_login(HttpResponse r) {
        lat.login.push_back(now_us() - t0_);
        if (!check_http(r, "login " + user_)) return;
        uint64_t uid = 0;
        std::string token;
        try {
            rapidjson::Document d = parse_json(r.body);
            if (!d.HasMember("data") || !d["data"].IsObject())
                throw std::runtime_error("login missing data");
            uid = json_uint(d["data"], "userId");
            token = json_string(d["data"], "token");
        } catch (const std::exception& e) {
            fail(std::string("login json: ") + e.what() + " | " + r.body);
            return;
        }
        if (!uid || token.empty()) {
            fail("login missing userId/token: " + r.body);
            return;
        }
        uid_ = uid;
        token_ = token;
        uidPub_.store(uid);
        connectTry_ = 0;
        setState(St::TcpConnect);
        connect_tcp();
    }

    // ---- 连接事件 ----
    void on_connected(int wsaErr) override {
        if (finished_) return;
        if (wsaErr != 0) {  // 理论上 0 才会调用，兜底
            on_conn_error(wsaErr, "connect");
            return;
        }
        setState(St::WaitLoginAck);
        kc::Message m;
        m.from = uid_;
        m.type = kc::MsgType::Login;
        m.content = token_;
        send_msg(m);
        arm_timeout(10000, St::WaitLoginAck, "tcp login ack");
    }

    void on_conn_error(int wsaErr, const char* stage) override {

        if (finished_) return;  // 正常收尾后的迟到事件
        // 建连/TCP 登录阶段的失败可以重试；业务阶段的断连直接判失败
        if (st_ == St::TcpConnect || st_ == St::WaitLoginAck) {
            ++connectTry_;
            if (connectTry_ < g_cfg.connectAttempts) {
                TRACE("friend " << idx_ << " tcp " << stage << " retry " << connectTry_ << ": "
                                << wsa_err_str(wsaErr));
                conn_.reset();  // 旧对象由 selfHold_ 保证安全析构
                setState(St::TcpConnect);
                Logic::I().later(300, [this] {
                    if (finished_ || st_ != St::TcpConnect) return;
                    connect_tcp();
                });
                return;
            }
            fail(std::string("tcp login failed: ") + stage + ": " + wsa_err_str(wsaErr));
            return;
        }
        if (st_ == St::Delete || st_ == St::Done) return;  // 删除后的关闭是预期的
        fail(std::string("connection lost at ") + stage + ": " + wsa_err_str(wsaErr));
    }

    void protocol_error(const std::string& what) override {
        fail("friend " + std::to_string(idx_) + ": " + what);
    }

    // ---- 服务端消息派发 ----
    void on_message(const kc::Message& m) override {
        if (finished_) return;
        switch (st_) {
            case St::WaitLoginAck:
                if (m.type == kc::MsgType::LoginAck) {
                    ++gen_;  // 取消登录 ack 超时
                    start_hb();
                    g_ready.fetch_add(1);
                    TRACE("friend " << idx_ << " TCP ready");
                    wait_phase(1, St::WaitP1, [this] { accept_request(); },
                               "phase 1 (accept)");
                } else if (m.type == kc::MsgType::Error) {
                    fail("login error: " + m.content);
                }
                return;

            case St::WaitSendAck:
                if (m.type == kc::MsgType::SendAck) {
                    lat.send.push_back(now_us() - t0_);
                    setState(St::WaitReply);
                    t0_ = now_us();
                    arm_timeout((int64_t)g_cfg.timeoutSec * 1000, St::WaitReply,
                                "wait reply");
                    // 回复比 SendAck 先到时先暂存，这里立刻消费，不再死等
                    if (earlyReplyValid_) {
                        kc::Message r = earlyReply_;
                        earlyReplyValid_ = false;
                        handle_reply_text(r);
                    }
                } else if (m.type == kc::MsgType::Text) {
                    earlyReply_ = m;  // 乱序兜底：暂存等 SendAck
                    earlyReplyValid_ = true;
                } else if (m.type == kc::MsgType::Error) {
                    retry_send("server error: " + m.content);
                }
                return;

            case St::WaitReply:
                if (m.type == kc::MsgType::Text) {
                    handle_reply_text(m);
                } else if (m.type == kc::MsgType::Error) {
                    fail("wait reply: server error: " + m.content);
                }
                return;

            default:
                // 与原版一致：非本阶段关注的事件（心跳 ACK、好友事件等）丢弃
                return;
        }
    }

    void handle_reply_text(const kc::Message& m) {
        const std::string expect = "reply-" + std::to_string(idx_);
        if (m.content != expect) {
            fail("reply mismatch: got '" + m.content + "', expected '" + expect + "'");
            return;
        }
        if (st_ == St::WaitReply) lat.recv.push_back(now_us() - t0_);
        kc::Message ack;
        ack.from = uid_;
        ack.type = kc::MsgType::Ack;
        ack.conversation_id = m.conversation_id;
        ack.seq = m.seq;
        ack.content = "delivered";
        send_msg(ack);
        g_messaged.fetch_add(1);
        TRACE("friend " << idx_ << " messaged");
        wait_phase(3, St::WaitP3, [this] { list_friends(); }, "phase 3 (list)");
    }

    // ---- 阶段 2：发文本（带重试） ----
    void send_text() {
        if (g_stop.load()) {
            fail("aborted before messaging");
            return;
        }
        setState(St::WaitSendAck);
        sendTry_ = 1;
        t0_ = now_us();
        pendingText_.from = uid_;
        pendingText_.to = mainUid_;
        pendingText_.type = kc::MsgType::Text;
        pendingText_.msg_id = 1000000ULL + (uint64_t)idx_;
        pendingText_.content = "hello-" + std::to_string(idx_);
        send_msg(pendingText_);
        arm_timeout((int64_t)(std::min)(g_cfg.timeoutSec, 30) * 1000, St::WaitSendAck,
                    "send ack");
    }

    void retry_send(const std::string& why) {
        if (finished_) return;
        if (g_stop.load() || sendTry_ >= 3) {
            fail("send text failed after " + std::to_string(sendTry_) + " attempts: " + why);
            return;
        }
        ++sendTry_;
        g_sendRetries.fetch_add(1);
        TRACE("retry send msg_id=" << pendingText_.msg_id << " (" << sendTry_ << "/3): " << why);
        setState(St::WaitSendAck);
        Logic::I().later(100, [this] {
            if (finished_ || st_ != St::WaitSendAck) return;
            send_msg(pendingText_);
            arm_timeout((int64_t)(std::min)(g_cfg.timeoutSec, 30) * 1000, St::WaitSendAck,
                        "send ack");
        });
    }

    // ---- 阶段 3 ----
    void list_friends() {
        if (g_stop.load()) {
            fail("aborted before list");
            return;
        }
        setState(St::List);
        t0_ = now_us();
        http_retry("GET", "/api/friends", token_, "", [this](HttpResponse r) {
            lat.list.push_back(now_us() - t0_);
            if (!check_http(r, "list friends")) return;
            g_listed.fetch_add(1);
            TRACE("friend " << idx_ << " listed");
            wait_phase(4, St::WaitP4, [this] { delete_main(); }, "phase 4 (delete)");
        });
    }

    // ---- 阶段 4 ----
    void delete_main() {
        if (g_stop.load()) {
            fail("aborted before delete");
            return;
        }
        setState(St::Delete);
        t0_ = now_us();
        const std::string path = "/api/friends/" + std::to_string(mainUid_);
        http_retry("DELETE", path, token_, "", [this](HttpResponse r) {
            lat.del.push_back(now_us() - t0_);
            if (!check_http(r, "delete friend")) return;
            g_deleted.fetch_add(1);
            ok = true;
            finished_ = true;   // 之后连接上的事件全部忽略
            stop_hb();
            setState(St::Done);
            TRACE("friend " << idx_ << " deleted, closing session");
            if (conn_) conn_->request_close();
        });
    }
};

// ---------------------------------------------------------------------------
// 主用户会话：收 N 条文本 -> 串行回复（等 SendAck）-> 全部落地后补发投递 ACK
// ---------------------------------------------------------------------------
class MainAgent : public Session {
public:
    explicit MainAgent(std::unordered_map<uint64_t, int> uidToIdx)
        : uidToIdx_(std::move(uidToIdx)) {}

    void begin() {
        idx_ = -1;
        connect_tcp();
    }

    void fail(const std::string& why) {
        if (finished_) return;
        finished_ = true;
        stop_hb();
        mark_failed("main user: " + why);
        LOGX("main user FAILED: " << why);
        if (conn_) conn_->request_close();
    }

    // 由主线程投递到逻辑线程调用：配置身份并发起 TCP 登录
    void configure(uint64_t uid, const std::string& token, int expected) {
        uid_ = uid;
        token_ = token;
        expected_ = (size_t)expected;
    }
    // 主线程在阶段 2 之前把 uid -> idx 映射交给逻辑线程（过滤未知发送者 + 生成回复正文）
    void install_index(const std::unordered_map<uint64_t, int>& m) { uidToIdx_ = m; }
    void close_now() { if (conn_) conn_->request_close(); }

private:
    enum class St : uint8_t { TcpConnect, WaitLoginAck, Collect, WaitReplyAck, Flushed, Done,
                              Failed };
    St st_ = St::TcpConnect;
    uint32_t gen_ = 0;
    std::unordered_map<uint64_t, int> uidToIdx_;
    std::deque<kc::Message> texts_;      // 已收、待回复
    std::vector<kc::Message> allTexts_;  // 收齐后统一补发投递 ACK
    size_t received_ = 0;
    size_t expected_ = 0;
    bool pumping_ = false;
    int replyTry_ = 0;
    kc::Message replyMsg_;

    void setState(St s) {
        st_ = s;
        ++gen_;
    }

    void on_connected(int wsaErr) override {
        if (finished_) return;
        if (wsaErr != 0) {
            on_conn_error(wsaErr, "connect");
            return;
        }
        setState(St::WaitLoginAck);
        kc::Message m;
        m.from = uid_;
        m.type = kc::MsgType::Login;
        m.content = token_;
        send_msg(m);
        const uint32_t g = gen_;
        Logic::I().later(10000, [this, g] {
            if (finished_ || g != gen_ || st_ != St::WaitLoginAck) return;
            fail("tcp login ack timeout");
        });
    }

    void on_conn_error(int wsaErr, const char* stage) override {
        if (finished_) return;
        if (st_ == St::TcpConnect || st_ == St::WaitLoginAck) {
            ++connectTry_;
            if (connectTry_ < g_cfg.connectAttempts) {
                LOGX("main tcp " << stage << " retry " << connectTry_ << ": "
                                 << wsa_err_str(wsaErr));
                conn_.reset();
                setState(St::TcpConnect);
                Logic::I().later(300, [this] {
                    if (finished_ || st_ != St::TcpConnect) return;
                    connect_tcp();
                });
                return;
            }
            fail(std::string("tcp login failed: ") + stage + ": " + wsa_err_str(wsaErr));
            return;
        }
        if (st_ == St::Flushed || st_ == St::Done) return;
        fail(std::string("connection lost at ") + stage + ": " + wsa_err_str(wsaErr));
    }

    void protocol_error(const std::string& what) override { fail(what); }

    void on_message(const kc::Message& m) override {
        if (finished_) return;
        switch (st_) {
            case St::WaitLoginAck:
                if (m.type == kc::MsgType::LoginAck) {
                    ++gen_;
                    start_hb();
                    setState(St::Collect);
                    g_mainReady.fetch_add(1);
                    LOGX("main user TCP ready, userId=" << uid_);
                } else if (m.type == kc::MsgType::Error) {
                    fail("login error: " + m.content);
                }
                return;

            case St::Collect:
            case St::WaitReplyAck:
                if (m.type == kc::MsgType::Text) {
                    on_text(m);
                } else if (m.type == kc::MsgType::SendAck && st_ == St::WaitReplyAck) {
                    on_reply_acked();
                } else if (m.type == kc::MsgType::Error && st_ == St::WaitReplyAck) {
                    retry_reply("server error: " + m.content);
                }
                return;

            default:
                return;
        }
    }

    void on_text(const kc::Message& m) {
        auto it = uidToIdx_.find(m.from);
        if (it == uidToIdx_.end()) {
            fail("text from unknown user: " + std::to_string(m.from));
            return;
        }
        texts_.push_back(m);
        allTexts_.push_back(m);
        ++received_;
        if (!pumping_) pump_reply();
    }

    // 串行回复：一次只等一条 SendAck，避免在同一连接上并发写多行会话
    void pump_reply() {
        if (texts_.empty()) {
            pumping_ = false;
            if (received_ >= expected_) flush_acks();
            return;
        }
        pumping_ = true;
        const kc::Message& src = texts_.front();
        const int idx = uidToIdx_[src.from];
        replyMsg_ = kc::Message();
        replyMsg_.from = uid_;
        replyMsg_.to = src.from;
        replyMsg_.type = kc::MsgType::Text;
        replyMsg_.msg_id = 2000000ULL + (uint64_t)idx;
        replyMsg_.content = "reply-" + std::to_string(idx);
        replyTry_ = 1;
        setState(St::WaitReplyAck);
        send_msg(replyMsg_);
        arm_reply_timeout();
    }

    void arm_reply_timeout() {
        const uint32_t g = gen_;
        Logic::I().later((int64_t)(std::min)(g_cfg.timeoutSec, 30) * 1000, [this, g] {
            if (finished_ || g != gen_ || st_ != St::WaitReplyAck) return;
            retry_reply("send ack timeout");
        });
    }

    void on_reply_acked() {
        texts_.pop_front();
        g_replyAcks.fetch_add(1);
        pump_reply();
    }

    void retry_reply(const std::string& why) {
        if (finished_) return;
        if (g_stop.load() || replyTry_ >= 3) {
            fail("reply failed after " + std::to_string(replyTry_) + " attempts: " + why);
            return;
        }
        ++replyTry_;
        g_sendRetries.fetch_add(1);
        TRACE("retry reply msg_id=" << replyMsg_.msg_id << " (" << replyTry_ << "/3): " << why);
        setState(St::WaitReplyAck);
        Logic::I().later(100, [this] {
            if (finished_ || st_ != St::WaitReplyAck) return;
            send_msg(replyMsg_);
            arm_reply_timeout();
        });
    }

    // 所有回复落地后，统一补发投递 ACK（保持原版落库顺序，规避 MariaDB 1020）
    void flush_acks() {
        if (st_ == St::Flushed || st_ == St::Done) return;
        for (const auto& t : allTexts_) {
            kc::Message ack;
            ack.from = uid_;
            ack.type = kc::MsgType::Ack;
            ack.conversation_id = t.conversation_id;
            ack.seq = t.seq;
            ack.content = "delivered";
            send_msg(ack);
        }
        setState(St::Flushed);
        g_mainFlushed.fetch_add(1);
        LOGX("main replies acked=" << g_replyAcks.load() << ", delivery acks flushed");
    }

    int connectTry_ = 0;
};

// ---------------------------------------------------------------------------
// 全局运行对象（逻辑线程独占访问；容器由主线程在启动前构建、之后只读）
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<FriendAgent>> g_agents;
MainAgent* g_main = nullptr;
std::atomic<bool> g_agentsClosed{false};

void close_all_sessions() {
    for (auto& a : g_agents) a->close_now();
    if (g_main) g_main->close_now();
}

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------
void print_usage(const char* prog) {
    std::cout
        << "用法: " << prog << " [选项]\n"
        << "  --host HOST           服务器地址（默认 127.0.0.1）\n"
        << "  --http-port PORT      HTTP 端口（默认 8080）\n"
        << "  --tcp-port PORT       TCP 端口（默认 8888）\n"
        << "  --friends N           好友数量 = 长连接数（默认 100，可上千）\n"
        << "  --io-threads N        IOCP 工作线程数（0=CPU 核数）\n"
        << "  --http-threads N      HTTP 线程池大小（0=按 friends 自动）\n"
        << "  --hb-interval SEC     心跳间隔秒（默认 30，需小于服务端 IDLE_TIMEOUT_SEC）\n"
        << "  --connect-attempts N  TCP 建连重试次数（默认 10，间隔 300ms）\n"
        << "  --ramp-up N           每秒启动好友数（默认 500，0=立即全启动）\n"
        << "  --timeout SEC         单阶段超时秒（默认 120）\n"
        << "  --verbose             大规模时也打印逐连接进度\n"
        << "  --help                帮助\n";
}

int parse_int(const std::string& s, const std::string& name) {
    try {
        int v = std::stoi(s);
        if (v < 0) throw std::invalid_argument("neg");
        return v;
    } catch (...) {
        throw std::runtime_error("invalid " + name + ": " + s);
    }
}

}  // namespace

// ===========================================================================
// main：阶段编排（全部在逻辑线程之外由主线程驱动，等待都用计数器，不阻塞连接）
// ===========================================================================
int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(a + " requires value");
                return argv[++i];
            };
            if (a == "--help" || a == "-h") {
                print_usage(argv[0]);
                return 0;
            } else if (a == "--host") g_cfg.host = val();
            else if (a == "--http-port") g_cfg.httpPort = (unsigned short)parse_int(val(), a);
            else if (a == "--tcp-port") g_cfg.tcpPort = (unsigned short)parse_int(val(), a);
            else if (a == "--friends") g_cfg.friends = parse_int(val(), a);
            else if (a == "--io-threads") g_cfg.ioThreads = parse_int(val(), a);
            else if (a == "--http-threads") g_cfg.httpThreads = parse_int(val(), a);
            else if (a == "--hb-interval") g_cfg.hbIntervalSec = parse_int(val(), a);
            else if (a == "--connect-attempts") g_cfg.connectAttempts = parse_int(val(), a);
            else if (a == "--ramp-up") g_cfg.rampUpPerSec = parse_int(val(), a);
            else if (a == "--timeout") g_cfg.timeoutSec = parse_int(val(), a);
            else if (a == "--verbose") g_verbose.store(true);
            else throw std::runtime_error("unknown option: " + a);
        }
        if (g_cfg.friends <= 0) throw std::runtime_error("friends must be > 0");
        if (g_cfg.timeoutSec <= 0) throw std::runtime_error("timeout must be > 0");
        if (g_cfg.hbIntervalSec <= 0) throw std::runtime_error("hb-interval must be > 0");
        if (g_cfg.connectAttempts <= 0) g_cfg.connectAttempts = 1;
    } catch (const std::exception& e) {
        std::cerr << "[stress03iocp] " << e.what() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    // Ctrl+C / 关窗口 / 关机都要能收尾，否则 5000 条长连接会裸奔退出
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[stress03iocp] WSAStartup failed: " << WSAGetLastError() << "\n";
        return 1;
    }

    const uint64_t runId = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) %
    1000000000ULL;
    g_totalFriends.store(g_cfg.friends);
    const int N = g_cfg.friends;

    LOGX("host=" << g_cfg.host << " http=" << g_cfg.httpPort << " tcp=" << g_cfg.tcpPort
                 << " friends=" << N << " runId=" << runId << " hb=" << g_cfg.hbIntervalSec
                 << "s timeout=" << g_cfg.timeoutSec << "s ramp-up=" << g_cfg.rampUpPerSec
                 << "/s");

    const int64_t runStartMs = tick_ms();
    bool aborted = false;
    std::string abortReason;

    LatGroup mainLat;
    std::string mainToken;
    uint64_t mainUid = 0;
    size_t remainingFriends = 0;

    // 退出顺序（重要）：关闭所有会话 -> 等 socket 排空 -> 停 HTTP 池 ->
    // 停逻辑环 -> 停 IOCP -> 销毁 agent。反了会出现悬挂完成包。
    auto shutdown_all = [&]() {
        Logic::I().post([] { close_all_sessions(); });
        const int64_t dl = tick_ms() + 20000;
        while (g_socketsOpen.load() > 0 && tick_ms() < dl) Sleep(20);
        if (g_socketsOpen.load() > 0)
            LOGX("WARN: " << g_socketsOpen.load() << " sockets still open at shutdown");
        g_http.stop();
        Logic::I().begin_stop();
        Logic::I().join();
        g_io.stop();
    };

    try {
        resolve_host();
        g_io.start((unsigned)g_cfg.ioThreads);
        g_http.start((unsigned)g_cfg.httpThreads);
        Logic::I().start();
        LOGX("iocp workers=" << g_io.workers() << " http workers=" << g_http.workers()
                             << " (logic thread = 1)");

        // ---- 主用户：注册 + HTTP 登录（主线程同步做，与原版一致）----
        const std::string mainUser = "s3M_" + std::to_string(runId);
        {
            const std::string body =
                "{\"username\":\"" + mainUser + "\",\"password\":\"secret123\"}";
            int64_t t0 = now_us();
            HttpResponse r = http_sync("POST", "/api/register", "", body);
            mainLat.reg.push_back(now_us() - t0);
            if (!r.err.empty() || r.status != 200)
                throw std::runtime_error("register main failed: " + r.err + " " + r.body);
            t0 = now_us();
            r = http_sync("POST", "/api/login", "", body);
            mainLat.login.push_back(now_us() - t0);
            if (!r.err.empty() || r.status != 200)
                throw std::runtime_error("login main failed: " + r.err + " " + r.body);
            rapidjson::Document d = parse_json(r.body);
            mainUid = json_uint(d["data"], "userId");
            mainToken = json_string(d["data"], "token");
            if (!mainUid || mainToken.empty())
                throw std::runtime_error("login missing userId/token: " + r.body);
        }

        // ---- 预建全部好友会话对象（容器此后只读，心跳扫描无需加锁）----
        g_agents.reserve((size_t)N);
        for (int i = 0; i < N; ++i)
            g_agents.emplace_back(std::make_unique<FriendAgent>(i, runId, mainUid));
        g_hbList.clear();
        g_hbList.reserve((size_t)N + 1);
        for (auto& a : g_agents) g_hbList.push_back(a.get());

        // ---- 主用户 TCP 登录（逻辑线程）----
        g_main = new MainAgent(std::unordered_map<uint64_t, int>{});
        g_hbList.push_back(g_main);
        Logic::I().post([mainUid, mainToken] {
            g_main->configure(mainUid, mainToken, g_cfg.friends);
            g_main->begin();
        });
        if (!wait_counter(g_mainReady, 1, g_cfg.timeoutSec))
            throw std::runtime_error("main tcp login timeout");

        // ---- 启动好友：爬坡限流，避免注册瞬时打满服务端认证限流 ----
        LOGX("main ready, launching " << N << " friend agents (ramp-up="
                                      << g_cfg.rampUpPerSec << "/s)");
        Logic::I().post([N] {
            const int64_t now = tick_ms();
            for (int i = 0; i < N; ++i) {
                if (g_stop.load()) return;
                const int64_t due = g_cfg.rampUpPerSec > 0
                                        ? now + (int64_t)(i * 1000.0 / g_cfg.rampUpPerSec)
                                        : now;
                FriendAgent* a = g_agents[(size_t)i].get();
                Logic::I().later_abs(due, [a] { a->start(); });
            }
        });

        if (!wait_counter(g_ready, N, g_cfg.timeoutSec))
            throw std::runtime_error("friends not ready: " + std::to_string(g_ready.load()) +
                                     "/" + std::to_string(N));
        LOGX("all friends registered + TCP logged in; creating requests...");

        // ---- 主用户并发创建 N 个好友申请 ----
        for (int i = 0; i < N; ++i) {
            if (g_stop.load()) break;
            const uint64_t toUid = g_agents[(size_t)i]->uidPub_.load();
            if (toUid == 0) throw std::runtime_error("friend " + std::to_string(i) + " no uid");
            const std::string body =
                "{\"toUserId\":" + std::to_string(toUid) + ",\"message\":\"stress03\"}";
            FriendAgent* a = g_agents[(size_t)i].get();
            const int64_t t0 = now_us();
            Logic::I().post([a, t0, body, mainToken] {
                http_retry("POST", "/api/friends/requests", mainToken, body,
                           [a, t0](HttpResponse r) {
                               a->lat.request.push_back(now_us() - t0);
                               if (!r.err.empty() && r.status == 0) {
                                   a->fail("create request: " + r.err);
                                   return;
                               }
                               if (r.status != 200) {
                                   a->fail("create request: HTTP " + std::to_string(r.status) +
                                           " " + r.body);
                                   return;
                               }
                               uint64_t rid = 0;
                               try {
                                   rapidjson::Document d = parse_json(r.body);
                                   if (d.HasMember("data") && d["data"].IsObject() &&
                                       d["data"].HasMember("request"))
                                       rid = json_uint(d["data"]["request"], "requestId");
                               } catch (const std::exception& e) {
                                   a->fail(std::string("create request json: ") + e.what());
                                   return;
                               }
                               if (rid == 0) {
                                   a->fail("create request missing requestId: " + r.body);
                                   return;
                               }
                               a->reqId_.store(rid);
                               g_reqCreated.fetch_add(1);
                           });
            });
        }
        if (!wait_counter(g_reqCreated, N, g_cfg.timeoutSec))
            throw std::runtime_error("create requests timeout: " +
                                     std::to_string(g_reqCreated.load()) + "/" +
                                     std::to_string(N));
        LOGX("friend requests created; waiting accepts...");

        // ---- 阶段 1：并发通过 ----
        enter_phase(1);
        if (!wait_counter(g_accepted, N, g_cfg.timeoutSec))
            throw std::runtime_error("accept timeout: " + std::to_string(g_accepted.load()) +
                                     "/" + std::to_string(N));

        // ---- 阶段 2：好友发文本，主用户回复 + 双方 ACK ----
        {
            std::unordered_map<uint64_t, int> uidToIdx;
            uidToIdx.reserve((size_t)N * 2);
            for (int i = 0; i < N; ++i) uidToIdx[g_agents[(size_t)i]->uidPub_.load()] = i;
            MainAgent* m = g_main;
            Logic::I().post([m, uidToIdx] { m->install_index(uidToIdx); });
        }
        enter_phase(2);
        LOGX("friends accepted; exchanging messages...");
        if (!wait_counter(g_messaged, N, g_cfg.timeoutSec))
            throw std::runtime_error("messaging timeout: " + std::to_string(g_messaged.load()) +
                                     "/" + std::to_string(N));
        if (!wait_counter(g_mainFlushed, 1, g_cfg.timeoutSec))
            throw std::runtime_error("main delivery-ack flush timeout");
        LOGX("messages exchanged; send retries=" << g_sendRetries.load());

        // ---- 阶段 3：并发查列表（主用户也查一次）----
        enter_phase(3);
        {
            const int64_t t0 = now_us();
            HttpResponse r = http_sync("GET", "/api/friends", mainToken, "");
            mainLat.list.push_back(now_us() - t0);
            if (!r.err.empty() || r.status != 200)
                throw std::runtime_error("main list friends failed: " + r.err + " " + r.body);
        }
        if (!wait_counter(g_listed, N, g_cfg.timeoutSec))
            throw std::runtime_error("list timeout: " + std::to_string(g_listed.load()) + "/" +
                                     std::to_string(N));

        // ---- 阶段 4：并发删除主用户 ----
        enter_phase(4);
        LOGX("lists queried; deleting friends...");
        if (!wait_counter(g_deleted, N, g_cfg.timeoutSec))
            throw std::runtime_error("delete timeout: " + std::to_string(g_deleted.load()) +
                                     "/" + std::to_string(N));

        // ---- 校验主用户好友列表应为空 ----
        LOGX("all deletes acknowledged; verifying main friend list...");
        {
            HttpResponse r = http_sync("GET", "/api/friends", mainToken, "");
            if (!r.err.empty() || r.status != 200)
                throw std::runtime_error("verify failed: " + r.err + " " + r.body);
            rapidjson::Document d = parse_json(r.body);
            if (d.HasMember("data") && d["data"].IsObject()) {
                const auto& data = d["data"];
                if (data.HasMember("friends") && data["friends"].IsArray())
                    remainingFriends = data["friends"].Size();
            }
        }
        enter_phase(5);
        LOGX("verify done, remaining=" << remainingFriends);
    } catch (const std::exception& e) {
        aborted = true;
        {
            // 有真实失败点时优先报它：wait timeout 只是症状，不是原因
            std::lock_guard<std::mutex> lk(g_fatalMu);
            abortReason = g_fatalErr.empty() ? e.what()
                                             : (g_fatalErr + " (waited until timeout: " +
                                                e.what() + ")");
        }
        g_stop.store(true);
        {
            std::lock_guard<std::mutex> lk(g_fatalMu);
            if (g_fatalErr.empty()) g_fatalErr = abortReason;
        }
        std::cerr << "[stress03iocp] fatal: " << abortReason << "\n";
    }

    // 中止时把还在等业务阶段推进的会话立刻放掉，别让 shutdown 等超时
    if (aborted || g_stop.load()) {
        g_phase.store(99);
        Logic::I().post([] { Logic::I().abort_all_waiters(); });
    }

    shutdown_all();

    int okCount = 0;
    for (const auto& a : g_agents)
        if (a->ok) ++okCount;
    if (!aborted) {
        for (size_t i = 0; i < g_agents.size(); ++i)
            if (!g_agents[i]->err.empty())
                std::cerr << "[stress03iocp] friend " << i << " error: " << g_agents[i]->err
                          << "\n";
    } else {
        for (size_t i = 0; i < g_agents.size(); ++i)
            if (!g_agents[i]->err.empty()) {
                std::cerr << "[stress03iocp] friend " << i << " error: " << g_agents[i]->err
                          << "\n";
                break;  // 中止场景只报首个，避免刷屏
            }
    }

    LatGroup agg;
    merge_append(agg.reg, mainLat.reg);
    merge_append(agg.login, mainLat.login);
    merge_append(agg.list, mainLat.list);
    for (const auto& a : g_agents) {
        merge_append(agg.reg, a->lat.reg);
        merge_append(agg.login, a->lat.login);
        merge_append(agg.request, a->lat.request);
        merge_append(agg.accept, a->lat.accept);
        merge_append(agg.send, a->lat.send);
        merge_append(agg.recv, a->lat.recv);
        merge_append(agg.list, a->lat.list);
        merge_append(agg.del, a->lat.del);
    }

    const double elapsed = (double)(tick_ms() - runStartMs) / 1000.0;
    std::cout << std::fixed << std::setprecision(2);

    if (aborted) {
        std::cout << "\n===== Stress03 Aborted =====\n";
        std::cout << "reason          : " << abortReason << "\n";
        std::cout << "friends ready   : " << g_ready.load() << "/" << N << "\n";
        std::cout << "accepted        : " << g_accepted.load() << "/" << N << "\n";
        std::cout << "messages        : " << g_messaged.load() << "/" << N << "\n";
        std::cout << "listed          : " << g_listed.load() << "/" << N << "\n";
        std::cout << "deleted         : " << g_deleted.load() << "/" << N << "\n";
        std::cout << "failed          : " << g_failed.load() << "\n";
        std::cout << "============================\n";
    } else {
        std::cout << "\n===== Stress03 Report (IOCP) =====\n";
        std::cout << "elapsed                  : " << elapsed << " s\n";
        std::cout << "friends                : " << N << "\n";
        std::cout << "friends ready          : " << g_ready.load() << "\n";
        std::cout << "friend requests created: " << agg.request.size() << "\n";
        std::cout << "accepted               : " << g_accepted.load() << "\n";
        std::cout << "messages sent+acked    : " << g_messaged.load() << "\n";
        std::cout << "friend lists queried   : " << g_listed.load() << "\n";
        std::cout << "friends deleted        : " << g_deleted.load() << "\n";
        std::cout << "full lifecycle ok      : " << okCount << "/" << N << "\n";
        std::cout << "failed                 : " << g_failed.load() << "\n";
        std::cout << "send retries           : " << g_sendRetries.load() << "\n";
        std::cout << "main friends after delete: " << remainingFriends << "\n";
        std::cout << "--- iocp runtime ---\n";
        std::cout << "io worker threads      : " << g_io.workers() << "\n";
        std::cout << "http pool threads      : " << g_http.workers() << "\n";
        std::cout << "tcp connections peak   : " << g_socketsPeak.load() << "\n";
        std::cout << "tcp connections open   : " << g_socketsOpen.load() << "\n";
        std::cout << "frames sent / recv     : " << g_framesSent.load() << " / "
                  << g_framesRecv.load() << "\n";
        std::cout << "bytes sent / recv      : " << g_bytesSent.load() << " / "
                  << g_bytesRecv.load() << "\n";
        std::cout << "http requests          : " << g_httpCalls.load() << "\n";
        std::cout << "throughput             : " << (elapsed > 0 ? (double)N / elapsed : 0.0)
                  << " friend-lifecycle/s\n";
        std::cout << "\nlatency (ms)        p50 / p95 / p99 / avg / max\n";
        print_latency("register", agg.reg);
        print_latency("login", agg.login);
        print_latency("create request", agg.request);
        print_latency("accept", agg.accept);
        print_latency("tcp send", agg.send);
        print_latency("tcp reply", agg.recv);
        print_latency("list friends", agg.list);
        print_latency("delete friend", agg.del);
        std::cout << "================================\n";
    }

    if (g_main) delete g_main;
    g_main = nullptr;
    g_agents.clear();
    WSACleanup();

    const bool allOk = !aborted && g_failed.load() == 0 && okCount == N &&
                       remainingFriends == 0 && !g_stop.load();
    return allOk ? 0 : 1;
}
