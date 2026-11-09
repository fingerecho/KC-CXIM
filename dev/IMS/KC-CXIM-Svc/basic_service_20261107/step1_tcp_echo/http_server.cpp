#include "http_server.h"                       // 本模块声明

#include "db.h"                                // Db：把重活投递到 worker 线程
#include "token_service.h"                     // TokenService：登录成功后签发 JWT

#include <boost/beast/core.hpp>                // Beast 核心：flat_buffer / error_code 等
#include <boost/beast/http.hpp>                // Beast HTTP：request/response/async_read/write
#include <boost/beast/version.hpp>             // Beast 版本号（用于 Server 头）

#include "rapidjson/document.h"                // 解析请求 JSON
#include "rapidjson/stringbuffer.h"            // 生成响应 JSON 的缓冲区
#include "rapidjson/writer.h"                  // 把 JSON 写进缓冲区

#include <iostream>                            // 打印监听/错误日志
#include <memory>                              // std::shared_ptr 管理连接生命周期
#include <string>                              // std::string

namespace kc {

namespace beast = boost::beast;                // 简写 Beast 命名空间
namespace http  = beast::http;                 // 简写 HTTP 命名空间
namespace net   = boost::asio;                // 简写 Asio 命名空间
using tcp       = boost::asio::ip::tcp;       // TCP 类型简写

namespace {

// ---------------------------------------------------------------------------
// 统一响应 JSON 构造：所有接口都返回 {"code":..,"message":..,"data":..}
// ---------------------------------------------------------------------------
std::string make_error(const std::string& message) {
    rapidjson::StringBuffer sb;                                             // 输出缓冲区
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);                       // JSON 写入器
    w.StartObject();                                                        // 开始对象
    w.Key("code");    w.Int(1);                                             // code 非 0 表示失败
    w.Key("message"); w.String(message.c_str(), message.size());            // 失败原因
    w.EndObject();                                                          // 结束对象
    return std::string(sb.GetString(), sb.GetSize());                       // 转成 std::string
}

std::string make_register_ok(std::uint64_t userId) {
    rapidjson::StringBuffer sb;                                             // 输出缓冲区
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);                       // JSON 写入器
    w.StartObject();                                                        // 开始对象
    w.Key("code");    w.Int(0);                                             // code=0 成功
    w.Key("message"); w.String("ok");                                       // 成功提示
    w.Key("data");    w.StartObject();                                      // data 子对象
    w.Key("userId");  w.Uint64(userId);                                     // 返回数据库分配的 userId
    w.EndObject();                                                          // 结束 data
    w.EndObject();                                                          // 结束根对象
    return std::string(sb.GetString(), sb.GetSize());                       // 转成 std::string
}

std::string make_login_ok(std::uint64_t userId,
                          const std::string& username,
                          const std::string& token) {
    rapidjson::StringBuffer sb;                                             // 输出缓冲区
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);                       // JSON 写入器
    w.StartObject();                                                        // 开始对象
    w.Key("code");    w.Int(0);                                             // code=0 成功
    w.Key("message"); w.String("ok");                                       // 成功提示
    w.Key("data");    w.StartObject();                                      // data 子对象
    w.Key("userId");   w.Uint64(userId);                                    // 用户ID
    w.Key("username"); w.String(username.c_str(), username.size());         // 用户名
    w.Key("token");    w.String(token.c_str(), token.size());               // 供 TCP 登录使用的 JWT
    w.EndObject();                                                          // 结束 data
    w.EndObject();                                                          // 结束根对象
    return std::string(sb.GetString(), sb.GetSize());                       // 转成 std::string
}

// ---------------------------------------------------------------------------
// http_session：一条 HTTP 短连接（处理一个请求后即关闭）
// ---------------------------------------------------------------------------
class http_session : public std::enable_shared_from_this<http_session> {
public:
    http_session(tcp::socket socket, Db& db, TokenService& tokens)
        : socket_(std::move(socket)),                                      // 接管已 accept 的 socket
          db_(db),                                                         // 保存 DB 引用
          tokens_(tokens) {}                                               // 保存 JWT 服务引用

    void run() { do_read(); }                                              // 启动：开始异步读请求

private:
    // 异步读取一个完整的 HTTP 请求（Beast 会处理头部与 body 的分段到达）。
    void do_read() {
        auto self = shared_from_this();                                    // 保活：回调期间对象不被销毁
        http::async_read(socket_, buffer_, req_,                           // 读取到成员 req_/buffer_
            [self](beast::error_code ec, std::size_t /*bytes*/) {          // 读完/出错时回调
                if (ec) return;                                            // 读失败直接结束（短连接，无响应可发）
                self->handle_request();                                    // 成功则进入路由
            });
    }

    // 路由 + 参数校验。
    void handle_request() {
        std::string target(req_.target());                                 // 取出请求路径
        const auto qpos = target.find('?');                                // 忽略查询串
        if (qpos != std::string::npos) target = target.substr(0, qpos);    // 只保留路径部分

        if (req_.method() != http::verb::post) {                           // 注册/登录只接受 POST
            send_json(http::status::method_not_allowed,                    // 返回 405
                      make_error("method not allowed"));                    // 统一错误结构
            return;                                                        // 结束
        }

        std::string username, password;                                    // 解析出的凭据
        if (!parse_credentials(username, password)) return;                // 解析失败时内部已回 400

        if (target == "/api/register") {                                   // 注册路由
            do_register(username, password);                               // 异步走 DB worker
        } else if (target == "/api/login") {                               // 登录路由
            do_login(username, password);                                  // 异步走 DB worker
        } else {                                                           // 未知路径
            send_json(http::status::not_found, make_error("not found"));    // 返回 404
        }
    }

