#include <iostream>
#include <boost/asio/io_context.hpp>
#include "server.h"

// ============================================================================
// 程序入口
// ----------------------------------------------------------------------------
// io_context 是 Boost.Asio 的"事件循环 / 任务队列"：
//   - 所有 async_xxx 操作完成后，回调都会被放进它里面；
//   - 调用 io.run() 才会真正开始执行这些回调，并一直阻塞到没有任务为止。
// 所以服务器主线程其实就是"卡在 io.run() 里不停处理网络事件"。
// ============================================================================

int main(int argc, char* argv[]) {
    // 默认监听 8888；命令行第一个参数可覆盖，例如：
    //   tcp_echo_server.exe 9000
    unsigned short port = 8888;
    if (argc > 1) port = static_cast<unsigned short>(std::stoi(argv[1]));

    try {
        boost::asio::io_context io;   // 事件循环
        kc::server srv(io, port);     // 创建服务器（构造里已开始监听并投递 accept）
        srv.run();                    // 打印监听端口
        io.run();                     // 进入事件循环，直到没有待处理任务
    } catch (const std::exception& e) {
        // 例如端口被占用、权限不足等，会在这里被捕获。
        std::cerr << "[server] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
