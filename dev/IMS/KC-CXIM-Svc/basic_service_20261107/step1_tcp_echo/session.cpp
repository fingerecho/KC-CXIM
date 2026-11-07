#include "session.h"
#include "session_manager.h"
#include "message_codec.h"

#include <cstring>    // std::memcpy
#include <iostream>   // std::cout / std::cerr

namespace kc {

// ---------------------------------------------------------------------------
// 构造函数：接管 socket，并保存在线表引用。
// std::move(socket) 把 socket 所有权转移进来，避免复制（socket 不可复制）。
// ---------------------------------------------------------------------------
session::session(boost::asio::ip::tcp::socket socket, SessionManager& manager)
    : socket_(std::move(socket)), manager_(manager) {}

// 启动读循环：先读长度头。之后的读会在回调里一环扣一环地继续。
void session::start() { do_read_header(); }

// ---------------------------------------------------------------------------
// 读 4 字节长度头
// ---------------------------------------------------------------------------
void session::do_read_header() {
    // 【生命周期关键】异步操作期间必须保证 this 还活着。
    // shared_from_this() 拿到一个强引用 self，并让 lambda 捕获它；
    // 只要这个读操作没完成，self 就存在，session 就不会被销毁。
    auto self = shared_from_this();

    // async_read 会"尽量读满"HEADER_LEN 字节才回调，不存在只读到一部分的问题。
    // 回调是稍后由 io_context.run() 调度的，不是立即执行。
    boost::asio::async_read(socket_, boost::asio::buffer(header_, frame::HEADER_LEN),
        [this, self](boost::system::error_code ec, std::size_t /*bytes_read*/) {
            if (ec) { close(); return; }   // 连接断开/出错 -> 注销并关闭

            // 还原 body 长度，并做合法性校验，避免恶意超长导致内存爆炸。
            auto len = frame::decode(header_);
            if (len == 0 || len > frame::MAX_BODY) { close(); return; }

            body_.resize(len);             // 精确分配本次 body 的空间
            do_read_body(len);             // 继续读 body
        });
}

// ---------------------------------------------------------------------------
// 按长度读 body（长度来自上面的 header）
// ---------------------------------------------------------------------------
void session::do_read_body(std::uint32_t body_len) {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(body_.data(), body_len),
        [this, self](boost::system::error_code ec, std::size_t /*bytes_read*/) {
            if (ec) { close(); return; }

            // 此时 body_ 里是一条完整的消息体（JSON），开始解析。
            Message msg;
            std::string err;
            if (!parse_message(body_, msg, err)) {
                std::cerr << "[session] bad message: " << err << std::endl;
                close();       // 协议错误：直接断开，避免脏数据继续流入
                return;
            }

            handle_message(msg);   // 交给业务层处理

            // 读和写是解耦的：处理完立刻继续读下一条，不等待"回复是否写完"。
            do_read_header();
        });
}

