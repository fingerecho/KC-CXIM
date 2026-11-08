#pragma once
#include <boost/asio.hpp>
#include "session_manager.h"

// ============================================================================
// server：负责监听端口、接受新连接
// ----------------------------------------------------------------------------
// 它本身不处理消息，只做两件事：
//   1) TCP accept（接收新连接）；
//   2) 为每个新连接创建 session，并把"在线用户表"和"数据库层"注入给它。
// ============================================================================

namespace kc {

class Db;   // 数据库访问层（定义在 db.h）

class server {
public:
    // 构造时就在指定端口开始监听，并投递第一个异步 accept。
    server(boost::asio::io_context& io, unsigned short port, Db& db);

    // 打印监听信息（真正的事件循环在 main.cpp 里的 io.run()）。
    void run();

private:
    void do_accept();

    Db&            db_;          // 数据库层（引用，由 main 持有）
    SessionManager sessions_;    // 全局在线用户表
    boost::asio::ip::tcp::acceptor acceptor_;
};

} // namespace kc
