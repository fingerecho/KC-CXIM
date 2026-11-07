#pragma once
#include <boost/asio.hpp>
#include "session_manager.h"

// ============================================================================
// server：负责监听端口、接受新连接
// ----------------------------------------------------------------------------
// 它本身不处理消息，只做两件事：
//   1) TCP accept（接收新连接）；
//   2) 为每个新连接创建 session，并把"全局在线用户表"注入给它。
// ============================================================================

namespace kc {
class server {
public:
    // 构造时就在指定端口开始监听，并投递第一个异步 accept。
    server(boost::asio::io_context& io, unsigned short port);

    // 打印监听信息（真正的事件循环在 main.cpp 里的 io.run()）。
    void run();

private:
    void do_accept();   // 投递一次异步 accept

    // 全局唯一的在线用户表。先于 acceptor_ 声明，保证构造顺序。
    SessionManager sessions_;

    // acceptor_：监听 socket，负责"接客"。
    boost::asio::ip::tcp::acceptor acceptor_;
};
} // namespace kc
