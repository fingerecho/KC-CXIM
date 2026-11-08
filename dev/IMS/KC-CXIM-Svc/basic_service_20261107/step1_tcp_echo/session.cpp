#include "session.h"
#include "session_manager.h"
#include "db.h"
#include "message_codec.h"

#include <cstring>
#include <iostream>

namespace kc {

// ---------------------------------------------------------------------------
// 构造：接管 socket，保存在线表 / 数据库层引用。
// idle_timer_ 必须用 socket_ 的 executor 构造，二者才会跑在同一个 io_context 上。
// ---------------------------------------------------------------------------
session::session(boost::asio::ip::tcp::socket socket, SessionManager& manager, Db& db)
    : socket_(std::move(socket)),
      idle_timer_(socket_.get_executor()),
      manager_(manager),
      db_(db) {}

void session::start() {
    do_read_header();
    reset_idle_timer();
}

// ---------------------------------------------------------------------------
// 读 4 字节长度头
// ---------------------------------------------------------------------------
void session::do_read_header() {
    auto self = shared_from_this();

    boost::asio::async_read(socket_, boost::asio::buffer(header_, frame::HEADER_LEN),
        [this, self](boost::system::error_code ec, std::size_t /*bytes_read*/) {
            if (ec) { close(); return; }

            auto len = frame::decode(header_);
            if (len == 0 || len > frame::MAX_BODY) { close(); return; }

            body_.resize(len);
            do_read_body(len);
        });
}

// ---------------------------------------------------------------------------
// 按长度读 body
// ---------------------------------------------------------------------------
void session::do_read_body(std::uint32_t body_len) {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(body_.data(), body_len),
        [this, self](boost::system::error_code ec, std::size_t /*bytes_read*/) {
            if (ec) { close(); return; }

            Message msg;
            std::string err;
            if (!parse_message(body_, msg, err)) {
                std::cerr << "[session] bad message: " << err << std::endl;
                close();
                return;
            }

            reset_idle_timer();
            handle_message(msg);

            // 读写解耦：处理完立刻继续读下一条。
            do_read_header();
        });
}

// ---------------------------------------------------------------------------
// 空闲超时
// ---------------------------------------------------------------------------
void session::reset_idle_timer() {
    if (closed_) return;

    idle_timer_.expires_after(IDLE_TIMEOUT);

    auto self = shared_from_this();
    idle_timer_.async_wait([this, self](boost::system::error_code ec) {
        if (ec) return;

        std::cout << "[timeout] user " << user_id_ << " idle > "
                  << IDLE_TIMEOUT.count() << "s, kick" << std::endl;
        close();
    });
}

// ---------------------------------------------------------------------------
// 业务分发
// ---------------------------------------------------------------------------
void session::handle_message(const Message& msg) {
    switch (msg.type) {
    case MsgType::Login:
        do_login(msg);
        break;

    case MsgType::Heartbeat:
        // 心跳已在 reset_idle_timer() 里刷新超时时间，不需要额外回应。
        break;

    case MsgType::Text:
    case MsgType::Image:
        do_send(msg);
        break;

    case MsgType::HistoryReq:
        do_history(msg);
        break;

    case MsgType::Ack:
        do_ack(msg);
        break;

    default:
        // 客户端不该发送 LoginAck/HistoryResp/SendAck/Error 等下行类型，忽略。
        break;
    }
}

