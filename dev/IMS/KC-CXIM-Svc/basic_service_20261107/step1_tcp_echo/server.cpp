#include "server.h"
#include "session.h"
#include "db.h"
#include "quiet_log.h"    // logFatal：拒绝新连接是要害事件，不能被 QUIET_LOG 静默

#include <cstdlib>     // P0-4：getenv 读取连接上限
#include <iostream>
#include <string>      // std::to_string：logFatal 的消息拼装

namespace kc {

namespace {

// P0-4：TCP 并发连接上限。
// 每条长连接都要占 1 个 fd、1 个 session 对象、读写缓冲和一个空闲计时器；
// 2GB 内存 + 默认 nofile 的机器上，500 并发就已经开始出问题（压测里
// connect 超时/被拒有相当一部分是 fd 与 backlog 层面的）。
// 超过上限时直接关闭新连接（三次握手已经建立，快速 FIN 让客户端自己退避重连），
// 优先保住已经在线的会话，而不是把所有人都拖慢。
std::size_t maxSessions() {
    static const std::size_t n = [] {
        const char* v = std::getenv("TCP_MAX_SESSIONS");
        if (!v || !*v) return static_cast<std::size_t>(2000);
        try {
            long x = std::stol(v);
            return x > 0 ? static_cast<std::size_t>(x) : static_cast<std::size_t>(2000);
        } catch (...) { return static_cast<std::size_t>(2000); }
    }();
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// 构造：创建监听 acceptor。
//   endpoint(v4(), port)：监听本机所有网卡的 IPv4 地址的指定端口。
//   构造 acceptor 时就会 bind + listen，端口被占用会抛异常（main 里会捕获）。
// 随后立即投递一次异步 accept，这样 io.run() 一启动就能接客。
// ---------------------------------------------------------------------------
server::server(boost::asio::io_context& io, unsigned short port, Db& db,
               TokenService& tokens)
    : db_(db),
      tokens_(tokens),
      acceptor_(io, boost::asio::ip::tcp::endpoint(
                        boost::asio::ip::tcp::v4(), port)) {
    do_accept();
}

// 打印实际监听端口（传 0 时系统会自动分配端口，所以要看 local_endpoint）。
// 同时自述 TCP 侧的过载参数：这些值全部由环境变量控制，没生效时的表现是
// "优化做了但行为没变"，所以启动就把它打印出来（对应 readme §11.6 检查清单第 3 条）。
void server::run() {
    std::cout << "[server] listening on port "
              << acceptor_.local_endpoint().port()
              << ", max_sessions=" << maxSessions() << std::endl;
}

// ---------------------------------------------------------------------------
// 投递一次异步 accept。
// ---------------------------------------------------------------------------
void server::do_accept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) {
                /*************P0-4：连接数过载保护***************/
                // 超限时立刻关掉新连接（不进入 session，也就不产生任何 DB/内存开销）。
                // 客户端收到 FIN 后按自己的退避策略重连，比"挤进来一起变慢"好。
                if (session::liveCount() >= maxSessions()) {
                    boost::system::error_code ignored;
                    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                    socket.close(ignored);
                    // logFatal：这是"容量已经打满"的信号，生产默认 QUIET_LOG=1 时
                    // 走 std::cerr 会等于没说。限频由 logFatal 内部负责。
                    kc::logFatal("[server] reject new connection, live=" +
                                 std::to_string(session::liveCount()) +
                                 " cap=" + std::to_string(maxSessions()));
                } else {
                    // 新连接：把 socket、在线表、数据库层、JWT 服务一起交给 session。
                    std::make_shared<session>(std::move(socket), sessions_, db_, tokens_)->start();
                }
                /*************P0-4：连接数过载保护***************/
            }
            // 注意：这里不因 ec 停止 accept 循环，端口存活就继续接客（原行为）。
            if (acceptor_.is_open()) do_accept();
        });
}

} // namespace kc
