#include "session.h"
#include "session_manager.h"
#include "db.h"
#include "friend_types.h"    // 好友事件 JSON 序列化
#include "message_codec.h"
#include "token_service.h"   // JWT 校验：TCP 登录只验 token，不再查库比对密码

#include <cstring>
#include <iostream>

namespace kc {

// ---------------------------------------------------------------------------
// 构造：接管 socket，保存在线表 / 数据库层引用。
// idle_timer_ 必须用 socket_ 的 executor 构造，二者才会跑在同一个 io_context 上。
// ---------------------------------------------------------------------------
session::session(boost::asio::ip::tcp::socket socket, SessionManager& manager,
                 Db& db, TokenService& tokens)
    : socket_(std::move(socket)),
      idle_timer_(socket_.get_executor()),
      strand_(boost::asio::make_strand(socket_.get_executor())),
      manager_(manager),
      db_(db),
      tokens_(tokens) {}

void session::start() {
    /*************并发登录/注册优化：start 也 post 到 strand***************/
    auto self = shared_from_this();
    boost::asio::post(strand_, [self] {
        if (self->closed_) return;
        self->do_read_header();
        self->reset_idle_timer();
    });
    /*************并发登录/注册优化：start 也 post 到 strand***************/
}

// ---------------------------------------------------------------------------
// 读 4 字节长度头
// ---------------------------------------------------------------------------
void session::do_read_header() {
    auto self = shared_from_this();

    boost::asio::async_read(socket_, boost::asio::buffer(header_, frame::HEADER_LEN),
        boost::asio::bind_executor(strand_,
        [this, self](boost::system::error_code ec, std::size_t /*bytes_read*/) {
            if (ec) { close(); return; }

            auto len = frame::decode(header_);
            if (len == 0 || len > frame::MAX_BODY) { close(); return; }

            body_.resize(len);
            do_read_body(len);
        }));
}

// ---------------------------------------------------------------------------
// 按长度读 body
// ---------------------------------------------------------------------------
void session::do_read_body(std::uint32_t body_len) {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(body_.data(), body_len),
        boost::asio::bind_executor(strand_,
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
        }));
}

// ---------------------------------------------------------------------------
// 空闲超时
// ---------------------------------------------------------------------------
void session::reset_idle_timer() {
    if (closed_) return;

    idle_timer_.expires_after(IDLE_TIMEOUT);

    auto self = shared_from_this();
    idle_timer_.async_wait(boost::asio::bind_executor(strand_,
        [this, self](boost::system::error_code ec) {
            if (ec) return;

            std::cout << "[timeout] user " << user_id_ << " idle > "
                      << IDLE_TIMEOUT.count() << "s, kick" << std::endl;
            close();
        }));
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
// 登录：msg.content 是 HTTP 登录后签发的 JWT。这里只做验签 + 过期检查，
// 不再查库、不再比对密码，因此完全不占用 DB worker，也不阻塞 io 线程。
// 注意：身份以 token 里的 sub 为准，客户端传来的 msg.from 不再可信。
// ---------------------------------------------------------------------------
void session::do_login(const Message& msg) {
    if (logged_in_ || logging_in_) {                    // 已登录 / 正在登录
        send_error("already logged in", msg.from);      // 拒绝重复登录
        return;                                         // 直接返回
    }

    std::uint64_t uid = 0;                              // 从 token 解析出的用户ID
    std::string username;                               // 从 token 解析出的用户名（仅日志用）

    // 同步验签：HS256 是微秒级运算，可以安全地放在 io_context 线程；
    // 失败原因（签名错/过期/格式错）统一对外返回同一条错误，避免泄露细节。
    if (!tokens_.verify(msg.content, uid, username)) {
        send_error("invalid or expired token", msg.from);   // 校验失败
        return;                                             // 结束
    }

    user_id_   = uid;                                   // 以 token 中的身份为准
    logged_in_ = true;                                  // 标记已登录
    logging_in_ = false;                                // 清除登录中标记

    auto self = shared_from_this();                     // 取得自身的 shared_ptr

    // 重复登录：踢掉旧连接。
    auto old = manager_.add(uid, self);                 // 登记/替换在线表
    if (old && old.get() != this) {                     // 存在另一条旧连接
        std::cout << "[login] user " << uid << " relogin, kick old" << std::endl;   // 日志
        old->close();                                   // 关闭旧连接
    }

    std::cout << "[login] user " << uid                   // 打印登录成功（附用户名）
              << " (" << username << ") ok, online="
              << manager_.size() << std::endl;

    Message ack;                                        // 构造登录成功回执
    ack.type    = MsgType::LoginAck;                    // 类型：登录响应
    ack.to      = uid;                                  // 接收者就是本人
    ack.content = "welcome";                            // 欢迎语
    send_message(ack);                                  // 发送回执

    // 登录成功后补推离线消息。
    push_offline();                                     // 复用原有离线补推逻辑

    // 好友事件的离线补偿：待处理申请 + 申请人还没收到的处理结果。
    // 这两类事件不适合塞进 offline_messages（那是聊天消息表），
    // 因此直接用 friend_requests 的状态字段作为“持久化事件队列”。
    push_friend_events();
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
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, sender, to, type, content, ok, err, sm]() {
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
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
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
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, uid, ok, err, msgs = std::move(msgs)]() {
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
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
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
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, uid, ok, err, msgs = std::move(msgs)]() {
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
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
        });
}

