#include <atomic>                          // std::atomic：io 线程存活/故障计数
#include <iostream>                        // std::cout / std::cerr：启动与错误日志
#include <cstdlib>                         // std::getenv / std::stoi：读取环境变量与端口参数
#include <string>                          // std::string：配置路径
#include <algorithm>                       // std::max
#include <thread>                          // std::thread：多线程运行 io_context
#include <vector>                          // std::vector：io 线程集合
#include <boost/asio/io_context.hpp>       // io_context：整个服务共享的事件循环

#include "server.h"                        // TCP 长连接服务器
#include "http_server.h"                   // 注册/登录 HTTP 短连接服务器（Boost.Beast）
#include "token_service.h"                 // 无状态 JWT（HS256）签发/校验
#include "jwt_config.h"                    // 加载项目根目录 JWTConfig.json
#include "db.h"                            // 数据库层（连接池 + worker 线程池）
#include "db_config.h"                     // 数据库配置

#include "quiet_log.h"                     // 防止运行期打印std::cout的日志

/*************core-dump 修复 ⑥：崩的时候必须自己留下栈帧***************/
// 现状（本次事故的直接取证障碍）：
//   journal 里只有
//       CXIM.service: Main process exited, code=dumped, status=11/SEGV
//       CXIM.service: Failed with result 'core-dump'.
//   而生成的 CXIM.service 写的是 LimitCORE=0 → **内核根本没落盘 core**，
//   coredumpctl 是空的；Release 构建又没有 -g，就算有 core 也只能看到地址。
//   于是"SEGV 在哪一行"这个问题，靠现有日志无法回答，只能靠读代码猜。
// 这一步不修 bug，它修的是"下一轮还是查不到"：
//   * 进程内装一个 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT 处理器，
//     在死了之前把信号名 + 出错地址 + 原生栈帧写进 fd 2（= journald），
//     完全不经过 std::cout/std::cerr，所以 QUIET_LOG 挡不住它；
//   * 写完恢复默认处理并重新触发同一个信号，让进程仍按"信号致死"记账，
//     这样 systemd 才会落 core、才会走 Restart 逻辑。
//     ⚠ 上一轮在这里踩了坑：**光 `signal(SIG_DFL)+raise()` 是不够的**，
//       sigaction 默认会在 handler 执行期间阻塞该信号本身，raise 只是挂起，
//       于是实际结果是 `_exit(139)` → journal 里 `code=exited, status=139` →
//       **core 没落盘**。15:15:09 那轮就是这样白崩一次。修法见下面的修复⑫
//       （SA_NODEFER 让同一个信号在 handler 内不被阻塞 + SA_ONSTACK 换备用栈，之后 raise 才真能打死）。
// 配合（缺一不可）：
//   1) build.sh 生成的 service 改成 LimitCORE=infinity → 有 core 可 gdb；
//   2) step1_tcp_echo/CMakeLists.txt 给服务端加 -g -fno-omit-frame-pointer
//      → core / 栈帧能符号化（仍是 -O2/-O3，性能不变）。
// 有了这两样，同一份日志里就会是：
//   [fatal] crash signal=SIGSEGV addr=0x0
//   #0 0x55e8... in kc::session::do_write() .../session.cpp:842
//   ...   → 直接定位到行，不用再猜。
// 条件编译用 __linux__/__APPLE__ 而不是 __unix__：MinGW 也定义了 __unix__，
// 但它的 CRT 里没有 <execinfo.h>，用 __unix__ 会让 Windows 侧构建直接断在这里。
#if defined(__linux__) || defined(__APPLE__)
#include <csignal>                          // sigaction / SIGSEGV / SIG_DFL / raise
#include <signal.h>                         // struct sigaction / stack_t / sigaltstack / SA_*（csignal 不保证带这些）
#include <cstddef>                          // nullptr_t / size_t
#include <cstdint>                          // std::uintptr_t
#include <cstring>                          // std::strlen
#include <execinfo.h>                       // backtrace / backtrace_symbols_fd
#include <unistd.h>                         // write / STDERR_FILENO

