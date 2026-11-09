#include "server.h"
#include "session.h"
#include "db.h"
#include <iostream>

namespace kc {

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
void server::run() {
    std::cout << "[server] listening on port "
              << acceptor_.local_endpoint().port() << std::endl;
}

// ---------------------------------------------------------------------------
// 投递一次异步 accept。
// ---------------------------------------------------------------------------
void server::do_accept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) {
                // 新连接：把 socket、在线表、数据库层、JWT 服务一起交给 session。
                std::make_shared<session>(std::move(socket), sessions_, db_, tokens_)->start();
            }
            if (acceptor_.is_open()) do_accept();
        });
}

} // namespace kc