// ---------------------------------------------------------------------------
// 登录：查 users 表校验凭据。注意这里把 result callback 放在 io_context 线程。
// ---------------------------------------------------------------------------
void session::do_login(const Message& msg) {
    if (msg.from == 0) {
        send_error("bad userId", msg.from);
        return;
    }
    if (logged_in_ || logging_in_) {
        send_error("already logged in", msg.from);
        return;
    }

    logging_in_ = true;
    auto self = shared_from_this();
    const std::uint64_t uid = msg.from;
    const std::string credential = msg.content;

    db_.login(uid, credential, [this, self, uid](bool ok, const std::string& err) {
        logging_in_ = false;
        if (closed_) return;

        if (!ok) {
            send_error(err.empty() ? "login failed" : err, uid);
            return;
        }

        user_id_   = uid;
        logged_in_ = true;

        // 重复登录：踢掉旧连接。
        auto old = manager_.add(uid, self);
        if (old && old.get() != this) {
            std::cout << "[login] user " << uid << " relogin, kick old" << std::endl;
            old->close();
        }

        std::cout << "[login] user " << uid
                  << " ok, online=" << manager_.size() << std::endl;

        Message ack;
        ack.type    = MsgType::LoginAck;
        ack.to      = uid;
        ack.content = "welcome";
        send_message(ack);

        // 登录成功后补推离线消息。
        push_offline();
    });
}

// ---------------------------------------------------------------------------
// 发消息：落库 + 在线投递
// ---------------------------------------------------------------------------
void session::do_send(const Message& msg) {
    if (!logged_in_) {
        send_error("login first", user_id_);
        return;
    }
    if (msg.to == 0) {
        send_error("bad recipient", user_id_);
        return;
    }

    // 以会话身份为准，不信任客户端传来的 from。
    const std::uint64_t sender     = user_id_;
    const std::uint64_t to         = msg.to;
    const int           type       = static_cast<int>(msg.type);
    const std::string   content    = msg.content;
    const std::uint64_t clientMsgId = msg.msg_id;   // 客户端临时消息号，用于去重

    auto self = shared_from_this();
    db_.sendMessage(sender, to, type, content, clientMsgId,
        [this, self, sender, to, type, content]
        (bool ok, const std::string& err, const StoredMessage& sm) {
            if (closed_) return;

            if (!ok) {
                send_error(err.empty() ? "send failed" : err, sender);
                return;
            }

            // 告诉发送方服务端分配的 msg_id / seq / ts
            Message ack;
            ack.type            = MsgType::SendAck;
            ack.to              = sender;
            ack.msg_id          = sm.msg_id;
            ack.conversation_id = sm.conversation_id;
            ack.seq             = sm.seq;
            ack.ts              = sm.ts;
            ack.content         = sm.duplicate ? "duplicate" : "sent";
            send_message(ack);

            // 接收者在线则直接投递；离线消息已经由 DB 层写入 offline_messages。
            if (auto peer = manager_.find(to)) {
                Message out;
                out.type            = static_cast<MsgType>(type);
                out.from            = sender;
                out.to              = to;
                out.msg_id          = sm.msg_id;
                out.conversation_id = sm.conversation_id;
                out.seq             = sm.seq;
                out.ts              = sm.ts;
                out.content         = content;
                peer->deliver(out);

                std::cout << "[route] " << sender << " -> " << to
                          << " : " << content << std::endl;
            } else {
                std::cout << "[route] user " << to
                          << " offline, stored for push" << std::endl;
            }
        });
}

// ---------------------------------------------------------------------------
// 历史分页
// ---------------------------------------------------------------------------
void session::do_history(const Message& msg) {
    if (!logged_in_) {
        send_error("login first", user_id_);
        return;
    }
    if (msg.to == 0) {
        send_error("bad peer", user_id_);
        return;
    }

    int limit = 20;
    if (!msg.content.empty()) {
        try { limit = std::stoi(msg.content); }
        catch (...) { limit = 20; }
    }

    const std::uint64_t uid    = user_id_;
    const std::uint64_t peer   = msg.to;
    const std::uint64_t before = msg.seq;   // 0 = 最新

    auto self = shared_from_this();
    db_.fetchHistory(uid, peer, before, limit,
        [this, self, uid](bool ok, const std::string& err,
                          std::vector<StoredMessage> msgs) {
            if (closed_) return;
            if (!ok) {
                send_error(err.empty() ? "history failed" : err, uid);
                return;
            }

            Message resp;
            resp.type    = MsgType::HistoryResp;
            resp.from    = 0;
            resp.to      = uid;
            resp.content = serialize_history(msgs);
            send_message(resp);
        });
}