namespace {

// 只用 write(2) 输出：handler 里不能碰 stdio 的锁、不能 new、不能拿 cout 的
// 流锁（崩溃点很可能正是持有着这些锁的时候）。
void writeRaw(const char* s, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(STDERR_FILENO, s + off, n - off);
        if (w <= 0) return;                 // 写不进去就算了，不要在这里打转
        off += static_cast<std::size_t>(w);
    }
}
void writeLit(const char* s) { writeRaw(s, std::strlen(s)); }

// 手写十六进制：避免 snprintf（内部可能 malloc）。
void writeHex(std::uintptr_t v) {
    char buf[2 + sizeof(std::uintptr_t) * 2];
    buf[0] = '0'; buf[1] = 'x';
    constexpr int digits = static_cast<int>(sizeof(std::uintptr_t) * 2);
    for (int i = 0; i < digits; ++i) {
        const unsigned nib = static_cast<unsigned>((v >> ((digits - 1 - i) * 4)) & 0xFu);
        buf[2 + i] = static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    writeRaw(buf, sizeof(buf));
}

void crashHandler(int sig, siginfo_t* info, void* /*ucontext*/) {
    writeLit("[fatal] crash signal=");
    switch (sig) {
    case SIGSEGV: writeLit("SIGSEGV"); break;
    case SIGBUS:  writeLit("SIGBUS");  break;
    case SIGILL:  writeLit("SIGILL");  break;
    case SIGFPE:  writeLit("SIGFPE");  break;
    case SIGABRT: writeLit("SIGABRT"); break;
    default:      writeLit("?");       break;
    }
    // addr 对 SIGSEGV/SIGBUS 就是被踩的那个指针：0x0 / 0x1～0x1000 这种小值
    // 一眼就能认出是"空指针 + 成员偏移"或"use-after-free 后读虚表"。
    writeLit(" addr=");
    writeHex(reinterpret_cast<std::uintptr_t>(info ? info->si_addr : nullptr));
    writeLit("\n[fatal] backtrace:\n");

    void* frames[64];
    const int n = ::backtrace(frames, 64);
    // 跳过 handler 自身那一两帧；写 fd 版本内部只做一次 malloc 失败也要能走，
    // 所以这里宁可接受"某些发行版只显示地址"，也强于什么都没有。
    const int skip = n > 3 ? 2 : 0;
    if (n > skip) ::backtrace_symbols_fd(frames + skip, n - skip, STDERR_FILENO);

    /*************core-dump 修复 ⑫：重新触发必须真的把 core 落下来***************/
    // 上一版这里只做了 `signal(sig, SIG_DFL); raise(sig);`，**实测是错的**：
    // 通过 sigaction 进入 handler 时，除非带 SA_NODEFER，内核会在 handler 执行期间
    // **阻塞该信号本身**，于是 raise(sig) 只是挂起、不会终止进程，控制流继续走到
    // 下面的 _exit()。结果 journal 里是
    //     code=exited, status=139/n/a   /   Failed with result 'exit-code'.
    // 而不是 code=dumped, status=11/SEGV —— **core 一个字节都没落盘**，
    // gdb 无从下手（15:15:09 那一轮就是这样，白死了一次）。
    // 修法：装处理器时带 **SA_NODEFER**（见 installCrashHandler）——它在"进入 handler
    // 时不阻塞该信号"这一步就把问题解决了，因此这里只需要恢复默认 + raise，
    // 不必再去动信号掩码（少一个 pthread.h/多线程语义的依赖，崩溃路径上少一样东西
    // 可能出错）。最后那句 _exit 是兜底：万一 raise 仍没打死，也绝不回到崩溃现场继续跑。
    ::signal(sig, SIG_DFL);
    ::raise(sig);
    ::_exit(128 + sig);        // raise 万一没打死（被忽略等），也要立刻结束，不能回到崩溃现场继续
}

// 备用信号栈：SIGSEGV 的常见成因之一是"栈被用尽/栈上野写"，
// 这种情况下在主栈上跑 handler 会二次 fault，现场就没了。
// SIGSTKSZ/MINSIGSTKSZ 在 glibc 2.34+ 已不是编译期常量，所以这里用固定大小。
void installAltStack() {
    static char altStackMem[64 * 1024];
    stack_t ss;
    ss.ss_sp    = altStackMem;
    ss.ss_size  = sizeof(altStackMem);
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);        // 失败也无所谓：只是少一层保险
}

