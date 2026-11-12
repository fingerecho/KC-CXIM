#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "frame.h"
#include "message.h"

// ============================================================================
// session：一条客户端连接（一个 socket）的完整生命周期封装
// ----------------------------------------------------------------------------
// 它负责：
//   1) 按"4 字节长度头 + body"的协议不断读消息（解决粘包/半包）；
//   2) 把 body 解析成 Message（JSON）；
//   3) 处理业务：登录鉴权、发消息落库、离线补推、历史分页、回执；
//   4) 用写队列把回复/转发安全地写回 socket；
//   5) 空闲超时踢人：超过 IDLE_TIMEOUT 没收到任何消息就断开；
//   6) 连接断开时从 SessionManager 注销。
//
// 线程模型（数据库接入后）：
//   - io_context 可能由多个线程同时 run()；
//   - 每条连接用独立的 strand_ 串行化自己的读/写/DB 回调，避免多线程数据竞争；
//   - DB worker 线程：执行阻塞 SQL；
//   - DB 完成后通过 boost::asio::post 回到 io_context，再切回本连接的 strand。
//   - deliver() 会把消息 post 到目标连接的 strand 上。
// ============================================================================

namespace kc {

class SessionManager;   // 前向声明，避免头文件循环包含
class Db;               // 数据库访问层
class TokenService;     // JWT 签发/校验服务

class session : public std::enable_shared_from_this<session> {
public:
    static constexpr std::chrono::seconds IDLE_TIMEOUT{60};

    // 构造时接管一条已建立的 TCP 连接，并绑定在线用户表、数据库层与 JWT 服务。
    session(boost::asio::ip::tcp::socket socket, SessionManager& manager,
            Db& db, TokenService& tokens);

    void start();

    // 供"其他 session"调用：把一条消息投递给本连接（消息路由的落点）。
    void deliver(const Message& msg);

    void close();

private:
    void do_read_header();
    void do_read_body(std::uint32_t body_len);
    void handle_message(const Message& msg);

    void do_login(const Message& msg);      // 登录：校验 JWT（不查库、不比对密码）
    void do_send(const Message& msg);       // 发消息：落库 + 在线投递
    void do_history(const Message& msg);    // 历史分页
    void do_ack(const Message& msg);        // 投递/已读回执
    void push_offline();                    // 登录成功后补推离线消息
    void push_friend_events();              // 登录成功后补推好友申请/处理结果

    void send_message(const Message& msg);  // 组帧后入写队列
    void send_error(const std::string& text, std::uint64_t to);
    void do_write();
    void reset_idle_timer();

    // 真正的关闭逻辑；close() 负责把它切回本连接的 strand 再执行。
    void do_close();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer     idle_timer_;

    /*************并发登录/注册优化：每条连接一个 strand***************/
    // 多线程 run() 同一个 io_context 时，本连接的读/写/DB 回调可能落到不同线程；
    // strand 保证这些回调串行执行，session 内部状态无需额外加锁。
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    /*************并发登录/注册优化：每条连接一个 strand***************/

    SessionManager&               manager_;
    Db&                           db_;
    TokenService&                 tokens_;   // JWT 校验能力（登录时验签，不再查库比对密码）

    char                          header_[frame::HEADER_LEN];
    std::vector<char>             body_;

    std::deque<std::string>       write_queue_;

    std::uint64_t                 user_id_   = 0;
    bool                          logged_in_ = false;
    bool                          logging_in_ = false;  // 防止登录请求并发处理
    bool                          closed_    = false;
};

} // namespace kc