// ---------------------------------------------------------------------------
// 业务分发：根据消息类型决定怎么处理
// ---------------------------------------------------------------------------
void session::handle_message(const Message& msg) {
    switch (msg.type) {
    case MsgType::Login:
        do_login(msg);
        break;

    case MsgType::Heartbeat:
        // 心跳目前不需要回应；真实场景可回一个 ack 或刷新"最后活跃时间"。
        break;

    case MsgType::Text:
    case MsgType::Image: {
        // 未登录不允许发消息（简单鉴权）。
        if (!logged_in_) {
            Message e;
            e.type    = MsgType::Error;
            e.to      = msg.from;
            e.content = "login first";
            send_message(e);
            return;
        }

        // 核心：按接收者 userId 在在线表里查连接。
        // find 返回的是 shared_ptr，保证投递期间对方连接不会被析构。
        if (auto peer = manager_.find(msg.to)) {
            peer->deliver(msg);    // 找到连接 -> 转发
            std::cout << "[route] " << msg.from << " -> " << msg.to
                      << " : " << msg.content << std::endl;
        } else {
            // 对方不在线。真实 IM 这里应先写离线消息库，等上线再推。
            std::cout << "[route] user " << msg.to << " offline" << std::endl;
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 登录处理：把自己登记到 SessionManager
// ---------------------------------------------------------------------------
void session::do_login(const Message& msg) {
    // userId 为 0 视为非法（0 留给系统消息用）。
    if (msg.from == 0) return;

    // 真实项目应在这里校验 msg.content 里的 token（查库/验签），失败则回 Error。
    user_id_   = msg.from;
    logged_in_ = true;

    auto self = shared_from_this();

    // 登记。若同一 userId 已在线，add 返回旧连接。
    auto old = manager_.add(user_id_, self);
    if (old && old.get() != this) {
        // 重复登录：踢掉旧连接（在 manager 的锁之外执行，避免死锁）。
        std::cout << "[login] user " << user_id_ << " relogin, kick old" << std::endl;
        old->close();
    }

    std::cout << "[login] user " << user_id_
              << " ok, online=" << manager_.size() << std::endl;

    // 回一条登录成功回执，客户端可用它判断是否登录成功。
    Message ack;
    ack.type    = MsgType::LoginAck;
    ack.to      = user_id_;
    ack.content = "welcome";
    send_message(ack);
}

// ---------------------------------------------------------------------------
// 供其他 session 调用的投递入口
// ---------------------------------------------------------------------------
void session::deliver(const Message& msg) { send_message(msg); }

// ---------------------------------------------------------------------------
// 发送：序列化 -> 组帧（补 4 字节长度头）-> 入写队列 -> 必要时启动写
// ---------------------------------------------------------------------------
void session::send_message(const Message& msg) {
    // 1) 结构体 -> JSON 文本（这只是"消息体"，还没有长度头）
    std::string payload = serialize_message(msg);

    // 2) 【关键修复】必须补上 4 字节长度头，否则对端会把 JSON 前 4 字节
    //    误当成长度，导致解析错乱、卡死。
    //    framed 布局：[4 字节长度][payload...]
    std::string framed(frame::HEADER_LEN + payload.size(), '\0');
    frame::encode(static_cast<std::uint32_t>(payload.size()), framed.data());
    std::memcpy(framed.data() + frame::HEADER_LEN, payload.data(), payload.size());

    // 3) 入队。如果当前队列为空，说明没有正在进行的写，需要主动启动；
    //    否则说明 do_write 的链式回调还在跑，它会自动接着发，无需重复启动。
    bool idle = write_queue_.empty();
    write_queue_.push_back(std::move(framed));   // move 避免拷贝
    if (idle) do_write();
}

// ---------------------------------------------------------------------------
// 从写队列取队首帧写出。一次只发一帧，发完再发下一帧（串行化）。
// ---------------------------------------------------------------------------
void session::do_write() {
    auto self = shared_from_this();

    // 注意：这里 buffer 引用的是 deque 里那个 std::string 的内容。
    // deque 的 push_back 不会使已有元素的引用失效，且此帧在发送完成前
    // 不会被 pop，所以缓冲区始终有效。
    boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
        [this, self](boost::system::error_code ec, std::size_t /*bytes_written*/) {
            if (ec) { close(); return; }

            write_queue_.pop_front();          // 这一帧发完了，移除
            if (!write_queue_.empty()) do_write();  // 还有 -> 继续发下一帧
        });
}

// ---------------------------------------------------------------------------
// 关闭连接：先注销在线状态，再关闭 socket。可被重复调用（幂等）。
// ---------------------------------------------------------------------------
void session::close() {
    if (!socket_.is_open()) return;   // 已经关过了，直接返回

    if (logged_in_) {
        manager_.remove(user_id_);    // 从在线表删除，之后 find 查不到
        logged_in_ = false;
        std::cout << "[logout] user " << user_id_
                  << ", online=" << manager_.size() << std::endl;
    }

    // shutdown 会先发送 FIN 优雅关闭；再用 close 释放本端 fd/socket。
    // 都用 error_code 版本，避免异常从析构路径抛出。
    boost::system::error_code ignored;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

} // namespace kc
