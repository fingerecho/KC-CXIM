#include "db.h"

#include <mariadb/conncpp.hpp>          // MariaDB Connector/C++（同步阻塞 API）
#include <boost/asio/post.hpp>

#include <algorithm>
#include <deque>
#include <iostream>
#include <stdexcept>

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

} // namespace

// ============================================================================
// ConnectionPool：固定大小的连接池
// ============================================================================
class ConnectionPool {
public:
    ConnectionPool(const DbConfig& cfg, std::size_t size) : cfg_(cfg) {
        for (std::size_t i = 0; i < size; ++i) {
            free_.push_back(makeConn());
        }
    }

    // 借一条连接；池被停止时返回 nullptr。借出的连接会做健康检查，
    // 坏连接会被自动重建。
    std::unique_ptr<sql::Connection> acquire() {
        std::unique_ptr<sql::Connection> conn;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return stopping_ || !free_.empty(); });
            if (free_.empty()) return nullptr;
            conn = std::move(free_.front());
            free_.pop_front();
        }

        try {
            if (conn && !conn->isClosed() && conn->isValid()) return conn;
        } catch (...) {
            // 落到下面重建
        }

        try { return makeConn(); }
        catch (const std::exception& e) {
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

    // 归还连接。如果连接已经坏了，尝试重建一条再放回池里。
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
            try { conn = makeConn(); }
            catch (const std::exception& e) {
                std::cerr << "[db] rebuild connection failed: " << e.what() << std::endl;
                return;
            }
        }
        free_.push_back(std::move(conn));
        cv_.notify_one();
    }

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
// DAO 2：单聊发送（一个事务内完成 seq/消息/离线队列/游标）
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
            conn->setAutoCommit(false);

            try {
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
                        conn->commit();
                        ok = true;
                        postToIo([cb, ok, err, out]() { cb(ok, err, out); });
                        return;   // 已投递结果，直接结束本次任务
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
                const std::uint64_t msgId = lastInsertId(conn);

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

                conn->commit();

                // 8) 读回服务端生成的 msg_id / ts
                {
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
                try { conn->rollback(); } catch (...) {}
                err = std::string("db error: ") + e.what();
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
            conn->setAutoCommit(false);

            try {
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
                conn->commit();
                ok = true;
            } catch (const std::exception& e) {
                try { conn->rollback(); } catch (...) {}
                err = std::string("db error: ") + e.what();
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    });
}

} // namespace kc
