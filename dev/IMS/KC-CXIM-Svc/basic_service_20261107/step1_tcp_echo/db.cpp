#include "db.h"

#include <mariadb/conncpp.hpp>          // MariaDB Connector/C++（同步阻塞 API）
#include <boost/asio/post.hpp>          // boost::asio::post：把结果投递回 io_context 线程
#include <bcrypt/BCrypt.hpp>            // bcrypt：生成/校验密码哈希（重 CPU，必须在 worker 线程执行）

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace kc {

namespace {

// SQLString（Connector 自己的字符串类）-> std::string
std::string sstr(const sql::SQLString& s) {
    return std::string(s.c_str(), s.length());
}

// 读取"当前连接"上最近一次 AUTO_INCREMENT / LAST_INSERT_ID(expr) 的值。
// 必须在同一个连接、同一个事务里调用。
std::uint64_t lastInsertId(sql::Connection* conn) {
    std::unique_ptr<sql::Statement> st(conn->createStatement());
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery("SELECT LAST_INSERT_ID()"));
    if (!rs->next()) return 0;
    return rs->getUInt64(1);
}

// 单聊去重键：u:<小userId>:<大userId>，保证同一对用户只有一个会话。
std::string dedupKey(std::uint64_t a, std::uint64_t b) {
    std::uint64_t lo = std::min(a, b);
    std::uint64_t hi = std::max(a, b);
    return "u:" + std::to_string(lo) + ":" + std::to_string(hi);
}

// 按固定列顺序读取一行消息。SQL 必须 SELECT：
//   id, conversation_id, seq, sender_id, type, content, ts(ms)
StoredMessage readStored(sql::ResultSet& rs) {
    StoredMessage m;
    m.msg_id          = rs.getUInt64(1);
    m.conversation_id = rs.getUInt64(2);
    m.seq             = rs.getUInt64(3);
    m.from            = rs.getUInt64(4);
    m.type            = static_cast<MsgType>(rs.getInt(5));
    m.content         = sstr(rs.getString(6));
    m.ts              = rs.getUInt64(7);
    return m;
}

// ---- 好友模块：行读取与业务异常 --------------------------------------------
// BizError 用来区分“业务规则拒绝”和“数据库异常”：
//   * BizError 直接把 what() 作为给客户端的友好提示，例如 "already friends"；
//   * 其他 std::exception 统一包成 "db error: ..."，方便定位基础设施问题。
struct BizError : std::runtime_error {
    explicit BizError(const std::string& msg) : std::runtime_error(msg) {}
};

/*************避免数据库死锁的解决方案********************/
// P0-2：事务级死锁重试。
// MySQL/MariaDB 的死锁(1213)与锁等待超时(1205)是瞬时可恢复错误，
// 标准做法是回滚整个事务后重试；这里只重试这两类错误：
//   * BizError 是业务规则拒绝，不重试；
//   * 其他异常/错误直接上抛。
// 注意：必须整个事务重来，不能只重试失败的那条 SQL。
inline bool isRetryableDbError(sql::SQLException& e) {
    const int code = e.getErrorCode();
    // 1213=deadlock, 1205=lock wait timeout,
    // 1020=Record has changed since last read（并发读写同一行时的瞬态错误）
    return code == 1213 || code == 1205 || code == 1020;
}

template <typename Fn>
void runTransactionWithRetry(sql::Connection* conn, Fn&& body) {
    constexpr int kMaxAttempts = 3;        // 1 次正常 + 2 次重试
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        try {
            conn->setAutoCommit(false);
            body();
            conn->commit();
            return;
        } catch (const BizError&) {
            try { conn->rollback(); } catch (...) {}
            throw;                          // 业务错误不重试
        } catch (sql::SQLException& e) {
            try { conn->rollback(); } catch (...) {}
            if (isRetryableDbError(e) && attempt < kMaxAttempts) {
                // 少量随机退避，降低再次撞锁的概率。
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5 * attempt + (std::rand() % 20)));
                continue;
            }
            throw;
        } catch (...) {
            try { conn->rollback(); } catch (...) {}
            throw;
        }
    }
}
/*************避免数据库死锁的解决方案********************/

// SQL 必须 SELECT：id, username, nickname, avatar_url
UserInfo readUserInfo(sql::ResultSet& rs) {
    UserInfo u;
    u.id         = rs.getUInt64(1);
    u.username   = sstr(rs.getString(2));
    u.nickname   = sstr(rs.getString(3));
    u.avatar_url = sstr(rs.getString(4));
    return u;
}

// SQL 必须 SELECT：id, username, nickname, avatar_url, remark, status
FriendInfo readFriendInfo(sql::ResultSet& rs) {
    FriendInfo f;
    f.id         = rs.getUInt64(1);
    f.username   = sstr(rs.getString(2));
    f.nickname   = sstr(rs.getString(3));
    f.avatar_url = sstr(rs.getString(4));
    f.remark     = sstr(rs.getString(5));
    f.status     = rs.getInt(6);
    return f;
}

// 好友申请的标准 SELECT 列顺序：
//   fr.id, fr.requester_id, fr.target_id,
//   ru.username, ru.nickname, tu.username, tu.nickname,
//   fr.message, fr.status, ts(ms)
FriendRequestInfo readFriendRequestInfo(sql::ResultSet& rs) {
    FriendRequestInfo r;
    r.id                 = rs.getUInt64(1);
    r.requester_id       = rs.getUInt64(2);
    r.target_id          = rs.getUInt64(3);
    r.requester_username = sstr(rs.getString(4));
    r.requester_nickname = sstr(rs.getString(5));
    r.target_username    = sstr(rs.getString(6));
    r.target_nickname    = sstr(rs.getString(7));
    r.message            = sstr(rs.getString(8));
    r.status             = rs.getInt(9);
    r.created_ts         = rs.getUInt64(10);
    return r;
}