// ---------------------------------------------------------------------------
// 投递/已读回执
// ---------------------------------------------------------------------------
void session::do_ack(const Message& msg) {
    if (!logged_in_) return;             // 未登录静默忽略
    if (msg.conversation_id == 0 || msg.seq == 0) return;

    const std::uint64_t uid  = user_id_;
    const std::uint64_t conv = msg.conversation_id;
    const std::uint64_t seq  = msg.seq;
    // content = "delivered" 表示只确认投递；否则同时推进已读游标。
    const bool markRead = (msg.content != "delivered");

    auto self = shared_from_this();
    db_.updateReceipt(uid, conv, seq, markRead ? seq : 0,
        [this, self](bool ok, const std::string& err) {
            if (!ok) std::cerr << "[ack] " << err << std::endl;
        });
}

// ---------------------------------------------------------------------------
// 登录后补推离线消息（客户端收到后应回 Ack，服务端再删除队列）
// ---------------------------------------------------------------------------
void session::push_offline() {
    const std::uint64_t uid = user_id_;
    auto self = shared_from_this();

    db_.fetchOffline(uid, 200,
        [this, self, uid](bool ok, const std::string& err,
                          std::vector<StoredMessage> msgs) {
            if (closed_) return;
            if (!ok) {
                std::cerr << "[offline] " << err << std::endl;
                return;
            }

            for (const auto& sm : msgs) {
                Message m;
                m.type            = sm.type;
                m.from            = sm.from;
                m.to              = uid;
                m.msg_id          = sm.msg_id;
                m.conversation_id = sm.conversation_id;
                m.seq             = sm.seq;
                m.ts              = sm.ts;
                m.content         = sm.content;
                send_message(m);
            }

            if (!msgs.empty()) {
                std::cout << "[offline] pushed " << msgs.size()
                          << " message(s) to user " << uid << std::endl;
            }
        });
}

// ---------------------------------------------------------------------------
// 投递入口
// ---------------------------------------------------------------------------
void session::deliver(const Message& msg) { send_message(msg); }

// ---------------------------------------------------------------------------
// 错误通知
// ---------------------------------------------------------------------------
void session::send_error(const std::string& text, std::uint64_t to) {
    Message e;
    e.type    = MsgType::Error;
    e.to      = to;
    e.content = text;
    send_message(e);
}

// ---------------------------------------------------------------------------
// 发送：序列化 -> 组帧 -> 入队 -> 必要时启动写
// ---------------------------------------------------------------------------
void session::send_message(const Message& msg) {
    std::string payload = serialize_message(msg);

    std::string framed(frame::HEADER_LEN + payload.size(), '\0');
    frame::encode(static_cast<std::uint32_t>(payload.size()), framed.data());
    std::memcpy(framed.data() + frame::HEADER_LEN, payload.data(), payload.size());

    bool idle = write_queue_.empty();
    write_queue_.push_back(std::move(framed));
    if (idle) do_write();
}

// ---------------------------------------------------------------------------
// 写队列串行化
// ---------------------------------------------------------------------------
void session::do_write() {
    auto self = shared_from_this();

    boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
        [this, self](boost::system::error_code ec, std::size_t /*bytes_written*/) {
            if (ec) { close(); return; }

            write_queue_.pop_front();
            if (!write_queue_.empty()) do_write();
        });
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------
void session::close() {
    if (closed_) return;
    closed_ = true;

    boost::system::error_code ignored;
    try {
        idle_timer_.cancel();
    } catch (...) {
        // ignore
    }

    if (logged_in_) {
        manager_.remove(user_id_);
        logged_in_ = false;
        std::cout << "[logout] user " << user_id_
                  << ", online=" << manager_.size() << std::endl;
    }

    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

} // namespace kc