// ---------------------------------------------------------------------------
// 登录后补推好友事件
// ---------------------------------------------------------------------------
// 顺序：
//   1) 拉取“别人发给我的待处理申请”，逐条推 FriendRequest(event=pending)；
//   2) 拉取“我发出的申请里已通过/已拒绝但还没通知我的”，推 FriendRequestResult；
//   3) 把第 2 步的结果标记为已通知，避免下次登录重复推送。
//
// 为什么用两级回调而不是一次性 join？
//   两类数据来自同一张表的两种查询条件，分开查语义更清晰，也方便分别限流。
//   DB 回调可能在任意 io 线程执行，因此每个回调都 post 回本连接的 strand，
//   再调用 send_message()，保证写队列只被一个线程访问。
// ---------------------------------------------------------------------------
void session::push_friend_events() {
    const std::uint64_t uid = user_id_;
    auto self = shared_from_this();

    db_.fetchPendingIncoming(uid, 100,
        [this, self, uid](bool ok, const std::string& err,
                          std::vector<FriendRequestInfo> reqs) {
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, uid, ok, err, reqs = std::move(reqs)]() {
            if (closed_) return;
            if (!ok) {
                std::cerr << "[friend] fetch pending failed: " << err << std::endl;
            } else {
                for (const auto& r : reqs) {
                    Message m;
                    m.type    = MsgType::FriendRequest;
                    m.from    = r.requester_id;
                    m.to      = uid;
                    m.msg_id  = r.id;          // 复用 msg_id 携带 request_id，便于客户端处理
                    m.ts      = r.created_ts;
                    m.content = serialize_friend_request_event(r, "pending");
                    send_message(m);
                }
                if (!reqs.empty()) {
                    std::cout << "[friend] pushed " << reqs.size()
                              << " pending request(s) to user " << uid << std::endl;
                }
            }

            // 第 2 步：补推“已处理、未通知”的结果。
            db_.fetchUnnotifiedResults(uid, 100,
                [this, self, uid](bool ok2, const std::string& err2,
                                  std::vector<FriendRequestInfo> results) {
                    boost::asio::post(strand_,
                        [this, self, uid, ok2, err2, results = std::move(results)]() {
                    if (closed_) return;
                    if (!ok2) {
                        std::cerr << "[friend] fetch results failed: " << err2 << std::endl;
                        return;
                    }

                    for (const auto& r : results) {
                        Message m;
                        m.type    = MsgType::FriendRequestResult;
                        m.from    = r.target_id;
                        m.to      = uid;
                        m.msg_id  = r.id;
                        m.ts      = r.created_ts;
                        m.content = serialize_friend_result_event(
                                        r, r.status == 1 ? "accepted" : "rejected");
                        send_message(m);
                    }

                    if (!results.empty()) {
                        std::cout << "[friend] pushed " << results.size()
                                  << " request result(s) to user " << uid << std::endl;
                        // 标记已通知：即使本次连接随后断开，也不会重复推送。
                        db_.markResultsNotified(uid,
                            [](bool ok3, const std::string& err3) {
                                if (!ok3) {
                                    std::cerr << "[friend] mark notified failed: "
                                              << err3 << std::endl;
                                }
                            });
                    }
                        });
                    /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
                });
                });
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
        });
}

// ---------------------------------------------------------------------------
// 投递入口
// ---------------------------------------------------------------------------
void session::deliver(const Message& msg) {
    /*************并发登录/注册优化：投递切回目标连接的 strand***************/
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, msg] {
        if (self->closed_) return;
        self->send_message(msg);
    });
    /*************并发登录/注册优化：投递切回目标连接的 strand***************/
}

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
        boost::asio::bind_executor(strand_,
        [this, self](boost::system::error_code ec, std::size_t /*bytes_written*/) {
            if (ec) { close(); return; }

            write_queue_.pop_front();
            if (!write_queue_.empty()) do_write();
        }));
}

// ---------------------------------------------------------------------------
// 关闭：可能从本连接的回调、也可能从“踢旧连接”的另一个 session 线程调用。
// 统一切回本连接的 strand 再执行真正的关闭，保证 closed_/socket_ 不被并发访问。
// ---------------------------------------------------------------------------
void session::close() {
    if (strand_.running_in_this_thread()) {
        do_close();
        return;
    }
    auto self = shared_from_this();
    boost::asio::post(strand_, [self] { self->do_close(); });
}

void session::do_close() {
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