// 所有好友申请查询共用这段 SELECT，避免列顺序在多个 DAO 里漂移。
const char* kRequestSelect =
    "SELECT fr.id, fr.requester_id, fr.target_id, "
    "       ru.username, ru.nickname, tu.username, tu.nickname, "
    "       fr.message, fr.status, "
    "       CAST(UNIX_TIMESTAMP(fr.created_at)*1000 AS UNSIGNED) "
    "FROM friend_requests fr "
    "LEFT JOIN users ru ON ru.id = fr.requester_id "
    "LEFT JOIN users tu ON tu.id = fr.target_id ";

} // namespace

// ============================================================================
// ConnectionPool：固定大小的连接池
// ============================================================================
class ConnectionPool {
public:
    ConnectionPool(const DbConfig& cfg, std::size_t size)
        : cfg_(cfg), max_size_(std::max<std::size_t>(1, size)) {
        /*************并发登录/注册优化：连接池懒创建***************/
        // 启动时只建 1 条连接做连通性校验；其余连接在 acquire() 时按需创建。
        // 这样不会在启动瞬间占满 MariaDB 的 max_connections（避免
        // "Too many connections"），同时仍能按并发逐步扩到 max_size_。
        free_.push_back(makeConn());
        created_ = 1;
        /*************并发登录/注册优化：连接池懒创建***************/
    }

    // 借一条连接；池被停止时返回 nullptr。借出的连接会做健康检查，
    // 坏连接会被丢弃，后续 acquire() 再按需重建。
    std::unique_ptr<sql::Connection> acquire() {
        std::unique_ptr<sql::Connection> conn;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            for (;;) {
                if (stopping_) return nullptr;

                if (!free_.empty()) {
                    conn = std::move(free_.front());
                    free_.pop_front();
                    break;
                }

                /*************并发登录/注册优化：按需扩容***************/
                // 没有空闲连接且还没到上限：先占一个名额，再到锁外建连接，
                // 避免持有锁执行阻塞的网络连接。
                if (created_ < max_size_) {
                    ++created_;
                    lk.unlock();
                    try {
                        return makeConn();
                    } catch (const std::exception& e) {
                        std::cerr << "[db] create connection failed: "
                                  << e.what() << std::endl;
                        lk.lock();
                        --created_;
                        cv_.notify_one();
                        return nullptr;
                    }
                }
                /*************并发登录/注册优化：按需扩容***************/

                // 已到上限：等待其他线程归还连接。
                cv_.wait(lk, [this] { return stopping_ || !free_.empty(); });
            }
        }

        try {
            if (conn && !conn->isClosed() && conn->isValid()) return conn;
        } catch (...) {
            // 落到下面丢弃坏连接
        }

        // 坏连接原地替换，created_ 名额先保持不变；只有替换失败才归还名额。
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stopping_) {
                --created_;
                cv_.notify_one();
                return nullptr;
            }
        }
        try {
            return makeConn();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mutex_);
            --created_;
            cv_.notify_one();
            std::cerr << "[db] reconnect failed: " << e.what() << std::endl;
            return nullptr;
        }
    }

    // 停止连接池：唤醒所有等待者，让析构流程不会卡在 acquire()。
    void stop() {
        std::lock_guard<std::mutex> lk(mutex_);
        stopping_ = true;
        cv_.notify_all();
    }

    // 归还连接。健康连接放回池；坏连接直接丢弃，由后续 acquire() 按需重建。
    void release(std::unique_ptr<sql::Connection> conn) {
        bool healthy = false;
        if (conn) {
            try {
                if (!conn->isClosed()) {
                    conn->setAutoCommit(true);   // 清掉可能残留的事务状态
                    healthy = conn->isValid();
                }
            } catch (...) {
                healthy = false;
            }
        }

        std::lock_guard<std::mutex> lk(mutex_);
        if (stopping_) return;

        if (!healthy) {
            /*************并发登录/注册优化：坏连接回收名额***************/
            --created_;
            cv_.notify_one();
            /*************并发登录/注册优化：坏连接回收名额***************/
            return;
        }
        free_.push_back(std::move(conn));
        cv_.notify_one();
    }

    std::size_t max_size() const { return max_size_; }

private:
    std::unique_ptr<sql::Connection> makeConn() {
        sql::Driver* driver = sql::mariadb::get_driver_instance();

        // MariaDB Connector/C++ 的连接参数是键值对。
        // 常见键：hostName / port / userName / password / schema，
        // 以及超时相关：connectTimeout / socketTimeout（毫秒）。
        sql::Properties props;
        props["hostName"]       = sql::SQLString(cfg_.host);
        props["port"]           = sql::SQLString(std::to_string(cfg_.port));
        props["userName"]       = sql::SQLString(cfg_.user);
        props["password"]       = sql::SQLString(cfg_.password);
        props["schema"]         = sql::SQLString(cfg_.dbname);
        props["connectTimeout"] = sql::SQLString("5000");
        props["socketTimeout"]  = sql::SQLString("30000");

        std::unique_ptr<sql::Connection> conn(driver->connect(props));
        conn->setAutoCommit(true);
        return conn;
    }

    DbConfig cfg_;
    std::size_t max_size_ = 1;   // 连接池上限（按需扩容，不会启动时全部创建）
    std::size_t created_  = 0;   // 当前已创建（含借出）的连接数
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<sql::Connection>> free_;
    bool stopping_ = false;
};

namespace {

// RAII：构造时借连接，析构时归还。异常路径也能保证归还。
class PooledConn {
public:
    explicit PooledConn(ConnectionPool& pool) : pool_(pool), conn_(pool.acquire()) {}
    ~PooledConn() {
        if (conn_) pool_.release(std::move(conn_));
    }

    sql::Connection* get() const { return conn_.get(); }
    sql::Connection* operator->() const { return conn_.get(); }
    explicit operator bool() const { return static_cast<bool>(conn_); }

private:
    ConnectionPool& pool_;
    std::unique_ptr<sql::Connection> conn_;
};

} // namespace