void installCrashHandler() {
    installAltStack();

    static struct sigaction sa;
    sa.sa_sigaction = &crashHandler;
    // SA_SIGINFO：拿 si_addr；SA_ONSTACK：栈溢出时也能跑；
    // SA_NODEFER：**关键**，否则 handler 里重新 raise 同一个信号会被阻塞，
    //            进程不会按信号死 → 不落 core（修复⑫的起因）。
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    const int kFatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (int s : kFatal) ::sigaction(s, &sa, nullptr);
}

} // namespace
#else
inline void installCrashHandler() {}   // Windows/MSVC：用调试器看崩溃转储，不装处理器
#endif
/*************core-dump 修复 ⑥：崩的时候必须自己留下栈帧***************/

// ============================================================================
// 程序入口
// ----------------------------------------------------------------------------
// 启动顺序：
//   1) 读 DBConfig.ini + DbRuntime（环境变量），初始化连接池 + SQL/哈希双 worker 池；
//   2) 读 JWTConfig.json，构造全局共享的 TokenService；
//   3) 在同一 io_context 里创建 TCP server 与 HTTP server（两个 acceptor）；
//   4) 用多个线程同时 run() 这个 io_context，充分利用多核处理高并发连接。
//
// 用法：
//   cxim-server
//   环境变量：
//     DB_CONFIG=... JWT_CONFIG=... HTTP_PORT=... TCP_PORT=... QUIET_LOG=0
//     —— DB 层（详见 db.h:DbRuntime）——
//     BCRYPT_COST=10            bcrypt 成本因子（每 ±1 算力翻倍/减半）
//     DB_WORKER_COUNT=<核数*2>  SQL worker；DB_POOL_SIZE 默认与它相同
//     DB_HASH_WORKER_COUNT=<核数> bcrypt worker
//     DB_QUEUE_LIMIT=2000       单条任务队列上限（P0-4 背压）
//     OFFLINE_CURSOR=0          1=用游标补推离线消息，不写 offline_messages（P1-4）
//     RELATION_CACHE_TTL=10000  好友关系缓存毫秒，0=关闭（P1-1）
//     RELATION_CACHE_MAX=20000  关系缓存条目上限
//     —— 过载保护（P0-4）——
//     HTTP_MAX_INFLIGHT=512     HTTP 在途请求上限，超过回 503 + Retry-After
//     HTTP_BODY_MAX=65536       HTTP 请求体上限，超过回 413
//     AUTH_RATE_BURST=40        注册/登录/刷新 突发额度（令牌桶容量）
//     AUTH_RATE_PER_SEC=20      注册/登录 每秒令牌数（按 2 核容量算）
//     TCP_MAX_SESSIONS=2000     TCP 并发连接上限，超过直接关闭新连接
//     TCP_MAX_OUTSTANDING=64    单条连接未完成的 DB 任务上限
//     IDLE_TIMEOUT_SEC=60       TCP 空闲踢人秒数（客户端心跳应远小于它）
// ============================================================================

