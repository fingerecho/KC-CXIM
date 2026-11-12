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

// ============================================================================
// 程序入口
// ----------------------------------------------------------------------------
// 启动顺序：
//   1) 读 DBConfig.ini，初始化连接池（100 条）+ DB 工作线程池（16 个）；
//   2) 读 JWTConfig.json，构造全局共享的 TokenService；
//   3) 在同一 io_context 里创建 TCP server 与 HTTP server（两个 acceptor）；
//   4) 用多个线程同时 run() 这个 io_context，充分利用多核处理高并发连接。
//
// 用法：
//   tcp_echo_server [tcp_port] [DBConfig.ini] [http_port]
//   环境变量：DB_CONFIG=... JWT_CONFIG=... HTTP_PORT=...
// ============================================================================

int main(int argc, char* argv[]) {
    unsigned short port = 8888;                                  // TCP 默认端口
    if (argc > 1) {                                              // 第一个参数：TCP 端口
        try { port = static_cast<unsigned short>(std::stoi(argv[1])); }   // 解析端口
        catch (...) { port = 8888; }                             // 解析失败回退默认值
    }

    std::string cfgPath = "DBConfig.ini";                        // 数据库配置默认路径
    if (const char* env = std::getenv("DB_CONFIG")) {            // 环境变量优先
        if (env[0] != '\0') cfgPath = env;                       // 非空才覆盖
    }
    if (argc > 2) cfgPath = argv[2];                             // 第二个参数覆盖

    unsigned short httpPort = 8080;                              // HTTP 默认端口
    if (const char* env = std::getenv("HTTP_PORT")) {            // 环境变量优先
        try { httpPort = static_cast<unsigned short>(std::stoi(env)); } catch (...) {}
    }
    if (argc > 3) {                                              // 第三个参数：HTTP 端口
        try { httpPort = static_cast<unsigned short>(std::stoi(argv[3])); } catch (...) {}
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
        /*************并发登录/注册优化：连接池与 DB worker 扩容***************/
        // 100 个好友并发注册/登录时，原 4 条连接 + 2 个 worker 会成为瓶颈。
        // 连接池改为“按需创建”：启动时只建 1 条，运行中最多扩到 poolSize。
        // 这样不会在启动瞬间触发 MariaDB "Too many connections"。
        // 可通过环境变量按部署环境调整：
        //   DB_POOL_SIZE   连接池上限（默认 100）
        //   DB_WORKER_COUNT  DB 工作线程数（默认 16）
        unsigned int poolSize = 100;
        unsigned int workerCount = 16;
        if (const char* env = std::getenv("DB_POOL_SIZE")) {
            try { int v = std::stoi(env); if (v > 0) poolSize = static_cast<unsigned int>(v); } catch (...) {}
        }
        if (const char* env = std::getenv("DB_WORKER_COUNT")) {
            try { int v = std::stoi(env); if (v > 0) workerCount = static_cast<unsigned int>(v); } catch (...) {}
        }
        // 注意：实际并发连接数不会超过 workerCount；poolSize 只是上限。
        // 若 MariaDB max_connections 较小，请同时调小 DB_WORKER_COUNT/DB_POOL_SIZE。
        kc::Db db(io, cfg, poolSize, workerCount);
        /*************并发登录/注册优化：连接池与 DB worker 扩容***************/

        // 加载 JWT 配置并构造 token 服务：HTTP 登录签发、TCP 登录验签共用同一实例。
        kc::JwtConfig jwtCfg = kc::JwtConfig::load(jwtPath);     // 读取 jwt_secret / jwt_issuer
        kc::TokenService tokens(jwtCfg);                         // 无状态签名/验签服务

        // 同一个 io_context 里挂两个 acceptor：TCP 长连接 + HTTP 短连接。
        kc::server srv(io, port, db, tokens);                    // 创建 TCP 监听
        // HTTP 层与 TCP 层共享同一个 SessionManager：
        // 好友申请/通过等写操作走 HTTP，完成后通过在线表把事件推给 TCP 长连接。
        kc::http_server http(io, httpPort, db, tokens, srv.sessions());
        srv.run();                                               // 打印 TCP 端口
        http.run();                                              // 打印 HTTP 端口

        /*************并发登录/注册优化：多线程运行 io_context***************/
        // 原来只在 main 线程里调用一次 io.run()，高并发时 accept/读写回调全部挤在
        // 单线程上。Boost.Asio 的 io_context 支持多线程同时 run()，这里按 CPU
        // 核数启动多个 io 线程（至少 4 个，最多 16 个）。
        unsigned int hw = std::thread::hardware_concurrency();
        unsigned int ioThreadCount = std::max(4u, hw);
        if (ioThreadCount > 16u) ioThreadCount = 16u;
        std::cout << "[server] io threads=" << ioThreadCount << std::endl;

        std::vector<std::thread> ioThreads;
        ioThreads.reserve(ioThreadCount);
        for (unsigned int i = 0; i < ioThreadCount; ++i) {
            ioThreads.emplace_back([&io, i] {
                try {
                    io.run();
                } catch (const std::exception& e) {
                    std::cerr << "[io] thread " << i << " exception: "
                              << e.what() << std::endl;
                }
            });
        }
        for (auto& t : ioThreads) {
            if (t.joinable()) t.join();
        }
        /*************并发登录/注册优化：多线程运行 io_context***************/
    } catch (const std::exception& e) {
        std::cerr << "[server] fatal: " << e.what() << std::endl; // 配置/启动失败
        return 1;                                                 // 非 0 退出码
    }
    return 0;                                                     // 正常退出
}