// ============================================================================
// Db：构造 / 销毁 / 线程池
// ============================================================================
Db::Db(boost::asio::io_context& io, const DbConfig& cfg,
       std::size_t pool_size, std::size_t worker_count)
    : io_(io),
      pool_(std::make_unique<ConnectionPool>(cfg, pool_size)) {
    std::cout << "[db] pool max=" << pool_->max_size()
              << ", workers=" << worker_count
              << " (connections are created lazily)" << std::endl;
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

Db::~Db() {
    {
        std::lock_guard<std::mutex> lk(jobs_mutex_);
        stopping_ = true;
    }
    jobs_cv_.notify_all();
    if (pool_) pool_->stop();   // 唤醒卡在 acquire() 的 worker
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void Db::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(jobs_mutex_);
        if (stopping_) return;
        jobs_.push(std::move(job));
    }
    jobs_cv_.notify_one();
}

void Db::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(jobs_mutex_);
            jobs_cv_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop();
        }

        try {
            job();
        } catch (const std::exception& e) {
            // 兜底：DAO 内部已经捕获异常；这里防止意外异常杀死 worker。
            std::cerr << "[db] worker exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[db] worker unknown exception" << std::endl;
        }
    }
}

void Db::postToIo(std::function<void()> fn) {
    // 结果必须回到 io_context 线程再碰 session/socket，避免多线程数据竞争。
    boost::asio::post(io_, std::move(fn));
}