int main(int argc, char* argv[]) {
    installCrashHandler();  // core-dump 修复 ⑥：先装崩溃现场记录，后面才有得查
    setupLogQuiet();    // 设置std::cout打印日志的开关
    unsigned short tcpPort = 8888;                                  // TCP 默认端口
    if (const char* env = std::getenv("TCP_PORT")) {            // 环境变量优先
        try { tcpPort = static_cast<unsigned short>(std::stoi(env)); } catch (...) {}
    }

    unsigned short httpPort = 8080;                              // HTTP 默认端口
    if (const char* env = std::getenv("HTTP_PORT")) {            // 环境变量优先
        try { httpPort = static_cast<unsigned short>(std::stoi(env)); } catch (...) {}
    }

    std::string cfgPath = "DBConfig.ini";                        // 数据库配置默认路径
    if (const char* env = std::getenv("DB_CONFIG")) {            // 环境变量优先
        if (env[0] != '\0') cfgPath = env;                       // 非空才覆盖
    }

    std::string jwtPath = "JWTConfig.json";                      // JWT 配置默认在项目根目录
    if (const char* env = std::getenv("JWT_CONFIG")) {           // 环境变量优先
        if (env[0] != '\0') jwtPath = env;                       // 非空才覆盖
    }

    try {
        boost::asio::io_context io;                              // 多线程共享的事件循环：TCP 与 HTTP 共用

        // 读取数据库配置并初始化 DB 层。
        kc::DbConfig cfg = kc::DbConfig::load(cfgPath);          // 解析 INI
        std::cout << "[db] connecting to " << cfg.host << ":" << cfg.port
                  << "/" << cfg.dbname << " as " << cfg.user << std::endl;
        /*************P0-1/P0-2/P0-7：DB 运行期参数集中在 DbRuntime***************/
        // 这里以前是"池 100 条 + 16 个 worker"：在 2 核机器上，16 个线程跑
        // bcrypt + SQL 只会互相抢核（还要带上同机的 mariadbd），实测对延迟无改善。
        // 现在由 DbRuntime::fromEnv() 统一决定，默认按核数收敛：
        //   BCRYPT_COST / DB_WORKER_COUNT / DB_POOL_SIZE / DB_HASH_WORKER_COUNT /
        //   DB_QUEUE_LIMIT / OFFLINE_CURSOR / RELATION_CACHE_TTL / RELATION_CACHE_MAX
        // 要点：连接池上限与 SQL worker 数一致（并发连接数不可能超过 worker 数），
        //       哈希 worker = 核数（超订不提高吞吐，只放大所有接口的延迟）。
        // 若 MariaDB max_connections 较小，请同时调小 DB_WORKER_COUNT/DB_POOL_SIZE。
        kc::DbRuntime dbRt = kc::DbRuntime::fromEnv();
        kc::Db db(io, cfg, dbRt);
        /*************P0-1/P0-2/P0-7：DB 运行期参数集中在 DbRuntime***************/

        // 加载 JWT 配置并构造 token 服务：HTTP 登录签发、TCP 登录验签共用同一实例。
        kc::JwtConfig jwtCfg = kc::JwtConfig::load(jwtPath);     // 读取 jwt_secret / jwt_issuer
        kc::TokenService tokens(jwtCfg);                         // 无状态签名/验签服务

        // 同一个 io_context 里挂两个 acceptor：TCP 长连接 + HTTP 短连接。
        kc::server srv(io, tcpPort, db, tokens);                    // 创建 TCP 监听
        // HTTP 层与 TCP 层共享同一个 SessionManager：
        // 好友申请/通过等写操作走 HTTP，完成后通过在线表把事件推给 TCP 长连接。
        kc::http_server http(io, httpPort, db, tokens, srv.sessions());
        srv.run();                                               // 打印 TCP 端口
        http.run();                                              // 打印 HTTP 端口

        /*************P0-1：io 线程数不再"至少 4 个"***************/
        // 原来固定 min 4 个 io 线程，在 2 核机器上意味着 4 个 io + 8 个 DB worker
        // 一起抢 2 个核（还要算上同机的 mariadbd）。io 线程只做 epoll 收发，
        // 线程数超过核数并不会提高吞吐，只会加剧抢占与上下文切换。
        // 这里取"核数，下限 2，上限 8"：单核机器也保证 accept 与读写不互相饿死。
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 2;                              // 探测失败按 2 核处理
        unsigned int ioThreadCount = hw;
        if (ioThreadCount < 2u)  ioThreadCount = 2u;
        if (ioThreadCount > 8u)  ioThreadCount = 8u;
        std::cout << "[server] io threads=" << ioThreadCount << std::endl;

        /*************P0-可观测：io 线程死亡必须留痕，并且不能"成功退出"***************/
        // Boost.Asio 的语义：completion handler 里抛出的异常会从 io_context::run()
        // 逃逸出来。也就是说一个 handler 抛异常的真实代价是"杀掉一个 io 线程"，
        // 而不是"断开那条连接"。原来的写法只在 std::cerr 上打一行就结束线程：
        //   * QUIET_LOG=1 时 std::cerr 也被重定向到 NullStreambuf（见 quiet_log.h），
        //     所以这唯一的一行在生产环境里根本不存在；
        //   * 全部 io 线程返回后，main() 会一路走到 return 0，进程以**退出码 0**
        //     干净退出 —— 而 systemd 的 Restart=on-failure 不覆盖退出码 0，
        //     服务于是死着，没人拉起来。压测里看到的就是
        //     "已建立连接集体 10054/EOF + 新连接 10061 refused + HTTP 收到 0 字节"。
        // 现在：每个线程退出都走 logFatal（永不被静音、自带限频），并记录还剩几个；
        // 只要发生过异常，进程就以 70 退出，让重启策略能识别这是故障。
        std::atomic<unsigned> ioThreadsAlive{ioThreadCount};
        std::atomic<unsigned> ioThreadsFaulted{0};

        std::vector<std::thread> ioThreads;
        ioThreads.reserve(ioThreadCount);
        for (unsigned int i = 0; i < ioThreadCount; ++i) {
            ioThreads.emplace_back([&io, i, &ioThreadsAlive, &ioThreadsFaulted] {
                try {
                    io.run();
                    // 正常返回同样是不该发生的事：acceptor 与空闲定时器会让 run()
                    // 永远有活儿干。它返回了，说明事件循环已经无工可作 = 服务失聪。
                    ioThreadsFaulted.fetch_add(1);
                    kc::logFatal("[io] thread " + std::to_string(i) +
                                 " returned from io.run() (context drained/stopped)" +
                                 ", alive=" +
                                 std::to_string(ioThreadsAlive.fetch_sub(1) - 1));
                } catch (const std::exception& e) {
                    ioThreadsFaulted.fetch_add(1);
                    kc::logFatal(std::string("[io] thread ") + std::to_string(i) +
                                 " DIED on handler exception: " + e.what() + ", alive=" +
                                 std::to_string(ioThreadsAlive.fetch_sub(1) - 1));
                } catch (...) {
                    ioThreadsFaulted.fetch_add(1);
                    kc::logFatal("[io] thread " + std::to_string(i) +
                                 " DIED on unknown exception, alive=" +
                                 std::to_string(ioThreadsAlive.fetch_sub(1) - 1));
                }
            });
        }
        for (auto& t : ioThreads) {
            if (t.joinable()) t.join();
        }

        // 走到这里 = 所有 io 线程都已返回。对本服务而言这永远是故障，不是"正常结束"。
        if (ioThreadsFaulted.load() > 0) {
            kc::logFatal("[fatal] all " + std::to_string(ioThreadCount) +
                         " io threads stopped, " +
                         std::to_string(ioThreadsFaulted.load()) +
                         " abnormal; exit 70 so the service manager restarts us");
            return 70;                         // 非 0：让 Restart 策略识别为故障
        }
        kc::logFatal("[fatal] io loop ended without fault, exit 70 (should not happen)");
        return 70;
        /*************P0-可观测：io 线程死亡必须留痕，并且不能"成功退出"***************/
    } catch (const std::exception& e) {
        kc::logFatal(std::string("[server] fatal: ") + e.what());  // 配置/启动失败
        return 1;                                                 // 非 0 退出码
    }
}
