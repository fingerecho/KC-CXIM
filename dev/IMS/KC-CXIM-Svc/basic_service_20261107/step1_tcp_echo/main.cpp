#include <iostream>                        // std::cout / std::cerr：启动与错误日志
#include <cstdlib>                         // std::getenv / std::stoi：读取环境变量与端口参数
#include <string>                          // std::string：配置路径
#include <boost/asio/io_context.hpp>       // io_context：整个服务唯一的事件循环

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
//   1) 读 DBConfig.ini，初始化连接池（4 条）+ DB 工作线程池（2 个）；
//   2) 读 JWTConfig.json，构造全局共享的 TokenService；
//   3) 在同一 io_context 里创建 TCP server 与 HTTP server（两个 acceptor）；
//   4) io.run() 进入网络事件循环，且只跑一个线程。
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
        boost::asio::io_context io;                              // 单线程事件循环：TCP 与 HTTP 共用

        // 读取数据库配置并初始化 DB 层。
        kc::DbConfig cfg = kc::DbConfig::load(cfgPath);          // 解析 INI
        std::cout << "[db] connecting to " << cfg.host << ":" << cfg.port
                  << "/" << cfg.dbname << " as " << cfg.user << std::endl;
        kc::Db db(io, cfg, /*pool_size=*/4, /*worker_count=*/2); // 4 条连接 + 2 个 worker

        // 加载 JWT 配置并构造 token 服务：HTTP 登录签发、TCP 登录验签共用同一实例。
        kc::JwtConfig jwtCfg = kc::JwtConfig::load(jwtPath);     // 读取 jwt_secret / jwt_issuer
        kc::TokenService tokens(jwtCfg);                         // 无状态签名/验签服务

        // 同一个 io_context 里挂两个 acceptor：TCP 长连接 + HTTP 短连接。
        kc::server srv(io, port, db, tokens);                    // 创建 TCP 监听
        kc::http_server http(io, httpPort, db, tokens);          // 创建 HTTP 监听
        srv.run();                                               // 打印 TCP 端口
        http.run();                                              // 打印 HTTP 端口
        io.run();                                                // 单线程运行，不引入 io.run() 多线程
    } catch (const std::exception& e) {
        std::cerr << "[server] fatal: " << e.what() << std::endl; // 配置/启动失败
        return 1;                                                 // 非 0 退出码
    }
    return 0;                                                     // 正常退出
}