// ============================================================================
// DAO 1：登录校验
// ============================================================================
void Db::login(std::uint64_t userId, const std::string& credential, LoginCb cb) {
    submit([this, userId, credential, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::unique_ptr<sql::PreparedStatement> st(
                pc->prepareStatement("SELECT password_hash FROM users WHERE id=?"));
            st->setUInt64(1, userId);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            if (!rs->next()) {
                err = "user not found";
            } else if (sstr(rs->getString(1)) != credential) {
                err = "bad credential";
            } else {
                ok = true;
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

// ============================================================================
// DAO 2：注册（用户名唯一 + bcrypt 哈希 + 插入 users）
// ----------------------------------------------------------------------------
// 关键点：
//   * bcrypt 是"故意慢"的算法（cost=12 约 200~300ms），必须在 worker 线程算；
//   * 先查重给出友好提示，最终仍由 users.uk_users_username 唯一键兜底；
//   * 插入后在同一连接上取 LAST_INSERT_ID()，拿到数据库分配的用户ID。
// ============================================================================
void Db::registerUser(const std::string& username, const std::string& password,
                      RegisterCb cb) {
    submit([this, username, password, cb]() {                                   // 把整个任务投递到 worker 线程
        bool ok = false;                                                        // 默认失败
        std::string err;                                                        // 失败原因
        std::uint64_t userId = 0;                                               // 注册成功后的用户ID

        try {
            PooledConn pc(*pool_);                                              // 从连接池借一条连接（RAII 自动归还）
            if (!pc) throw std::runtime_error("db pool unavailable");           // 池不可用直接走异常分支

            sql::Connection* conn = pc.get();                                   // 取出裸连接指针，便于复用连接

            // 1) 先查用户名是否已存在，给出明确错误。
            {
                std::unique_ptr<sql::PreparedStatement> st(                     // 预编译语句防注入
                    conn->prepareStatement("SELECT id FROM users WHERE username=?"));
                st->setString(1, username);                                     // 绑定用户名参数
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());         // 执行查询
                if (rs->next()) err = "username already exists";                // 有结果说明重复
            }

            // 2) 用户名可用：在 worker 线程计算 bcrypt 哈希（重 CPU，绝不放到 io 线程）。
            if (err.empty()) {
                const std::string hash = BCrypt::generateHash(password, 12);    // cost=12，返回 $2a$... 哈希串
                std::unique_ptr<sql::PreparedStatement> st(                     // 预编译 INSERT
                    conn->prepareStatement(
                        "INSERT INTO users(username,password_hash,nickname) VALUES(?,?,?)"));
                st->setString(1, username);                                     // 用户名
                st->setString(2, hash);                                         // 只存哈希，禁止明文
                st->setString(3, username);                                     // 昵称缺省用用户名
                try {
                    st->executeUpdate();                                        // 执行插入
                } catch (sql::SQLException& e) {
                    // 1062 = 唯一键冲突：并发注册同一用户名时由数据库兜底。
                    if (e.getErrorCode() == 1062) err = "username already exists";   // 转成友好错误
                    else throw;                                                 // 其他错误继续上抛
                }
                if (err.empty()) {                                             // 插入成功才取 ID
                    userId = lastInsertId(conn);                               // 同一连接上取自增ID
                    ok = (userId != 0);                                        // 取到ID视为成功
                    if (!ok) err = "failed to create user";                    // 异常兜底
                }
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();                        // 数据库/哈希异常统一报错
        }

        postToIo([cb, ok, err, userId]() { cb(ok, err, userId); });             // 结果回到 io_context 线程再回调
    });
}

// ============================================================================
// DAO 3：HTTP 登录（按 username 查用户 + 在 worker 里 bcrypt verify）
// ============================================================================
void Db::loginUser(const std::string& username, const std::string& password,
                   LoginUserCb cb) {
    submit([this, username, password, cb]() {                                   // 任务投递到 worker 线程
        bool ok = false;                                                        // 默认失败
        std::string err;                                                        // 失败原因
        std::uint64_t userId = 0;                                               // 登录成功后的用户ID
        std::string name = username;                                            // 登录成功后的用户名

        try {
            PooledConn pc(*pool_);                                              // 借连接
            if (!pc) throw std::runtime_error("db pool unavailable");           // 池不可用

            std::unique_ptr<sql::PreparedStatement> st(                         // 预编译查询
                pc->prepareStatement(
                    "SELECT id, username, password_hash, status FROM users WHERE username=?"));
            st->setString(1, username);                                         // 绑定用户名
            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());             // 执行查询

            if (!rs->next()) {
                err = "user not found";                                         // 用户不存在
            } else {
                const std::uint64_t id = rs->getUInt64(1);                      // 用户ID
                const std::string uname = sstr(rs->getString(2));               // 用户名
                const std::string hash = sstr(rs->getString(3));                // 存储的 bcrypt 哈希
                const int status = rs->getInt(4);                               // 账号状态

                if (status != 1) {
                    err = "account disabled";                                   // 被禁用则拒绝
                } else if (BCrypt::validatePassword(password, hash)) {          // worker 线程里验证密码
                    ok = true;                                                  // 校验通过
                    userId = id;                                                // 返回用户ID
                    name = uname;                                               // 返回用户名
                } else {
                    err = "bad credential";                                     // 密码错误
                }
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();                        // 异常统一报错
        }

        postToIo([cb, ok, err, userId, name]() { cb(ok, err, userId, name); }); // 回到 io 线程回调
    });
}

// ============================================================================
// DAO 4：单聊发送（一个事务内完成 seq/消息/离线队列/游标）
// ============================================================================
void Db::sendMessage(std::uint64_t sender, std::uint64_t to, int type,
                     const std::string& content, std::uint64_t clientMsgId,
                     SendCb cb) {
    submit([this, sender, to, type, content, clientMsgId, cb]() {
        bool ok = false;
        std::string err;
        StoredMessage out;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();
            std::uint64_t msgId = 0;

            /*************避免数据库死锁的解决方案********************/
            // P0-2：整个事务重试，遇到死锁(1213)/锁等待超时(1205)自动回滚重来。
            // 发送链路会同时更新 conversations / messages / offline_messages /
            // conversation_members，并发下确实可能出现瞬时死锁，需要兜底重试。
            runTransactionWithRetry(conn, [&]() {
                ok = false;
                msgId = 0;

                // 接收者必须存在（同时覆盖"非法用户"的错误处理）
                {
                    std::unique_ptr<sql::PreparedStatement> st(
                        conn->prepareStatement("SELECT 1 FROM users WHERE id=?"));
                    st->setUInt64(1, to);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw std::runtime_error("recipient not found");
                }

                // 1) 取或建单聊会话（dedup_key 唯一；重复时用 LAST_INSERT_ID 拿回原 id）
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT INTO conversations(type,title,owner_id,dedup_key) "
                        "VALUES(1,'',0,?) "
                        "ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)"));
                    st->setString(1, dedupKey(sender, to));
                    st->executeUpdate();
                }
                const std::uint64_t conv = lastInsertId(conn);

                // 2) 保证双方都是会话成员
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT IGNORE INTO conversation_members(conversation_id,user_id) "
                        "VALUES(?,?),(?,?)"));
                    st->setUInt64(1, conv); st->setUInt64(2, sender);
                    st->setUInt64(3, conv); st->setUInt64(4, to);
                    st->executeUpdate();
                }

                // 3) 去重：同一会话 + 发送者 + client_msg_id 已存在则直接返回旧消息
                if (clientMsgId != 0) {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT id,conversation_id,seq,sender_id,type,content,"
                        "CAST(UNIX_TIMESTAMP(created_at)*1000 AS UNSIGNED) "
                        "FROM messages "
                        "WHERE conversation_id=? AND sender_id=? AND client_msg_id=?"));
                    st->setUInt64(1, conv);
                    st->setUInt64(2, sender);
                    st->setString(3, std::to_string(clientMsgId));

                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (rs->next()) {
                        out = readStored(*rs);
                        out.to = to;
                        out.duplicate = true;
                        ok = true;      // 已拿到结果，helper 负责提交事务
                        return;         // 结束本次事务体，不再插入新消息
                    }
                }

                // 4) 原子分配会话内单调 seq
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE conversations SET last_seq=LAST_INSERT_ID(last_seq+1) WHERE id=?"));
                    st->setUInt64(1, conv);
                    st->executeUpdate();
                }
                const std::uint64_t seq = lastInsertId(conn);

                // 5) 插入消息（msg_id / ts 都由数据库生成）
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT INTO messages(conversation_id,seq,sender_id,type,content,client_msg_id) "
                        "VALUES(?,?,?,?,?,?)"));
                    st->setUInt64(1, conv);
                    st->setUInt64(2, seq);
                    st->setUInt64(3, sender);
                    st->setInt(4, type);
                    st->setString(5, content);
                    if (clientMsgId != 0) st->setString(6, std::to_string(clientMsgId));
                    else                  st->setNull(6, sql::SQLNULL);
                    st->executeUpdate();
                }
                msgId = lastInsertId(conn);

                // 6) 离线队列：先无条件写入，在线投递后由客户端 ACK 删除。
                //    这样即使接收者在发送瞬间掉线，消息也不会丢。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT IGNORE INTO offline_messages(user_id,message_id,conversation_id,seq) "
                        "VALUES(?,?,?,?)"));
                    st->setUInt64(1, to);
                    st->setUInt64(2, msgId);
                    st->setUInt64(3, conv);
                    st->setUInt64(4, seq);
                    st->executeUpdate();
                }

                // 7) 发送者自己的游标推进，避免把自己的消息算成未读
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE conversation_members "
                        "SET last_delivered_seq=GREATEST(last_delivered_seq,?), "
                        "    last_read_seq=GREATEST(last_read_seq,?) "
                        "WHERE conversation_id=? AND user_id=?"));
                    st->setUInt64(1, seq);
                    st->setUInt64(2, seq);
                    st->setUInt64(3, conv);
                    st->setUInt64(4, sender);
                    st->executeUpdate();
                }
            });
            /*************避免数据库死锁的解决方案********************/

            // 8) 读回服务端生成的 msg_id / ts（去重命中时上面的 ok 已为 true，跳过）
            if (!ok) {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT id,conversation_id,seq,sender_id,type,content,"
                    "CAST(UNIX_TIMESTAMP(created_at)*1000 AS UNSIGNED) "
                    "FROM messages WHERE id=?"));
                st->setUInt64(1, msgId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                if (rs->next()) {
                    out = readStored(*rs);
                    out.to = to;
                    ok = true;
                } else {
                    err = "message inserted but not found";
                }
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out]() { cb(ok, err, out); });
    });
}

