#pragma once
#include <boost/asio.hpp>
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
//   3) 处理业务：登录、心跳、私聊转发；
//   4) 用写队列把回复/转发安全地写回 socket；
//   5) 连接断开时从 SessionManager 注销。
//
// 关于 enable_shared_from_this：
//   Boost.Asio 的异步回调可能在任意时刻才被调用。如果回调里只捕获 this，
//   而对象此时已被析构，就会出现"野指针"。继承 enable_shared_from_this 后，
//   可以在回调里调用 shared_from_this() 拿到一个 shared_ptr 并捕获它，
//   从而保证"回调还没执行完，对象就绝不会被销毁"。
// ============================================================================

namespace kc {

class SessionManager;   // 前向声明，避免头文件循环包含

class session : public std::enable_shared_from_this<session> {
public:
    // 构造时接管一条已建立的 TCP 连接，并绑定全局在线用户表。
    session(boost::asio::ip::tcp::socket socket, SessionManager& manager);

    // 启动读循环（由 server 在 accept 成功后调用）。
    void start();

    // 供"其他 session"调用：把一条消息投递给本连接（消息路由的落点）。
    void deliver(const Message& msg);

    // 关闭连接并注销在线状态。所有出错路径都统一走它。
    void close();

private:
    void do_read_header();                      // 读 4 字节长度头
    void do_read_body(std::uint32_t body_len);  // 按长度读 body
    void handle_message(const Message& msg);    // 业务分发
    void do_login(const Message& msg);          // 处理登录
    void send_message(const Message& msg);      // 把 Message 组帧后入写队列
    void do_write();                            // 从写队列取一帧写出

    boost::asio::ip::tcp::socket socket_;       // 底层 TCP 连接
    SessionManager&              manager_;      // 在线用户表（引用，server 持有它）

    // 分帧用缓冲区。header_ 固定 4 字节；body_ 每次按长度 resize。
    char                         header_[frame::HEADER_LEN];
    std::vector<char>            body_;

    // 写队列：所有要发给该客户端的数据都先进队列，再串行写出。
    // 为什么需要？因为可能同时有"登录回执"和"别人转发给我的消息"两路写操作，
    // 而同一个 socket 上并发 async_write 会导致数据交错/损坏。
    // 这里每个元素已经是"带 4 字节长度头的完整帧"。
    std::deque<std::string>      write_queue_;

    std::uint64_t                user_id_   = 0;      // 登录后的 userId
    bool                         logged_in_ = false;  // 是否已完成登录
};

} // namespace kc
