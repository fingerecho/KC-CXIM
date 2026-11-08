#include <iostream>
#include <cstdlib>
#include <boost/asio/io_context.hpp>

#include "server.h"
#include "db.h"
#include "db_config.h"

// ============================================================================
// 程序入口
// ----------------------------------------------------------------------------
// 启动顺序：
//   1) 读 DBConfig.ini，初始化连接池（4 条）+ DB 工作线程池（2 个）；
//   2) 创建 server（监听端口、开始 accept）；
//   3) io.run() 进入网络事件循环。
//
// 用法：
//   tcp_echo_server [port] [DBConfig.ini]
//   或设置环境变量 DB_CONFIG=/path/to/DBConfig.ini
// ============================================================================

int main(int argc, char* argv[]) {
    unsigned short port = 8888;
    if (argc > 1) {
        try { port = static_cast<unsigned short>(std::stoi(argv[1])); }
        catch (...) { port = 8888; }
    }

    std::string cfgPath = "DBConfig.ini";
    if (const char* env = std::getenv("DB_CONFIG")) {
        if (env[0] != '\0') cfgPath = env;
    }
    if (argc > 2) cfgPath = argv[2];

    try {
        boost::asio::io_context io;

        // 读取数据库配置并初始化 DB 层。
        kc::DbConfig cfg = kc::DbConfig::load(cfgPath);
        std::cout << "[db] connecting to " << cfg.host << ":" << cfg.port
                  << "/" << cfg.dbname << " as " << cfg.user << std::endl;
        kc::Db db(io, cfg, /*pool_size=*/4, /*worker_count=*/2);

        kc::server srv(io, port, db);
        srv.run();
        io.run();
    } catch (const std::exception& e) {
        std::cerr << "[server] fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