// ============================================================================
// DAO 3：拉取离线消息
// ============================================================================
void Db::fetchOffline(std::uint64_t userId, int limit, MsgListCb cb) {
    submit([this, userId, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<StoredMessage> msgs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            if (limit <= 0)   limit = 200;
            if (limit > 500)  limit = 500;

            const std::string sql =
                "SELECT m.id,m.conversation_id,m.seq,m.sender_id,m.type,m.content,"
                "CAST(UNIX_TIMESTAMP(m.created_at)*1000 AS UNSIGNED) "
                "FROM offline_messages om "
                "JOIN messages m ON m.id=om.message_id "
                "WHERE om.user_id=? "
                "ORDER BY om.conversation_id, om.seq "
                "LIMIT " + std::to_string(limit);

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            st->setUInt64(1, userId);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) msgs.push_back(readStored(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, msgs = std::move(msgs)]() mutable {
            cb(ok, err, std::move(msgs));
        });
    });
}

// ============================================================================
// DAO 4：历史分页（keyset，按 seq 倒序取 N 条，再翻转成升序返回）
// ============================================================================
void Db::fetchHistory(std::uint64_t userId, std::uint64_t peerId,
                      std::uint64_t beforeSeq, int limit, MsgListCb cb) {
    submit([this, userId, peerId, beforeSeq, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<StoredMessage> msgs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            if (limit <= 0)  limit = 20;
            if (limit > 100) limit = 100;

            const std::string sql =
                "SELECT m.id,m.conversation_id,m.seq,m.sender_id,m.type,m.content,"
                "CAST(UNIX_TIMESTAMP(m.created_at)*1000 AS UNSIGNED) "
                "FROM messages m "
                "JOIN conversations c ON c.id=m.conversation_id "
                "WHERE c.dedup_key=? AND (?=0 OR m.seq<?) "
                "ORDER BY m.seq DESC "
                "LIMIT " + std::to_string(limit);

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            st->setString(1, dedupKey(userId, peerId));
            st->setUInt64(2, beforeSeq);
            st->setUInt64(3, beforeSeq);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) msgs.push_back(readStored(*rs));

            std::reverse(msgs.begin(), msgs.end());   // 返回按 seq 升序
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, msgs = std::move(msgs)]() mutable {
            cb(ok, err, std::move(msgs));
        });
    });
}

// ============================================================================
// DAO 5：投递 / 已读回执 + 删除已投递的离线消息
// ============================================================================
void Db::updateReceipt(std::uint64_t userId, std::uint64_t conversationId,
                       std::uint64_t deliveredSeq, std::uint64_t readSeq,
                       StatusCb cb) {
    submit([this, userId, conversationId, deliveredSeq, readSeq, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案********************/
            // P0-2：整个事务重试，遇到死锁(1213)/锁等待超时(1205)自动回滚重来。
            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE conversation_members "
                        "SET last_delivered_seq=GREATEST(last_delivered_seq,?), "
                        "    last_read_seq=GREATEST(last_read_seq,?) "
                        "WHERE conversation_id=? AND user_id=?"));
                    st->setUInt64(1, deliveredSeq);
                    st->setUInt64(2, readSeq);
                    st->setUInt64(3, conversationId);
                    st->setUInt64(4, userId);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM offline_messages "
                        "WHERE user_id=? AND conversation_id=? AND seq<=?"));
                    st->setUInt64(1, userId);
                    st->setUInt64(2, conversationId);
                    st->setUInt64(3, deliveredSeq);
                    st->executeUpdate();
                }
            });
            /*************避免数据库死锁的解决方案********************/
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

// ============================================================================
// 好友 DAO 1：用户搜索（BE-FRIEND-01）
// ----------------------------------------------------------------------------
// 搜索是典型的“低频只读”操作，因此：
//   * 走 HTTP GET，天然分页、可缓存、可限流；
//   * SQL 只 SELECT 公开字段，绝不把 password_hash / token 带出去；
//   * 排除自己，避免“结果里出现自己”这种无效条目；
//   * 同时支持用户名、昵称、精确用户ID 三种命中方式，便于客户端“输入即搜索”。
// ============================================================================
void Db::searchUsers(std::uint64_t selfId, const std::string& keyword,
                     int limit, UserListCb cb) {
    submit([this, selfId, keyword, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<UserInfo> users;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            if (limit <= 0)  limit = 20;
            if (limit > 50)  limit = 50;          // 防止一次拉取过多，给 DB/网络留余量
            const std::string like = "%" + keyword + "%";

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "SELECT id, username, nickname, avatar_url FROM users "
                "WHERE status=1 AND id<>? "
                "  AND (username LIKE ? OR nickname LIKE ? OR CAST(id AS CHAR)=?) "
                "ORDER BY (username=?) DESC, id ASC "
                "LIMIT ?"));
            st->setUInt64(1, selfId);
            st->setString(2, like);
            st->setString(3, like);
            st->setString(4, keyword);
            st->setString(5, keyword);            // 用户名精确命中的排前面
            st->setInt(6, limit);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) users.push_back(readUserInfo(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, users = std::move(users)]() mutable {
            cb(ok, err, std::move(users));
        });
    });
}

