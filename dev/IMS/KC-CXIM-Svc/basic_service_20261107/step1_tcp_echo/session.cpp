#include "session.h"
#include "session_manager.h"
#include "db.h"
#include "friend_types.h"    // 好友事件 JSON 序列化
#include "group_types.h"     // /*************群聊功能：群事件 JSON 序列化***************/
#include "file_types.h"      // /*************图片文件功能（七牛云直传）：媒体元数据校验/重建***************/
#include "message_codec.h"
#include "token_service.h"   // JWT 校验：TCP 登录只验 token，不再查库比对密码
#include "quiet_log.h"       // logFatal：踢连接/预算越线这类事件不能被 QUIET_LOG 吞掉

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace kc {

namespace {

// P0-4：全局存活连接计数。构造 +1、析构 -1，天然平衡（不依赖 close 路径）。
std::atomic<long> g_liveSessions{0};

// P0-4：所有 session 待写帧的总字节数（进程级内存预算）。
// 只在"入队 / 写完成出队 / 连接销毁清队"三处变动，前两处见 send_message 与 do_write，
// 第三处必须存在：否则连接 churn 之后预算会单向漂到"永远满载"，把好连接也踢掉。
std::atomic<std::size_t> g_writeQueueTotalBytes{0};

// 单连接在途 DB 任务上限（TCP_MAX_OUTSTANDING，默认 64）。
int maxOutstandingJobs() {
    static const int n = [] {
        const char* v = std::getenv("TCP_MAX_OUTSTANDING");
        if (!v || !*v) return session::kMaxOutstandingDefault;
        try {
            int x = std::stoi(v);
            return x > 0 ? x : session::kMaxOutstandingDefault;
        } catch (...) { return session::kMaxOutstandingDefault; }
    }();
    return n;
}

// 空闲踢人秒数（IDLE_TIMEOUT_SEC，默认 60，钳制到 [15, 3600]）。
int idleTimeoutSec() {
    static const int n = [] {
        const char* v = std::getenv("IDLE_TIMEOUT_SEC");
        if (!v || !*v) return static_cast<int>(session::IDLE_TIMEOUT.count());
        try {
            int x = std::stoi(v);
            if (x < 15) x = 15;
            if (x > 3600) x = 3600;
            return x;
        } catch (...) { return static_cast<int>(session::IDLE_TIMEOUT.count()); }
    }();
    return n;
}

} // namespace

long session::liveCount() { return g_liveSessions.load(std::memory_order_relaxed); }

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
      tokens_(tokens),
      idle_timeout_(idleTimeoutSec()) {
    g_liveSessions.fetch_add(1, std::memory_order_relaxed);
}

session::~session() {
    g_liveSessions.fetch_sub(1, std::memory_order_relaxed);
}

void session::start() {
    /*************P1-5：关闭 Nagle***************/
    // IM 的报文都是"几十字节请求 -> 几十字节回执"，Nagle 与对端延迟 ACK 叠加
    // 会造出 40~200ms 的台阶式停顿（压测数据里那一簇 ~3.1s 的长尾，
    // 至少有一部分就是这种停顿撞上重传造成的）。
    boost::system::error_code ec;
    socket_.set_option(boost::asio::ip::tcp::no_delay(true), ec);
    // 应用层已有心跳 + 空闲踢人，SO_KEEPALIVE 只是兜住"半开连接"这种
    // 心跳也发不出来的情况，失败不影响主流程。
    socket_.set_option(boost::asio::socket_base::keep_alive(true), ec);
    /*************P1-5：关闭 Nagle***************/

    /*************并发登录/注册优化：start 也 post 到 strand***************/
    auto self = shared_from_this();
    boost::asio::post(strand_, [self] {
        if (self->closed_) return;
        self->do_read_header();
        self->reset_idle_timer();
    });
    /*************并发登录/注册优化：start 也 post 到 strand***************/
}

// ============================================================================
// P0-4：单连接在途 DB 任务配额
// ----------------------------------------------------------------------------
// 客户端可以在一条连接上连续狂发消息，每条消息都会变成一个 DB 任务。
// 没有上限时，一条"话痨"连接就能把整条 SQL 队列占满，殃及所有在线用户。
// 配额按连接分配、回调释放，超限时直接回 Error，让客户端自己降速/重试。
// ============================================================================
bool session::reserveJob() {
    if (closed_) return false;
    if (outstanding_ >= maxOutstandingJobs()) return false;
    ++outstanding_;
    return true;
}

void session::releaseJob() {
    if (outstanding_ > 0) --outstanding_;
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

    // idle_timeout_ 来自 IDLE_TIMEOUT_SEC；客户端心跳间隔必须显著小于它
    // （压测/桌面端默认已调成 30s，见 readme.md）。
    idle_timer_.expires_after(idle_timeout_);

    auto self = shared_from_this();
    idle_timer_.async_wait(boost::asio::bind_executor(strand_,
        [this, self](boost::system::error_code ec) {
            if (ec) return;

            std::cout << "[timeout] user " << user_id_ << " idle > "
                      << idle_timeout_.count() << "s, kick" << std::endl;
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

    /*************图片文件功能（七牛云直传）：File=4 与 Text/Image 同一入口***************/
    // 单聊/群聊由 is_group 区分，媒体/纯文本由 do_send* 内部再分流——
    // 投递、离线补推、历史、回执链路对 type 完全无感（type 原样透传），
    // 媒体特有的只是"先验权属、再重建元数据"这一前置步骤。
    case MsgType::Text:
    case MsgType::Image:
    case MsgType::File:
        /*************群聊功能：is_group=true 时 to 是 groupId，走群消息写扩散***************/
        if (msg.is_group) do_send_group(msg);
        else              do_send(msg);
        break;
    /*************图片文件功能（七牛云直传）：File=4 与 Text/Image 同一入口***************/

    case MsgType::HistoryReq:
        if (msg.is_group) do_history_group(msg);
        else              do_history(msg);
        break;
    /*************群聊功能：is_group 路由群消息/群历史***************/

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

    /*************P1-3：一次取回全部登录补偿数据***************/
    // 原来这里连着调 push_offline() / push_friend_events() / push_group_events()，
    // 每个函数各 1~2 个 DB 任务，合计最多 7 个：
    //   * 在 SQL 队列里要排 7 次队，重连风暴时把队列长度直接乘 7；
    //   * 每次借还连接都要过一遍连接池锁；
    //   * 补推 200 条离线消息 × N 个用户还会瞬间打满 3Mbps 带宽。
    // 现在合并成 1 个任务（内部一条连接顺序跑完所有 SELECT），
    // 离线消息单次上限也从 200 降到 50，剩余用 has_more 让客户端继续拉。
    push_login_sync();
    /*************P1-3：一次取回全部登录补偿数据***************/
}

// ---------------------------------------------------------------------------
// 发消息：关系校验 + 落库 + 在线投递
// ---------------------------------------------------------------------------
// /*************P1-2：从"两段串行回调"改成"一次调用"***************/
// 原来 do_send 是 areFriends(cb -> sendMessage(cb -> ...))：
//   * 一个消息要排两次 SQL 队列、借还两次连接、切两次 strand；
//   * content / StoredMessage 在两层 lambda 里各拷贝一次；
//   * 校验与落库之间还有 TOCTOU 窗口（对方在这瞬间删了你或拉黑你）。
// 现在 Db::sendChecked 在一个任务（必要时同一个事务）里做完，
// 关系命中 P1-1 缓存时连那条 SELECT 都省掉。
// /*************P1-2：从"两段串行回调"改成"一次调用"***************/
void session::do_send(const Message& msg) {
    if (!logged_in_) {
        send_error("login first", user_id_);
        return;
    }
    if (msg.to == 0) {
        send_error("bad recipient", user_id_);
        return;
    }

    /*************图片文件功能（七牛云直传）：媒体消息先验权属再落库***************/
    if (msg.type == MsgType::Image || msg.type == MsgType::File) {
        do_send_media(msg, /*group=*/false);
        return;
    }
    /*************图片文件功能（七牛云直传）：媒体消息先验权属再落库***************/

    // 以会话身份为准，不信任客户端传来的 from。
    finish_send(user_id_, msg.to, static_cast<int>(msg.type),
                msg.content, msg.msg_id);
}

// do_send 的"内容已定稿"部分：文本直达这里，媒体在权属校验通过后也汇入这里。
void session::finish_send(std::uint64_t sender, std::uint64_t to, int type,
                          const std::string& content, std::uint64_t clientMsgId) {
    /*************P0-4：在途任务配额***************/
    if (!reserveJob()) {
        send_error(Db::kBusyError, user_id_);   // 让客户端按自己的重试策略降速
        return;
    }
    /*************P0-4：在途任务配额***************/

    auto self = shared_from_this();
    db_.sendChecked(sender, to, type, content, clientMsgId,
        [this, self, sender, to, type, content](bool ok, const std::string& err,
                                                const StoredMessage& sm,
                                                bool notFriends, bool blocked) {
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, sender, to, type, content, ok, err, sm,
                 notFriends, blocked]() {
            // P0-4：配额在 strand 内归还。outstanding_ 只允许本连接的 strand 碰，
            // 在 DB 回调线程里直接改会构成数据竞争。
            releaseJob();
            if (closed_) return;

            if (!ok) {
                if (notFriends) {
                    /****************文案区分：被对方拉黑 vs 非好友（陌生人）****************/
                    send_error(blocked ? "blocked by recipient" : "not friends", sender);
                } else {
                    send_error(err.empty() ? "send failed" : err, sender);
                }
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

            // 接收者在线则直接投递；离线的部分由 DB 层落库，
            // 对方登录时通过 push_login_sync() 的游标/离线队列补推。
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

// ============================================================================
// /*************图片文件功能（七牛云直传）：do_send_media——媒体消息发送前置校验***************/
// ----------------------------------------------------------------------------
// 校验链（规划 5.TCP 的落地）：
//   1) io 线程：粗解析 content JSON，抽 file_id 并验 uuid 形状（纯 CPU，微秒级）；
//   2) DB worker：getFileByUuid 取回 files 行（走 submitSql 硬上限：满了
//      立刻回 kBusyError，与文本发送同一套背压语义）；
//   3) strand 内：found && status==1 && uploader_id == 登录身份 —— 三个条件
//      缺一不可，分别挡"不存在的 file_id""已删除文件""冒用他人文件"；
//   4) 用 buildMediaMetaJson **整体重建** content：url 固定为自家换取入口、
//      name/size/mime/qetag 全部来自 DB 行——客户端自报的 url 在这一刻死亡，
//      接收端不可能被诱导去点任何外部链接（规划里的防钓鱼红线）。
// 校验通过后汇入 finish_send / finish_send_group：与文本共用同一条
// sendChecked / sendGroupMessage 事务，媒体差异只存在于"发什么内容"，
// 不存在于"怎么投递"——这正是消息链路对 type 无感的红利。
//
// 已知取舍（如实记录，不藏）：
//   * 权属校验与落库是**两个 DB 任务**，之间有理论窗口："校验通过后文件被
//     并发删除"的消息仍会入库。后果被限制为"接收者点开时 404"，不是安全
//     问题；把它压进一个事务要动 sendChecked/sendGroupMessage 的签名，
//     不值得为这个窗口幅度（毫秒级 × 删除频率）改两条热路径。
//   * 每条媒体消息比文本多 1 个 SQL 任务（uuid 索引点查）。
// ============================================================================
void session::do_send_media(const Message& msg, bool group) {
    // 以会话身份为准，不信任客户端传来的 from（与文本路径同一纪律）。
    const std::uint64_t sender      = user_id_;
    const std::uint64_t dest        = msg.to;                 // 单聊=对方 uid / 群聊=groupId
    const int           type        = static_cast<int>(msg.type);
    const std::uint64_t clientMsgId = msg.msg_id;

    // ---- 1) io 线程粗解析 --------------------------------------------------
    std::string uuid, perr;
    if (!extractFileIdFromContent(msg.content, uuid, perr)) {
        send_error("bad media metadata: " + perr, sender);    // 回 Error 帧，不断连接
        return;
    }

    // ---- 2) DB worker 查权属 -----------------------------------------------
    if (!reserveJob()) {
        send_error(Db::kBusyError, sender);
        return;
    }

    auto self = shared_from_this();
    db_.getFileByUuid(uuid,
        [this, self, sender, dest, type, clientMsgId, group, uuid]
        (bool ok, const std::string& err, const FileRecord& rec) {
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, sender, dest, type, clientMsgId, group, rec, ok, err]() {
            releaseJob();
            if (closed_) return;

            if (!ok) {
                send_error(err.empty() ? "file lookup failed" : err, sender);
                return;
            }
            // ---- 3) 三条件权属校验 ------------------------------------------
            if (!rec.found || rec.status != 1) {
                send_error("file not found or deleted", sender);
                return;
            }
            if (rec.uploader_id != sender) {
                // 冒用他人 file_id：这是探测别人文件的行为，值得单独一条日志
                // （不带任何文件内容，只有 uid，避免日志泄漏文件名）。
                std::cout << "[media] user " << sender
                          << " tried to send file owned by " << rec.uploader_id
                          << std::endl;
                send_error("file not uploaded by sender", sender);
                return;
            }

            // ---- 4) 服务端权威重建元数据后，汇入与文本相同的投递路径 ----------
            const std::string meta = buildMediaMetaJson(rec, mediaBaseUrl());
            if (group) finish_send_group(sender, dest, type, meta, clientMsgId);
            else       finish_send(sender, dest, type, meta, clientMsgId);
                });
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
        });
}
/*************图片文件功能（七牛云直传）：do_send_media 结束***************/

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

    /*************core-dump 加固：limit 必须钳制***************/
    // content 是客户端给的字符串，原来只做 stoi 失败回退，**没有上界**。
    // 一条 {"type":7,"to":X,"content":"100000000"} 就会让 DB 层拉回一亿行、
    // 再由 serialize_history 在内存里整段拼成 JSON —— 帧体积远超写预算，
    // 拼字符串时抛 bad_alloc。bad_alloc 从 strand handler 里逃逸 = 杀掉一个
    // io 线程（main.cpp 只能记一条 then 退出 70）。所以钳制在这里做，
    // 而不是指望下游兜。200 与 kLoginOfflineBatch 同档，够正常翻页用。
    int limit = 20;
    if (!msg.content.empty()) {
        try { limit = std::stoi(msg.content); }
        catch (...) { limit = 20; }
    }
    if (limit < 1)   limit = 1;
    if (limit > 200) limit = 200;
    /*************core-dump 加固：limit 必须钳制***************/

    const std::uint64_t uid    = user_id_;
    const std::uint64_t peer   = msg.to;
    const std::uint64_t before = msg.seq;   // 0 = 最新

    if (!reserveJob()) {                     // P0-4：历史翻页也能被狂发，纳入配额
        send_error(Db::kBusyError, uid);
        return;
    }

    auto self = shared_from_this();
    db_.fetchHistory(uid, peer, before, limit,
        [this, self, uid](bool ok, const std::string& err,
                          std::vector<StoredMessage> msgs) {
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, uid, ok, err, msgs = std::move(msgs)]() {
            releaseJob();                    // P0-4：strand 内归还配额
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
// /*************群聊功能：发群消息（写扩散 + 在线成员直推）***************/
// ---------------------------------------------------------------------------
// 与单聊 do_send 的差异：
//   1) 不做好友/拉黑校验——群发言资格 = “是该群成员且群未解散”，
//      这个判断放在 sendGroupMessage 事务里（查 group_members），
//      退群/解散即时生效，无需额外缓存；
//   2) 一次落库会 fan-out 给全群成员：DB 层已给每个成员写 offline_messages，
//      这里再把在线成员遍历 SessionManager 直接投递；
//   3) SendAck 语义与单聊一致（msg_id/seq/ts 由服务端分配，duplicate 去重）。
void session::do_send_group(const Message& msg) {
    if (!logged_in_) {
        send_error("login first", user_id_);
        return;
    }
    if (msg.to == 0) {
        send_error("bad group id", user_id_);
        return;
    }

    /*************图片文件功能（七牛云直传）：群媒体消息同样先验权属再写扩散***************/
    if (msg.type == MsgType::Image || msg.type == MsgType::File) {
        do_send_media(msg, /*group=*/true);
        return;
    }
    /*************图片文件功能（七牛云直传）：群媒体消息同样先验权属再写扩散***************/

    finish_send_group(user_id_, msg.to, static_cast<int>(msg.type),
                      msg.content, msg.msg_id);
}

// do_send_group 的"内容已定稿"部分（与 finish_send 对称）。
void session::finish_send_group(std::uint64_t sender, std::uint64_t groupId, int type,
                                const std::string& content, std::uint64_t clientMsgId) {
    /*************P0-4：群消息同样受在途配额约束（它的 DB 成本比单聊更高）***************/
    if (!reserveJob()) {
        send_error(Db::kBusyError, user_id_);
        return;
    }
    /*************P0-4：群消息同样受在途配额约束***************/

    auto self = shared_from_this();
    db_.sendGroupMessage(sender, groupId, type, content, clientMsgId,
        [this, self, sender, groupId, type, content]
        (bool ok, const std::string& err, const StoredMessage& sm,
         const std::vector<std::uint64_t>& memberIds) {
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
            boost::asio::post(strand_,
                [this, self, sender, groupId, type, content, ok, err, sm, memberIds]() {
            releaseJob();                    // P0-4：strand 内归还配额
            if (closed_) return;
            if (!ok) {
                send_error(err.empty() ? "group send failed" : err, sender);
                return;
            }

            // 发送方回执：带服务端分配的 conversation_id/seq/msg_id/ts。
            Message ack;
            ack.type            = MsgType::SendAck;
            ack.to              = sender;
            ack.msg_id          = sm.msg_id;
            ack.conversation_id = sm.conversation_id;
            ack.seq             = sm.seq;
            ack.ts              = sm.ts;
            ack.is_group        = true;
            ack.content         = sm.duplicate ? "duplicate" : "sent";
            send_message(ack);

            if (sm.duplicate) return;   // 去重命中：不再向群成员重复投递

            // 在线成员直推；离线的成员已写入 offline_messages，登录时补推。
            for (std::uint64_t uid : memberIds) {
                if (uid == sender) continue;      // 不给发送者自己投递
                if (auto peer = manager_.find(uid)) {
                    Message out;
                    out.type            = static_cast<MsgType>(type);
                    out.from            = sender;
                    out.to              = groupId;
                    out.msg_id          = sm.msg_id;
                    out.conversation_id = sm.conversation_id;
                    out.seq             = sm.seq;
                    out.ts              = sm.ts;
                    out.content         = content;
                    out.is_group        = true;
                    peer->deliver(out);
                }
            }
            std::cout << "[group-route] " << sender << " -> group " << groupId
                      << " members=" << memberIds.size() << std::endl;
                });
            /*************并发登录/注册优化：DB 回调切回本连接 strand***************/
        });
}

// ---------------------------------------------------------------------------
// /*************群聊功能：群历史分页（校验成员身份）***************/
// ---------------------------------------------------------------------------
void session::do_history_group(const Message& msg) {
    if (!logged_in_) {
        send_error("login first", user_id_);
        return;
    }
    if (msg.to == 0) {
        send_error("bad group id", user_id_);
        return;
    }

    /*************core-dump 加固：limit 必须钳制***************/
    // content 是客户端给的字符串，原来只做 stoi 失败回退，**没有上界**。
    // 一条 {"type":7,"to":X,"content":"100000000"} 就会让 DB 层拉回一亿行、
    // 再由 serialize_history 在内存里整段拼成 JSON —— 帧体积远超写预算，
    // 拼字符串时抛 bad_alloc。bad_alloc 从 strand handler 里逃逸 = 杀掉一个
    // io 线程（main.cpp 只能记一条 then 退出 70）。所以钳制在这里做，
    // 而不是指望下游兜。200 与 kLoginOfflineBatch 同档，够正常翻页用。
    int limit = 20;
    if (!msg.content.empty()) {
        try { limit = std::stoi(msg.content); }
        catch (...) { limit = 20; }
    }
    if (limit < 1)   limit = 1;
    if (limit > 200) limit = 200;
    /*************core-dump 加固：limit 必须钳制***************/

    const std::uint64_t uid  = user_id_;
    const std::uint64_t gid  = msg.to;      // 群聊：to = groupId
    const std::uint64_t before = msg.seq;

    if (!reserveJob()) {                     // P0-4：与单聊历史一致的配额保护
        send_error(Db::kBusyError, uid);
        return;
    }

    auto self = shared_from_this();
    db_.fetchGroupHistory(gid, uid, before, limit,
        [this, self, uid](bool ok, const std::string& err,
                          std::vector<StoredMessage> msgs) {
            boost::asio::post(strand_,
                [this, self, uid, ok, err, msgs = std::move(msgs)]() {
            releaseJob();                    // P0-4：strand 内归还配额
            if (closed_) return;
            if (!ok) {
                send_error(err.empty() ? "history failed" : err, uid);
                return;
            }
            Message resp;
            resp.type    = MsgType::HistoryResp;
            resp.to      = uid;
            resp.is_group = true;
            resp.content = serialize_history(msgs);
            send_message(resp);
                });
        });
}

// ---------------------------------------------------------------------------
// P1-3：登录补推（离线消息 + 好友事件 + 群事件，一次 DB 任务取完）
// ---------------------------------------------------------------------------
// 原来这里有三套各自独立的"取 -> 回调 -> 再取 -> 回调"链路，合计最多 7 个 DB 任务：
//   push_offline()        fetchOffline(200)
//   push_friend_events()  fetchPendingIncoming + fetchUnnotifiedResults
//                         + markResultsNotified
//   push_group_events()   fetchPendingGroupRequestsForApprover
//                         + fetchUnnotifiedGroupResults + markGroupResultsNotified
// 重连风暴时"每个登录 7 个任务"把 SQL 队列长度直接乘 7，这正是压测里
// 登录之后所有接口一起变慢的原因之一。现在合并成 Db::fetchLoginSync 一个任务：
//   * 一条连接顺序跑完所有 SELECT（外加必要的标记 UPDATE），只借还一次连接；
//   * 离线消息单次上限降到 kLoginOfflineBatch，并用 offlineMore 告知客户端继续拉，
//     避免"100 个用户同时登录 × 200 条"瞬间打满 3Mbps 带宽；
//   * 无群用户在 SQL 内部直接跳过两条群查询。
// 推给客户端的顺序保持与原来一致：离线消息 -> 好友申请 -> 好友结果
//   -> 入群申请 -> 入群结果，老客户端无需任何改动。
void session::push_login_sync() {
    const std::uint64_t uid = user_id_;
    auto self = shared_from_this();

    if (!reserveJob()) {          // P0-4：登录补推本身也占一个配额
        std::cerr << "[login] skip catch-up, too many outstanding jobs, user="
                  << uid << std::endl;
        return;
    }

    db_.fetchLoginSync(uid, kLoginOfflineBatch,
        [this, self, uid](bool ok, const std::string& err, Db::LoginSync sync) {
            boost::asio::post(strand_,
                [this, self, uid, ok, err, sync = std::move(sync)]() {
            releaseJob();          // P0-4：配额必须在本连接的 strand 内归还
            if (closed_) return;
            if (!ok) {
                std::cerr << "[login] catch-up failed: " << err << std::endl;
                return;
            }

            // ---- 1) 离线消息 ----
            for (const auto& sm : sync.offline) {
                Message m;
                m.type            = sm.type;
                m.from            = sm.from;
                /*************群聊功能：群消息 to 填 groupId，单聊维持 to=uid***************/
                m.to              = sm.is_group ? sm.to : uid;
                m.is_group        = sm.is_group;
                /*************群聊功能：群消息 to 填 groupId***************/
                m.msg_id          = sm.msg_id;
                m.conversation_id = sm.conversation_id;
                m.seq             = sm.seq;
                m.ts              = sm.ts;
                m.content         = sm.content;
                send_message(m);
            }
            if (!sync.offline.empty()) {
                std::cout << "[offline] pushed " << sync.offline.size()
                          << " message(s) to user " << uid
                          << (sync.offlineMore ? " (more pending)" : "") << std::endl;
            }

            // ---- 2) 待我处理的好友申请 ----
            for (const auto& r : sync.pendingRequests) {
                Message m;
                m.type    = MsgType::FriendRequest;
                m.from    = r.requester_id;
                m.to      = uid;
                m.msg_id  = r.id;          // 复用 msg_id 携带 request_id，便于客户端处理
                m.ts      = r.created_ts;
                m.content = serialize_friend_request_event(r, "pending");
                send_message(m);
            }

            // ---- 3) 我发出的申请、现在才通知到的处理结果 ----
            for (const auto& r : sync.requestResults) {
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

            // ---- 4) 待我审批的入群申请 ----
            for (const auto& r : sync.pendingGroupRequests) {
                Message m;
                m.type    = MsgType::GroupRequest;
                m.from    = r.requester_id;
                m.to      = uid;
                m.msg_id  = r.id;
                m.ts      = r.created_ts;
                m.content = serialize_group_request_event(r, "pending");
                send_message(m);
            }

            // ---- 5) 我发出的入群申请的处理结果 ----
            for (const auto& r : sync.groupResults) {
                Message m;
                m.type    = MsgType::GroupRequestResult;
                m.from    = r.handler_id;
                m.to      = uid;
                m.msg_id  = r.id;
                m.ts      = r.created_ts;
                m.content = serialize_group_request_result_event(
                                r, r.status == 1 ? "accepted" : "rejected");
                send_message(m);
            }

            /*************P1-3：标记"已通知"仍在推送之后***************/
            // 与合并前的行为逐字一致：只有列表非空才发这条 UPDATE。
            // 顺序很重要 —— 先推后标记，进程在中途崩溃时最坏是重复推送，
            // 反过来（先标记后推送）就会真的丢事件。
            if (!sync.requestResults.empty()) {
                db_.markResultsNotified(uid, [](bool ok3, const std::string& err3) {
                    if (!ok3) std::cerr << "[friend] mark notified failed: "
                                         << err3 << std::endl;
                });
            }
            if (!sync.groupResults.empty()) {
                db_.markGroupResultsNotified(uid, [](bool ok3, const std::string& err3) {
                    if (!ok3) std::cerr << "[group] mark notified failed: "
                                        << err3 << std::endl;
                });
            }
            /*************P1-3：标记"已通知"仍在推送之后***************/

            const std::size_t events = sync.pendingRequests.size() +
                                       sync.requestResults.size() +
                                       sync.pendingGroupRequests.size() +
                                       sync.groupResults.size();
            if (events) {
                std::cout << "[login] catch-up user=" << uid
                          << " offline=" << sync.offline.size()
                          << " events=" << events << std::endl;
            }
                });
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

    /*************P0-4：ACK 也计入在途配额***************/
    // 超限时静默丢弃即可：ACK 是幂等的"游标推进"，客户端不需要错误提示，
    // 下一条 ACK 会带着更大的 seq 把这段补回来。
    if (!reserveJob()) return;
    /*************P0-4：ACK 也计入在途配额***************/

    auto self = shared_from_this();
    db_.updateReceipt(uid, conv, seq, markRead ? seq : 0,
        [this, self](bool ok, const std::string& err) {
            // 归还配额必须在本连接的 strand 内做（outstanding_ 非原子）。
            boost::asio::post(strand_, [this, self, ok, err]() {
                releaseJob();
                if (!ok) std::cerr << "[ack] " << err << std::endl;
            });
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
// 发送：序列化 -> 组帧 -> 入队（带高水位）-> 必要时启动写
// ---------------------------------------------------------------------------
void session::send_message(const Message& msg) {
    /*************core-dump 修复 ④：关闭后不再记账***************/
    // do_close() 已经把这条连接的字节数从进程级预算里结清了。此后再入队
    // 就是"只加不还"（写在关闭的 socket 上必然以 ec 结束，而 ec 分支不还数），
    // 预算会被连接 churn 单向推高，最后随机踢掉好连接。
    if (closed_) return;

    std::string payload = serialize_message(msg);

    std::string framed(frame::HEADER_LEN + payload.size(), '\0');
    frame::encode(static_cast<std::uint32_t>(payload.size()), framed.data());
    std::memcpy(framed.data() + frame::HEADER_LEN, payload.data(), payload.size());

    /*************P0-4：写队列两级预算***************/
    // 对端读得慢（弱网、3Mbps 链路、客户端卡在 UI 线程）时，帧会一直堆在这里。
    // 无界队列在 2GB 机器上就是 OOM 路径，而消息本身已经落库，
    // 所以"踢掉这一条连接"比"拖死整个进程"好：客户端重连后会走
    // push_login_sync() 的游标补推 / 历史接口把缺的部分对齐回来，不丢消息。
    //
    // 两级一起判：单连接 512KB + 进程级 64MB。只看单连接上限的话，
    // 500 条慢连接各堆 1MB 也能吃掉 500MB，而那时**每条连接都没越线**。
    //
    // 这里的"先占额度、失败再退回"在并发下可能一瞬间多放过一帧：
    // 预算本来就是软阈值（保护量级，不是精确记账），为它加全局锁会把
    // 所有连接的发送路径串起来，不值得。
    const std::size_t need        = framed.size();
    const std::size_t connPending = write_queue_bytes_ + need;
    const std::size_t globalWas   = g_writeQueueTotalBytes.fetch_add(need);

    if (connPending > kMaxWriteBytes ||
        globalWas + need > kGlobalWriteBudgetBytes) {
        g_writeQueueTotalBytes.fetch_sub(need);        // 退回占用的额度
        logFatal("[write] queue over budget, kick user=" + std::to_string(user_id_) +
                 " conn=" + std::to_string(connPending) + "B/cap=" +
                 std::to_string(kMaxWriteBytes) + "B, global=" +
                 std::to_string(globalWas + need) + "B/cap=" +
                 std::to_string(kGlobalWriteBudgetBytes) + "B");
        close();
        return;
    }
    /*************P0-4：写队列两级预算***************/

    write_queue_bytes_ += need;
    const bool idle = write_queue_.empty();
    write_queue_.push_back(std::move(framed));
    if (idle) do_write();
}

// ---------------------------------------------------------------------------
// 写队列串行化
// /*************P1-8：一次 async_write 合并多帧***************/
// 登录补推、好友事件风暴这类场景会在很短时间内往同一条连接塞几十上百个小帧。
// 一帧一次 async_write = 一帧一次系统调用 + 一帧一个独立报文，
// 再叠上 Nagle/延迟 ACK 的交互（P1-5 已关闭 Nagle），CPU 和长尾都很难看。
// 现在一次最多合并 kMaxWriteFrames 帧、kMaxWriteBatchBytes 字节。
//
// /*************core-dump 修复 ②：本批字节必须有自己的副本***************/
// 上一版把 write_queue_ 里各个 std::string 的 const_buffer 直接交给 async_write，
// 并在注释里断言"deque 尾追加不会让已有元素的引用失效，所以安全"。
// 前半句是对的，结论是错的：**追加**确实不动已有元素，但这一版同时给
// do_close() 加了 `write_queue_.clear()`（归还进程级写预算必须清队列）。
// 于是任何一次"写入还没完成就关闭连接"（空闲踢人、对端断开、预算越线、
// 重复登录踢旧连接）都会把 async_write 正在引用的那批 std::string **释放掉**，
// 而 op 对象仍持有指向它们的指针 —— 标准的 use-after-free，
// 只在"多帧成批 + 同刻断线"时出现，正是压测阶段4（100 条连接同时收
// FriendListChanged、同时有连接在关）的形状。
// 现在：本批内容拷进一个由本次写操作独占持有的连续缓冲区（shared_ptr 进回调），
// write_queue_ 之后怎么 clear/pop 都碰不到内核正在读的那块内存。
// 代价是一次 ≤64KB 的 memcpy，换来的是"写路径不再有悬垂指针"这一整类故障。
// ---------------------------------------------------------------------------
void session::do_write() {
    if (closed_ || writing_) return;      // 修复①：绝不允许第二个写在途

    std::size_t bytes  = 0;
    std::size_t frames = 0;
    for (const auto& f : write_queue_) {
        if (frames >= static_cast<std::size_t>(kMaxWriteFrames) ||
            (bytes > 0 && bytes + f.size() > kMaxWriteBatchBytes)) {
            break;
        }
        bytes += f.size();
        ++frames;
    }
    if (frames == 0) return;

    auto batch = std::make_shared<std::string>();
    batch->reserve(bytes);
    for (std::size_t i = 0; i < frames; ++i) batch->append(write_queue_[i]);

    writing_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(*batch),
        boost::asio::bind_executor(strand_,
        [this, self, batch, frames](boost::system::error_code ec, std::size_t /*bytes_written*/) {
            writing_ = false;
            if (ec) { close(); return; }
            // 连接已关：write_queue_ 与两份字节计数都由 do_close() 结清了，
            // 这里绝不能再 pop/扣减（那是第二次扣，会把全局预算扣成天文数字）。
            if (closed_) return;

            // 出队：本地与全局两份字节数都要还回去。
            // 出错分支**故意不**在这里还：那时由 do_close() 一次性清干净，
            // 两条路径各还一次，才不会重复扣减全局预算。
            std::size_t freed = 0;
            for (std::size_t i = 0; i < frames && !write_queue_.empty(); ++i) {
                freed += write_queue_.front().size();
                write_queue_.pop_front();
            }
            write_queue_bytes_ -= freed;
            g_writeQueueTotalBytes.fetch_sub(freed);
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

    /*************core-dump 修复 ③：注销要认"是不是我"***************/
    // 原来是无条件 manager_.remove(user_id_)。重复登录时在线表里这一格
    // 已经被新连接占用（do_login 先 add 再踢旧），旧连接随后走到这里就会把
    // **新连接**的登记删掉：新连接还活着、socket 还好用，但已经不在表里，
    // 于是所有发给它的路由都判成"对方离线"，改走离线落库，
    // 客户端表现为"我在线却收不到消息"。必须带身份比对。
    if (logged_in_) {
        manager_.remove(user_id_, this);
        logged_in_ = false;
        std::cout << "[logout] user " << user_id_
                  << ", online=" << manager_.size() << std::endl;
    }

    /*************P0-4：归还这条连接还压在写队列里的字节***************/
    // 必须有这一步。写队列的进程级预算是**全局计数**，而出队只发生在
    // "写完成"这条路径上；连接被踢 / 对端断开 / 中途出错时，队列里那些
    // 永远等不到写完成的帧如果不在这里释放，预算就会随连接 churn 单向上涨，
    // 最后把好端端的连接也一起踢掉——一个典型的记账泄漏。
    // 与 do_write 的分工很清楚：写成功 → 由写回调按帧归还；
    // 其余一切路径 → 由这里一次性清空。两条路径不会同时命中同一批字节
    // （写回调遇到 ec / closed_ 时不 pop、不扣减，直接返回）。
    // 这里的 clear() 之所以安全：do_write 交给 async_write 的是**本批自己的副本**，
    // 不再指向 deque 里的这些 string（见 do_write 的"core-dump 修复 ②"）。
    if (write_queue_bytes_ > 0) {
        g_writeQueueTotalBytes.fetch_sub(write_queue_bytes_);
        write_queue_bytes_ = 0;
    }
    write_queue_.clear();
    /*************P0-4：归还这条连接还压在写队列里的字节***************/

    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

} // namespace kc
