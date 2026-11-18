#pragma once
#include <iostream>
#include <streambuf>
#include <cstdlib>
#include <cstdio>          // std::fwrite/std::fflush：致命事件直写 fd 2
#include <chrono>          // 限频窗口
#include <mutex>           // 致命日志的限频状态（见 logFatal 的说明）
#include <string>

// ============================================================================
// 日志策略（两条通道，故意分开）
// ----------------------------------------------------------------------------
// 1) 常规流水（std::cout / std::cerr）
//    QUIET_LOG=1 时全部丢进 NullStreambuf。这条纪律不能破：
//    服务端每条消息都会打印（[login]/[route]/[offline]/[timeout]…，而且多数带
//    std::endl 触发 flush），在 2 核机器上"往 journald 写日志"会直接挤占
//    epoll 收发的时间片，压测数据会被日志本身污染。
//
// 2) 致命事件（logFatal）
//    **永远输出**，直写 stderr 的文件描述符，绕开被替换掉的 std::cerr 流缓冲区。
//    动机是一次真实事故：2 核机器上 io 线程被 handler 异常打断后
//    main() 会以退出码 0 干净退出，而唯一能定罪的
//        [io] thread N exception: <原因>
//    正好走 std::cerr，被 QUIET_LOG=1 静默吞掉了 —— 于是"服务进程消失"这件事
//    在日志里一个字都没留下，只能靠客户端错误码反推。这种日志不能再有第二个。
//
//    为了不重新引入"刷日志拖慢 io 线程"，logFatal 自带限频：
//    每秒最多输出 kFatalPerSec 条，其余只累计计数，并在下一次输出时带上
//    "(+N suppressed)"。也就是说它**便宜**，可以放进过载路径里。
// ============================================================================

class NullStreambuf : public std::streambuf {
protected:
    int overflow(int c) override { return c; }
    int sync() override { return 0; }
};

inline void setupLogQuiet() {
    const char* q = std::getenv("QUIET_LOG");
    if (!q || !*q) return;                    // 未设置或空：正常输出
    if (std::string(q) == "0") return;        // 显式 0：正常输出

    static NullStreambuf nullBuf;
    std::cout.rdbuf(&nullBuf);
    std::cerr.rdbuf(&nullBuf);
    // 注意：这里**不**动 stderr 这个 FILE*（fd 2），logFatal 走它。
}

namespace kc {

// 致命事件：一行一条，带 "[fatal] " 前缀，直写 fd 2，并限频。
// ----------------------------------------------------------------------------
// 为什么用 mutex 而不是无锁计数：致命日志按定义是低频事件，而且下面已经把
// 每秒输出量压到 kFatalPerSec；无锁窗口滚动要处理"CAS 赢了才重置计数器"这种
// 容易写错的细节（写错的结果是限流失效或计数丢失），一把锁反而更短更准。
// 锁的粒度只覆盖"计数 + 可能的这一次写"，不会长时间持有。
inline void logFatal(const std::string& msg) {
    static std::mutex                 mu;
    static long long                  windowStartMs = 0;
    static int                        emittedInWindow = 0;
    static long long                  suppressed = 0;

    constexpr int kFatalPerSec = 20;      // 每秒最多 20 条，其余只累计不写

    auto nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    std::lock_guard<std::mutex> lk(mu);
    const long long now = nowMs();

    if (now - windowStartMs >= 1000) {     // 窗口滚动
        windowStartMs   = now;
        emittedInWindow = 0;
    }

    if (emittedInWindow >= kFatalPerSec) { // 本秒已写满：只计数，不碰 fd
        ++suppressed;
        return;
    }
    ++emittedInWindow;

    // 直写 fd 2：std::cerr 的 rdbuf 可能已被换成 NullStreambuf，
    // 但 stderr 这个 FILE* 仍然指向原来的描述符。
    std::fputs("[fatal] ", stderr);
    std::fwrite(msg.data(), 1, msg.size(), stderr);

    if (suppressed > 0) {                  // 把上一段被压掉的数量带出来
        const std::string tail = " (+" + std::to_string(suppressed) + " suppressed)";
        std::fwrite(tail.data(), 1, tail.size(), stderr);
        suppressed = 0;
    }
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace kc