    // 从 req_.body() 解析 username/password，并做基础校验。
    bool parse_credentials(std::string& username, std::string& password) {
        rapidjson::Document doc;                                           // JSON 文档
        const std::string& body = req_.body();                             // 请求体
        doc.Parse(body.data(), body.size());                               // 带长度解析，避免越界
        if (doc.HasParseError() || !doc.IsObject()) {                      // 非合法 JSON 对象
            send_json(http::status::bad_request, make_error("invalid json"));   // 400
            return false;                                                  // 解析失败
        }
        if (!doc.HasMember("username") || !doc["username"].IsString() ||   // username 必须是字符串
            !doc.HasMember("password") || !doc["password"].IsString()) {   // password 必须是字符串
            send_json(http::status::bad_request, make_error("missing username/password"));   // 400
            return false;                                                  // 解析失败
        }
        username = doc["username"].GetString();                            // 取出用户名
        password = doc["password"].GetString();                            // 取出密码（仅在内存中短暂存在）

        if (username.empty() || username.size() > 64) {                    // 与 users.username VARCHAR(64) 对齐
            send_json(http::status::bad_request, make_error("invalid username"));   // 400
            return false;                                                  // 校验失败
        }
        if (password.size() < 6 || password.size() > 72) {                 // bcrypt 只安全处理前 72 字节
            send_json(http::status::bad_request, make_error("invalid password"));   // 400
            return false;                                                  // 校验失败
        }
        return true;                                                       // 参数合法
    }

    // 注册：投递到 DB worker；bcrypt 哈希也在 worker 里完成。
    void do_register(const std::string& username, const std::string& password) {
        auto self = shared_from_this();                                    // 保活到 DB 回调返回
        db_.registerUser(username, password,                               // 异步注册
            [self](bool ok, const std::string& err, std::uint64_t userId) { // 回调运行在 io_context 线程
                if (!ok) {                                                 // 注册失败
                    self->send_json(http::status::bad_request,             // 400
                                    make_error(err));                       // 返回原因
                    return;                                                // 结束
                }
                self->send_json(http::status::ok, make_register_ok(userId));   // 200 + userId
            });
    }

    // 登录：worker 里 bcrypt verify；成功后回到 io 线程签发 JWT。
    void do_login(const std::string& username, const std::string& password) {
        auto self = shared_from_this();                                    // 保活到 DB 回调返回
        db_.loginUser(username, password,                                  // 异步校验
            [self](bool ok, const std::string& err,                        // 回调运行在 io_context 线程
                   std::uint64_t userId, const std::string& name) {
                if (!ok) {                                                 // 登录失败
                    self->send_json(http::status::unauthorized,            // 401
                                    make_error(err));                      // 返回原因
                    return;                                                // 结束
                }
                const std::string token = self->tokens_.sign(userId, name);     // HS256 签发（微秒级，可在 io 线程）
                self->send_json(http::status::ok,                              // 200
                                make_login_ok(userId, name, token));           // 返回 token
            });
    }

    // 发送 JSON 响应，随后关闭连接（短连接）。
    void send_json(http::status status, std::string body) {
        auto self = shared_from_this();                                    // 保活到 async_write 完成
        auto res = std::make_shared<http::response<http::string_body>>(    // 用 shared_ptr 延长响应生命周期
            status, req_.version());                                       // 状态码 + 与请求一致的 HTTP 版本
        res->set(http::field::server, "KC-CXIM");                          // Server 响应头
        res->set(http::field::content_type, "application/json; charset=utf-8");   // 明确返回 JSON
        res->keep_alive(false);                                            // 短连接：回完即关
        res->body() = std::move(body);                                     // 填入响应体
        res->prepare_payload();                                            // 自动计算 Content-Length

        http::async_write(socket_, *res,                                   // 异步写回
            [self, res](beast::error_code ec, std::size_t /*bytes*/) {     // res 一起捕获，保证写期间存活
                beast::error_code ignored;                                 // 关闭时的错误可忽略
                self->socket_.shutdown(tcp::socket::shutdown_send, ignored);   // 优雅关闭发送方向
                self->socket_.close(ignored);                              // 关闭 socket
            });
    }

    beast::flat_buffer buffer_;                     // 读缓冲（Beast 负责处理粘包/半包）
    http::request<http::string_body> req_;          // 当前请求
    tcp::socket socket_;                            // 客户端连接
    Db& db_;                                        // DB 层引用
    TokenService& tokens_;                          // JWT 服务引用
};

} // namespace

// ---------------------------------------------------------------------------
// http_server
// ---------------------------------------------------------------------------
http_server::http_server(boost::asio::io_context& io, unsigned short port,
                         Db& db, TokenService& tokens)
    : acceptor_(io, tcp::endpoint(tcp::v4(), port)),   // 监听所有 IPv4 网卡的指定端口
      db_(db),                                          // 注入 DB
      tokens_(tokens) {                                 // 注入 JWT 服务
    do_accept();                                        // 立即投递第一次 accept
}

void http_server::run() {
    std::cout << "[http] listening on port "
              << acceptor_.local_endpoint().port() << std::endl;   // 打印真实端口（支持传 0 自动分配）
}

void http_server::do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {              // 异步接受一个连接
            if (!ec) {                                                  // 成功
                std::make_shared<http_session>(std::move(socket),       // 创建连接对象
                                               db_, tokens_)->run();    // 开始读请求
            }
            if (acceptor_.is_open()) do_accept();                       // 继续接受下一个连接
        });
}

} // namespace kc
