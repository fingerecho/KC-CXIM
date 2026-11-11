#pragma once
#include <boost/asio/io_context.hpp>              // io_context：与 TCP 服务共享的同一个事件循环
#include <boost/asio/ip/tcp.hpp>                  // TCP acceptor/socket 类型

namespace kc {

class Db;               // 数据库访问层（前向声明，避免头文件耦合）
class TokenService;     // JWT 签发/校验服务（前向声明）
class SessionManager;   // 在线用户表（HTTP 完成后用它把好友事件推给在线用户）

// ============================================================================
// http_server：处理注册/登录的 HTTP 短连接服务器（基于 Boost.Beast）
// ----------------------------------------------------------------------------
// 设计要点：
//   * 与 TCP 长连接服务共用同一个 io_context，且 io.run() 仍然只跑一个线程；
//   * 每个 HTTP 连接只处理一次请求就关闭（短连接），符合注册/登录的场景；
//   * 所有耗时操作（SQL + bcrypt）都通过 Db 的 worker 线程完成，本层只做
//     解析 JSON、入队、回写，保证不会阻塞 io_context。
// ============================================================================

class http_server {
public:
    // 构造时立即 bind + listen；端口被占用会抛异常（由 main 捕获）。
    // sessions 用于把好友事件实时推给已登录的 TCP 长连接。
    http_server(boost::asio::io_context& io, unsigned short port,
                Db& db, TokenService& tokens, SessionManager& sessions);

    // 打印监听信息（真正的事件循环仍在 main 的 io.run()）。
    void run();

private:
    void do_accept();                              // 投递一次异步 accept

    boost::asio::ip::tcp::acceptor acceptor_;      // HTTP 监听器
    Db&             db_;                           // 注入的数据库层
    TokenService&   tokens_;                       // 注入的 JWT 服务
    SessionManager& sessions_;                     // 注入的在线用户表（用于实时推送好友事件）
};

} // namespace kc
