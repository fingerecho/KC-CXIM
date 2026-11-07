#include "server.h"
#include "session.h"
#include <iostream>

namespace kc {

// ---------------------------------------------------------------------------
// 构造：创建监听 acceptor。
//   endpoint(v4(), port)：监听本机所有网卡的 IPv4 地址的指定端口。
//   构造 acceptor 时就会 bind + listen，端口被占用会抛异常（main 里会捕获）。
// 随后立即投递一次异步 accept，这样 io.run() 一启动就能接客。
// ---------------------------------------------------------------------------
server::server(boost::asio::io_context& io, unsigned short port)
    : acceptor_(io, boost::asio::ip::tcp::endpoint(
                        boost::asio::ip::tcp::v4(), port)) {
    do_accept();
}

// 打印实际监听端口（传 0 时系统会自动分配端口，所以要看 local_endpoint）。
void server::run() {
    std::cout << "[server] listening on port "
              << acceptor_.local_endpoint().port() << std::endl;
}

// ---------------------------------------------------------------------------
// 投递一次异步 accept。
// 异步 accept 的回调在"有新连接到达"时才被 io_context 调用。
// 注意：一次 async_accept 只接收一个连接，所以回调里要再投递下一次，
//       形成"永不停歇"的接收循环。
// ---------------------------------------------------------------------------
void server::do_accept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) {
                // 有新连接：创建 session，把 socket 和在线表交给它，然后 start。
                // 注意：这里用 make_shared，session 会在其异步回调里通过
                // shared_from_this 保住自己，所以这个临时 shared_ptr 释放后
                // 连接依然存活。
                std::make_shared<session>(std::move(socket), sessions_)->start();
            }
            // 只要 acceptor 还开着，就继续接收下一个连接。
            if (acceptor_.is_open()) do_accept();
        });
}

} // namespace kc