// ============================================================================
// 好友 DAO 2：创建好友申请（BE-FRIEND-02 / BE-FRIEND-03）
// ----------------------------------------------------------------------------
// 事务内的检查顺序（任何一步失败都回滚）：
//   1) 不能加自己；
//   2) 目标用户必须存在且状态正常；
//   3) 双方不能已经是好友；
//   4) 如果对方已经拉黑我，禁止申请；
//   5) 如果对方已经向我发过待处理申请，提示直接去处理，避免互相刷申请；
//   6) 同一方向的历史申请：pending 则拒绝重复提交，accepted 则说明已是好友，
//      rejected/canceled 则重置为 pending（允许重新申请）。
// 这样既保证幂等，又不会因为“被拒过一次”就永远无法再次申请。
// ============================================================================
void Db::createFriendRequest(std::uint64_t requesterId, std::uint64_t targetId,
                             const std::string& message, RequestCb cb) {
    submit([this, requesterId, targetId, message, cb]() {
        bool ok = false;
        std::string err;
        FriendRequestInfo out;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();
            std::uint64_t reqId = 0;

            /*************避免数据库死锁的解决方案********************/
            // P0-1：不再对“可能不存在的唯一键”执行 SELECT ... FOR UPDATE。
            // REPEATABLE READ 下该锁定读会加 gap lock，与并发 INSERT 需要的
            // 插入意向锁互相等待，最终触发 1213 死锁。
            // 这里改为：普通 SELECT 做业务校验 + 直接 INSERT，由唯一键
            // uk_friend_requests_pair 兜底并发；冲突(1062)时再读状态决定处理方式。
            // P0-2：整个事务交给 runTransactionWithRetry，遇到 1213/1205 自动重试。
            runTransactionWithRetry(conn, [&]() {
                reqId = 0;

                if (requesterId == targetId) throw BizError("cannot add yourself");

                // 目标用户必须存在且未被禁用。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT 1 FROM users WHERE id=? AND status=1"));
                    st->setUInt64(1, targetId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("target user not found");
                }

                // 检查双方现有的最终关系：已是好友 / 被对方拉黑。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT user_id, status FROM friends "
                        "WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)"));
                    st->setUInt64(1, requesterId);
                    st->setUInt64(2, targetId);
                    st->setUInt64(3, targetId);
                    st->setUInt64(4, requesterId);

                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    while (rs->next()) {
                        const std::uint64_t owner = rs->getUInt64(1);
                        const int status = rs->getInt(2);
                        if (status == 1) throw BizError("already friends");
                        if (status == 2 && owner == targetId)
                            throw BizError("blocked by target user");
                    }
                }

                // 反向待处理申请：对方已经申请过我，应该让我直接处理，而不是互相申请。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT 1 FROM friend_requests "
                        "WHERE requester_id=? AND target_id=? AND status=0"));
                    st->setUInt64(1, targetId);
                    st->setUInt64(2, requesterId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (rs->next())
                        throw BizError("target already sent you a pending request");
                }

                // 同一方向的申请：直接 INSERT，靠唯一键处理并发。
                try {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT INTO friend_requests"
                        "(requester_id,target_id,message,status,result_notified) "
                        "VALUES(?,?,?,0,0)"));
                    st->setUInt64(1, requesterId);
                    st->setUInt64(2, targetId);
                    st->setString(3, message);
                    st->executeUpdate();
                    reqId = lastInsertId(conn);
                } catch (sql::SQLException& e) {
                    if (e.getErrorCode() != 1062) throw;   // 非唯一键冲突交给重试/外层

                    // 已存在同一方向的申请：普通读状态，决定重复申请 / 已是好友 / 重置。
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT id, status FROM friend_requests "
                        "WHERE requester_id=? AND target_id=?"));
                    st->setUInt64(1, requesterId);
                    st->setUInt64(2, targetId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw;                // 极少数竞态，交给重试

                    reqId = rs->getUInt64(1);
                    const int oldStatus = rs->getInt(2);

                    if (oldStatus == 0) throw BizError("request already pending");
                    if (oldStatus == 1) throw BizError("already friends");

                    // 被拒绝/取消后允许重新申请：复用同一行，重置状态与时间。
                    // 带 status IN (2,3) 条件，避免并发下覆盖别人的处理结果。
                    std::unique_ptr<sql::PreparedStatement> up(conn->prepareStatement(
                        "UPDATE friend_requests "
                        "SET message=?, status=0, result_notified=0, "
                        "    created_at=CURRENT_TIMESTAMP(3) "
                        "WHERE id=? AND status IN (2,3)"));
                    up->setString(1, message);
                    up->setUInt64(2, reqId);
                    if (up->executeUpdate() == 0)
                        throw BizError("request already pending");
                }
            });
            /*************避免数据库死锁的解决方案********************/

            // 事务提交后读回完整信息（含双方用户名/昵称），供 HTTP 响应和 TCP 事件使用。
            {
                std::string sql = std::string(kRequestSelect) + "WHERE fr.id=?";
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                st->setUInt64(1, reqId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                if (!rs->next()) throw std::runtime_error("request not found after create");
                out = readFriendRequestInfo(*rs);
            }
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out]() { cb(ok, err, out); });
    });
}

// ============================================================================
// 好友 DAO 3：处理好友申请（BE-FRIEND-04）
// ----------------------------------------------------------------------------
// 关键点：
//   * 必须校验“当前登录用户 == 申请的被申请人”，防止越权替别人通过申请；
//   * SELECT ... FOR UPDATE 锁住申请行，避免同一申请被并发通过两次；
//   * 通过时在同一事务里写 friends 双向两行，保证“关系成立”是原子操作；
//   * 结果先写 result_notified=0，若申请人离线，登录时再补推。
// ============================================================================
void Db::handleFriendRequest(std::uint64_t userId, std::uint64_t requestId,
                             bool accept, RequestCb cb) {
    submit([this, userId, requestId, accept, cb]() {
        bool ok = false;
        std::string err;
        FriendRequestInfo out;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案********************/
            // P0-2：整个事务重试，遇到死锁(1213)/锁等待超时(1205)自动重来。
            // 这里的 SELECT ... FOR UPDATE 命中已存在的主键行，只加行锁不加 gap lock；
            // 真正需要兜底的是通过申请时 friends 双向写入可能产生的瞬时死锁。
            runTransactionWithRetry(conn, [&]() {
                std::uint64_t requesterId = 0;
                int status = -1;

                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT requester_id, status FROM friend_requests "
                        "WHERE id=? AND target_id=? FOR UPDATE"));
                    st->setUInt64(1, requestId);
                    st->setUInt64(2, userId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("request not found");
                    requesterId = rs->getUInt64(1);
                    status      = rs->getInt(2);
                }

                if (status != 0) throw BizError("request already handled");

                if (accept) {
                    // 双向写入好友关系。ON DUPLICATE KEY 兼容“历史行已存在”的情况。
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT INTO friends(user_id,friend_id,status) VALUES(?,?,1),(?,?,1) "
                        "ON DUPLICATE KEY UPDATE status=1"));
                    st->setUInt64(1, userId);
                    st->setUInt64(2, requesterId);
                    st->setUInt64(3, requesterId);
                    st->setUInt64(4, userId);
                    st->executeUpdate();
                }

                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE friend_requests "
                        "SET status=?, result_notified=0 "
                        "WHERE id=?"));
                    st->setInt(1, accept ? 1 : 2);
                    st->setUInt64(2, requestId);
                    st->executeUpdate();
                }
            });
            /*************避免数据库死锁的解决方案********************/
            {
                std::string sql = std::string(kRequestSelect) + "WHERE fr.id=?";
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                st->setUInt64(1, requestId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                if (!rs->next()) throw std::runtime_error("request not found after handle");
                out = readFriendRequestInfo(*rs);
            }
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out]() { cb(ok, err, out); });
    });
}

// ============================================================================
// 好友 DAO 4：申请列表（BE-FRIEND-05）
// ----------------------------------------------------------------------------
// direction 语义：
//   incoming：我收到的申请（target_id = 我）
//   outgoing：我发出的申请（requester_id = 我）
//   all     ：两者都返回
// 按 created_at 倒序，客户端做“最近申请”列表更自然。
// ============================================================================
void Db::listFriendRequests(std::uint64_t userId, const std::string& direction,
                            RequestListCb cb) {
    submit([this, userId, direction, cb]() {
        bool ok = false;
        std::string err;
        std::vector<FriendRequestInfo> reqs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::string sql = kRequestSelect;
            if (direction == "outgoing") {
                sql += "WHERE fr.requester_id=? ";
            } else if (direction == "all") {
                sql += "WHERE (fr.requester_id=? OR fr.target_id=?) ";
            } else {
                sql += "WHERE fr.target_id=? ";
            }
            sql += "ORDER BY fr.created_at DESC LIMIT 200";

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            if (direction == "all") {
                st->setUInt64(1, userId);
                st->setUInt64(2, userId);
            } else {
                st->setUInt64(1, userId);
            }

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) reqs.push_back(readFriendRequestInfo(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, reqs = std::move(reqs)]() mutable {
            cb(ok, err, std::move(reqs));
        });
    });
}

// ============================================================================
// 好友 DAO 5：好友列表（BE-FRIEND-05）
// ----------------------------------------------------------------------------
// 因为 friends 表是“双向各一行”，查我的好友只需要 WHERE user_id=?，
// 再 JOIN users 取公开资料，最后按 userId 排序，结果稳定、便于客户端 diff。
// ============================================================================
void Db::listFriends(std::uint64_t userId, FriendListCb cb) {
    submit([this, userId, cb]() {
        bool ok = false;
        std::string err;
        std::vector<FriendInfo> friends;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                /*************补索引增强MariaDB查询性能********************/
                // ORDER BY 改为 friends.friend_id：与索引
                // idx_friends_user_status(user_id,status,friend_id,remark) 的列顺序对齐，
                // 且 u.id == f.friend_id，返回顺序不变；避免额外 filesort。
                "SELECT u.id, u.username, u.nickname, u.avatar_url, f.remark, f.status "
                "FROM friends f JOIN users u ON u.id=f.friend_id "
                "WHERE f.user_id=? AND f.status=1 "
                "ORDER BY f.friend_id ASC"
                /*************补索引增强MariaDB查询性能********************/
                ));
            st->setUInt64(1, userId);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) friends.push_back(readFriendInfo(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, friends = std::move(friends)]() mutable {
            cb(ok, err, std::move(friends));
        });
    });
}

// ============================================================================
// 好友 DAO 6：修改备注（BE-FRIEND-05）
// ----------------------------------------------------------------------------
// 备注是“私有的”：只更新 user_id=我 的那一行，不影响好友自己的列表。
// ============================================================================
void Db::updateFriendRemark(std::uint64_t userId, std::uint64_t friendId,
                            const std::string& remark, StatusCb cb) {
    submit([this, userId, friendId, remark, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案********************/
            // P0-2：整个事务重试，遇到死锁(1213)/锁等待超时(1205)自动回滚重来。
            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT 1 FROM friends WHERE user_id=? AND friend_id=? AND status=1"));
                    st->setUInt64(1, userId);
                    st->setUInt64(2, friendId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("friend not found");
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE friends SET remark=? WHERE user_id=? AND friend_id=? AND status=1"));
                    st->setString(1, remark);
                    st->setUInt64(2, userId);
                    st->setUInt64(3, friendId);
                    st->executeUpdate();
                }
            });
            /*************避免数据库死锁的解决方案********************/
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

// ============================================================================
// 好友 DAO 7：删除好友（BE-FRIEND-05）
// ----------------------------------------------------------------------------
// 删除必须双向进行，否则只删一行会出现“我删了他、他还看得到我”的不一致。
// 同时清理双方待处理申请，避免删好友后又被一条旧申请“加回来”。
// ============================================================================
void Db::deleteFriend(std::uint64_t userId, std::uint64_t friendId, StatusCb cb) {
    submit([this, userId, friendId, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案********************/
            // P0-2：整个事务重试，遇到死锁(1213)/锁等待超时(1205)自动回滚重来。
            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM friends "
                        "WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)"));
                    st->setUInt64(1, userId);
                    st->setUInt64(2, friendId);
                    st->setUInt64(3, friendId);
                    st->setUInt64(4, userId);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM friend_requests "
                        "WHERE (requester_id=? AND target_id=?) "
                        "   OR (requester_id=? AND target_id=?)"));
                    st->setUInt64(1, userId);
                    st->setUInt64(2, friendId);
                    st->setUInt64(3, friendId);
                    st->setUInt64(4, userId);
                    st->executeUpdate();
                }
            });
            /*************避免数据库死锁的解决方案********************/
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

// ============================================================================
// 好友 DAO 8：拉黑 / 取消拉黑（BE-FRIEND-05）
// ----------------------------------------------------------------------------
// 拉黑：
//   1) 删除双方好友行（拉黑意味着关系解除）；
//   2) 写入 (我,对方,status=2) 的单向拉黑行；
//   3) 清理双方申请，防止对方用旧申请绕过拉黑。
// 取消拉黑：
//   只删除自己那条 status=2 的行；之后需要重新走加好友流程。
// ============================================================================
void Db::setFriendBlocked(std::uint64_t userId, std::uint64_t friendId,
                          bool blocked, StatusCb cb) {
    submit([this, userId, friendId, blocked, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案********************/
            // P0-2：整个事务重试，遇到死锁(1213)/锁等待超时(1205)自动回滚重来。
            runTransactionWithRetry(conn, [&]() {
                if (blocked) {
                    if (userId == friendId) throw BizError("cannot block yourself");

                    {
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "SELECT 1 FROM users WHERE id=? AND status=1"));
                        st->setUInt64(1, friendId);
                        std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                        if (!rs->next()) throw BizError("target user not found");
                    }
                    {
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "DELETE FROM friends "
                            "WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)"));
                        st->setUInt64(1, userId);
                        st->setUInt64(2, friendId);
                        st->setUInt64(3, friendId);
                        st->setUInt64(4, userId);
                        st->executeUpdate();
                    }
                    {
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "INSERT INTO friends(user_id,friend_id,status,remark) "
                            "VALUES(?,?,2,'') ON DUPLICATE KEY UPDATE status=2"));
                        st->setUInt64(1, userId);
                        st->setUInt64(2, friendId);
                        st->executeUpdate();
                    }
                    {
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "DELETE FROM friend_requests "
                            "WHERE (requester_id=? AND target_id=?) "
                            "   OR (requester_id=? AND target_id=?)"));
                        st->setUInt64(1, userId);
                        st->setUInt64(2, friendId);
                        st->setUInt64(3, friendId);
                        st->setUInt64(4, userId);
                        st->executeUpdate();
                    }
                } else {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM friends WHERE user_id=? AND friend_id=? AND status=2"));
                    st->setUInt64(1, userId);
                    st->setUInt64(2, friendId);
                    st->executeUpdate();
                }
            });
            /*************避免数据库死锁的解决方案********************/
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

// ============================================================================
// 好友 DAO 9：登录补推——待处理申请（BE-FRIEND-06）
// ----------------------------------------------------------------------------
// 用户离线期间可能收到多条申请。申请本身已经持久化在 friend_requests，
// 登录时按“待处理 + 按时间升序”拉出来逐条推送，即可实现离线补偿。
// ============================================================================
void Db::fetchPendingIncoming(std::uint64_t userId, int limit, RequestListCb cb) {
    submit([this, userId, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<FriendRequestInfo> reqs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            if (limit <= 0)  limit = 100;
            if (limit > 200) limit = 200;

            std::string sql = std::string(kRequestSelect) +
                "WHERE fr.target_id=? AND fr.status=0 "
                "ORDER BY fr.created_at ASC LIMIT ?";
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            st->setUInt64(1, userId);
            st->setInt(2, limit);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) reqs.push_back(readFriendRequestInfo(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, reqs = std::move(reqs)]() mutable {
            cb(ok, err, std::move(reqs));
        });
    });
}

// ============================================================================
// 好友 DAO 10：登录补推——未通知的申请结果（BE-FRIEND-06）
// ----------------------------------------------------------------------------
// 申请被通过/拒绝时，如果申请人不在线，result_notified 会保持 0。
// 申请人登录后把 status IN (1,2) 且未通知的结果推给他，然后由
// markResultsNotified() 统一置 1。
// ============================================================================
void Db::fetchUnnotifiedResults(std::uint64_t userId, int limit, RequestListCb cb) {
    submit([this, userId, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<FriendRequestInfo> reqs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            if (limit <= 0)  limit = 100;
            if (limit > 200) limit = 200;

            std::string sql = std::string(kRequestSelect) +
                "WHERE fr.requester_id=? AND fr.status IN (1,2) "
                "  AND fr.result_notified=0 "
                "ORDER BY fr.updated_at ASC LIMIT ?";
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            st->setUInt64(1, userId);
            st->setInt(2, limit);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) reqs.push_back(readFriendRequestInfo(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, reqs = std::move(reqs)]() mutable {
            cb(ok, err, std::move(reqs));
        });
    });
}

// ============================================================================
// 好友 DAO 11：精确标记某条申请结果已通知（BE-FRIEND-06）
// ----------------------------------------------------------------------------
// 用于“申请人正好在线，HTTP 处理完申请后已经实时推送”的场景。
// 只更新这一条申请，避免把同一个人其他还没推的结果一起标记掉。
// ============================================================================
void Db::markRequestResultNotified(std::uint64_t requestId, StatusCb cb) {
    submit([this, requestId, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "UPDATE friend_requests SET result_notified=1 "
                "WHERE id=? AND status IN (1,2)"));
            st->setUInt64(1, requestId);
            st->executeUpdate();
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

// ============================================================================
// 好友 DAO 12：批量标记申请结果已通知（BE-FRIEND-06）
// ----------------------------------------------------------------------------
// 用于登录补偿：一次性拉取并推送后，把该用户所有“已产生结果且未通知”的
// 行置 1。WHERE 条件保证重复调用是幂等的，不会重复推送。
// ============================================================================
void Db::markResultsNotified(std::uint64_t userId, StatusCb cb) {
    submit([this, userId, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "UPDATE friend_requests SET result_notified=1 "
                "WHERE requester_id=? AND status IN (1,2) AND result_notified=0"));
            st->setUInt64(1, userId);
            st->executeUpdate();
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

} // namespace kc
