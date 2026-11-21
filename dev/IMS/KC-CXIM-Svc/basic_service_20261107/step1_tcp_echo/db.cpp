#include "db.h"

#include <mariadb/conncpp.hpp>          // MariaDB Connector/C++（同步阻塞 API）
#include <boost/asio/post.hpp>          // boost::asio::post：把结果投递回 io_context 线程
#include <bcrypt/BCrypt.hpp>            // bcrypt：生成/校验密码哈希（重 CPU，必须在 worker 线程执行）
#include "quiet_log.h"                   // logFatal：连接失败/worker 异常/队列超阈不能被 QUIET_LOG 吞掉

#include <atomic>          // 队列超限告警的限频计数
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>     // 批量好友申请：按目标 id 归档关系/申请状态
#include <unordered_set>     // 批量好友申请：存在性/反向 pending 集合

namespace kc {

const char* Db::kBusyError = "server busy";   // P0-4：HTTP 层据此回 503 + Retry-After

namespace {

// SQLString（Connector 自己的字符串类）-> std::string
std::string sstr(const sql::SQLString& s) {
    return std::string(s.c_str(), s.length());
}

/*************P0-6：记录"本线程最近是否开过事务"***************/
// 归还连接到池里时，只有真正开过事务的连接才需要 RESET autocommit（1 个网络
// 往返）。纯 SELECT 的任务不需要。约定：一个 worker 线程一次只跑一个任务，
// 任务开始（PooledConn 构造）时清零标记，事务 helper 里置位。
thread_local bool t_txnUsed = false;
/*************P0-6：记录"本线程最近是否开过事务"***************/

// 读取"当前连接"上最近一次 AUTO_INCREMENT / LAST_INSERT_ID(expr) 的值。
// 必须在同一个连接、同一个事务里调用。
//
// /*************P0-6：为什么没有改成 conn->getLastInsertId()***************/
// MariaDB Connector/C++ 1.1.9 的 sql::Connection / sql::Statement 都没有暴露
// getLastInsertId()（只有 JDBC 风格的 getGeneratedKeys()，而本处有一半的调用是
// LAST_INSERT_ID(expr) 这种"借这个通道回传我自己算的值"的用法：会话 id 的
// ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)、seq 的
// UPDATE conversations SET last_seq=LAST_INSERT_ID(last_seq+1)），
// getGeneratedKeys() 覆盖不了后者，因此统一保留这条 SELECT。
// 省下来的往返来自：注册去掉前置查重 SELECT、池健康检查降频、
// 以及消息落库后"读回整行"与"取 id"合并成一条语句（见 sendTxnBody）。
// /*************P0-6：为什么没有改成 conn->getLastInsertId()***************/
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
    // 退避时间是在 worker 线程里 sleep，会占用一个并发额度：
    // 因此退避上限刻意保持很小（<=16ms），靠"随机抖动"错开而不是靠长等待。
    thread_local std::uint32_t s_rng =
        static_cast<std::uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        t_txnUsed = true;                  // P0-6：告诉池子"这条连接需要 RESET"
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
                // xorshift 代替 std::rand()：无全局锁，也避免多线程同时取到同一退避值。
                s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
                const int jitter = 5 * attempt + static_cast<int>(s_rng % 12);
                std::this_thread::sleep_for(std::chrono::milliseconds(jitter));
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
    // 空闲超过该时长的连接，借出时做一次 ping（覆盖 server 侧 wait_timeout 杀连接）。
    // 频繁借还的连接不再 ping —— 原来"每次借出 + 每次归还"各 ping 一次，
    // 相当于给每个 DB 任务凭空加 2 个网络往返（P0-6）。
    static constexpr std::chrono::seconds kPingIdle{30};

    ConnectionPool(const DbConfig& cfg, std::size_t size)
        : cfg_(cfg), max_size_(std::max<std::size_t>(1, size)) {
        /*************并发登录/注册优化：连接池懒创建***************/
        // 启动时只建 1 条连接做连通性校验；其余连接在 acquire() 时按需创建。
        // 这样不会在启动瞬间占满 MariaDB 的 max_connections（避免
        // "Too many connections"），同时仍能按并发逐步扩到 max_size_。
        free_.push_back(Item{makeConn(), Clock::now()});
        created_ = 1;
        /*************并发登录/注册优化：连接池懒创建***************/
    }

    // 借一条连接；池被停止时返回 nullptr。借出的连接会做健康检查，
    // 坏连接会被丢弃，后续 acquire() 再按需重建。
    std::unique_ptr<sql::Connection> acquire() {
        std::unique_ptr<sql::Connection> conn;
        bool needPing = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            for (;;) {
                if (stopping_) return nullptr;

                if (!free_.empty()) {
                    Item item = std::move(free_.front());
                    free_.pop_front();
                    conn = std::move(item.conn);
                    // P0-6：只在"放了很久"时才 ping，热连接零额外往返。
                    needPing = (Clock::now() - item.since) > kPingIdle;
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
                        logFatal(std::string("[db] create connection failed: ") + e.what());
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

        /*************P0-6：健康检查按需做，不再无条件 ping***************/
        // isClosed() 只读本地状态、不产生网络往返，用来快速剔除明显已断的连接。
        bool healthy = false;
        try {
            if (conn && !conn->isClosed()) {
                healthy = needPing ? conn->isValid() : true;
            }
        } catch (...) {
            healthy = false;      // ping 失败：这条连接已废，丢弃重建
        }
        if (healthy) return conn;
        /*************P0-6：健康检查按需做，不再无条件 ping***************/

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
            logFatal(std::string("[db] reconnect failed: ") + e.what());
            return nullptr;
        }
    }

    // 停止连接池：唤醒所有等待者，让析构流程不会卡在 acquire()。
    void stop() {
        std::lock_guard<std::mutex> lk(mutex_);
        stopping_ = true;
        cv_.notify_all();
    }

    /*************P0-6：归还连接不再 ping***************/
    // needReset 由调用方（PooledConn）根据"本任务是否开过事务"给出：
    //   * 纯 SELECT 任务归还时零网络往返；
    //   * 只有真正 setAutoCommit(false) 过的连接才需要 RESET 回 autocommit。
    // 健康判断只用本地 isClosed()；服务端侧断连靠"下一次使用时抛错 + 重建"
    // 以及空闲超时 ping 兜住。
    void release(std::unique_ptr<sql::Connection> conn, bool needReset) {
        bool healthy = false;
        if (conn) {
            try {
                if (!conn->isClosed()) {
                    if (needReset) conn->setAutoCommit(true);
                    healthy = true;
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
        free_.push_back(Item{std::move(conn), Clock::now()});
        cv_.notify_one();
    }
    /*************P0-6：归还连接不再 ping***************/

    std::size_t max_size() const { return max_size_; }

private:
    using Clock = std::chrono::steady_clock;

    // 池里的一条连接 + 它被放回的时刻（用于 P0-6 的空闲超时 ping 判断）。
    struct Item {
        std::unique_ptr<sql::Connection> conn;
        Clock::time_point                 since;
    };
    std::unique_ptr<sql::Connection> makeConn() {
        sql::Driver* driver = sql::mariadb::get_driver_instance();
        /*************core-dump 加固：驱动/连接的返回值必须查空***************/
        // Connector/C++ 1.1.9 的失败路径是"多数抛异常、少数返回空指针"混着来的
        // （历史行为）。这里原本是直接 driver->connect(...) 构造 unique_ptr 再
        // conn->setAutoCommit(true)：一旦哪天返回空，就是**worker 线程里的空指针
        // 解引用**，症状正好是"压测上量时 SEGV core-dump"（本函数只在连接池
        // 首次按需扩容、以及坏连接重建时被调用 —— 都是负载最陡的时刻）。
        // 换成异常以后，调用方 acquire() 的 catch 会记一条 [db] create
        // connection failed 并按既定逻辑回退，行为可控。
        if (!driver) throw std::runtime_error("mariadb driver unavailable");

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
        if (!conn) throw std::runtime_error("driver->connect returned null");
        conn->setAutoCommit(true);
        return conn;
    }

    DbConfig cfg_;
    std::size_t max_size_ = 1;   // 连接池上限（按需扩容，不会启动时全部创建）
    std::size_t created_  = 0;   // 当前已创建（含借出）的连接数
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> free_;
    bool stopping_ = false;
};

namespace {

// RAII：构造时借连接，析构时归还。异常路径也能保证归还。
class PooledConn {
public:
    explicit PooledConn(ConnectionPool& pool)
        : pool_(pool), conn_(pool.acquire()) {
        t_txnUsed = false;   // P0-6：本任务是否开过事务，从这里开始重新计数
    }
    ~PooledConn() {
        if (conn_) pool_.release(std::move(conn_), t_txnUsed);
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
// DbRuntime：运行期参数（环境变量）
// ============================================================================
DbRuntime DbRuntime::fromEnv() {
    DbRuntime rt;
    auto envInt  = [](const char* name, long long def) -> long long {
        const char* v = std::getenv(name);
        if (!v || !*v) return def;
        try { return std::stoll(v); } catch (...) { return def; }
    };
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());

    // P0-1：bcrypt 成本因子。每 +1 算力翻倍、每 -1 减半。
    // 允许区间刻意收窄到 [8,15]：低于 8 就脱离了"故意慢"的意义上限，
    // 离线爆破成本几乎可以忽略，那不是"调优"而是"关掉防护"。
    // 10 是当前公认基线（OWASP 口令存储建议的最小 work factor），
    // 2 核机器上想再往下降到 8，必须同时确认登录限流参数已收紧。
    const long long cost = envInt("BCRYPT_COST", rt.bcryptCost);
    if (cost >= 8 && cost <= 15) rt.bcryptCost = static_cast<int>(cost);

    // P0-2/P0-7：SQL worker 与连接池上限保持一致。
    // 同时被占用的连接不可能多于 SQL worker，所以 poolSize > sqlWorkers 没有意义，
    // 只会让读代码的人误以为"还能再扩"。默认 2*核数，上限 8（2 核机器给 DB 留算力）。
    long long w = envInt("DB_WORKER_COUNT", static_cast<long long>(hw) * 2);
    if (w < 1) w = 1;
    if (w > 8) w = 8;
    rt.sqlWorkers = static_cast<std::size_t>(w);

    long long p = envInt("DB_POOL_SIZE", rt.sqlWorkers);
    if (p < 1) p = 1;
    rt.poolSize = static_cast<std::size_t>(p);

    // P0-1/P0-2：哈希 worker 默认 = 核数。超订不会提高吞吐，只会让
    // 所有线程（含 mariadbd）的延迟一起变差。
    long long h = envInt("DB_HASH_WORKER_COUNT", static_cast<long long>(hw));
    if (h < 1) h = 1;
    if (h > 8) h = 8;
    rt.hashWorkers = static_cast<std::size_t>(h);

    long long q = envInt("DB_QUEUE_LIMIT", static_cast<long long>(rt.queueLimit));
    if (q >= 16) rt.queueLimit = static_cast<std::size_t>(q);

    rt.offlineCursor = envInt("OFFLINE_CURSOR", 0) != 0;   // P1-4

    long long ttl = envInt("RELATION_CACHE_TTL", rt.relationCacheTtlMs);
    if (ttl >= 0 && ttl <= 600000) rt.relationCacheTtlMs = ttl;   // P1-1（0=关闭）
    long long rmax = envInt("RELATION_CACHE_MAX",
                            static_cast<long long>(rt.relationCacheMax));
    if (rmax >= 1) rt.relationCacheMax = static_cast<std::size_t>(rmax);

    return rt;
}

// ============================================================================
// Db：构造 / 销毁 / 双线程池
// ============================================================================
Db::Db(boost::asio::io_context& io, const DbConfig& cfg, const DbRuntime& rt)
    : io_(io),
      rt_(rt),
      pool_(std::make_unique<ConnectionPool>(cfg, rt.poolSize)) {
    std::cout << "[db] pool max=" << pool_->max_size()
              << ", sqlWorkers=" << rt_.sqlWorkers
              << ", hashWorkers=" << rt_.hashWorkers
              << ", bcryptCost=" << rt_.bcryptCost
              << ", queueLimit=" << rt_.queueLimit
              << ", offlineCursor=" << (rt_.offlineCursor ? 1 : 0)
              << ", relationCacheTtlMs=" << rt_.relationCacheTtlMs
              << " (connections are created lazily)" << std::endl;

    for (std::size_t i = 0; i < rt_.sqlWorkers; ++i) {
        sqlWorkers_.emplace_back([this] { workerLoop(sqlQueue_); });
    }
    for (std::size_t i = 0; i < rt_.hashWorkers; ++i) {
        hashWorkers_.emplace_back([this] { workerLoop(hashQueue_); });
    }
}

Db::~Db() {
    // 先停 SQL（可能还有任务在等连接），再停 hash，最后关池唤醒所有等待者。
    {
        std::lock_guard<std::mutex> lk(sqlQueue_.mutex);
        sqlQueue_.stopping = true;
    }
    sqlQueue_.cv.notify_all();
    {
        std::lock_guard<std::mutex> lk(hashQueue_.mutex);
        hashQueue_.stopping = true;
    }
    hashQueue_.cv.notify_all();
    if (pool_) pool_->stop();   // 唤醒卡在 acquire() 的 worker
    for (auto& t : sqlWorkers_)  { if (t.joinable()) t.join(); }
    for (auto& t : hashWorkers_) { if (t.joinable()) t.join(); }
}

/*************P0-2/P0-4：分队列投递 + 队列深度上限（背压）***************/
bool Db::submitSql(JobQueue& q, std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(q.mutex);
        if (q.stopping) return false;
        // P0-4：队列不再无界。打满时让调用方立即失败（上层回 503 + Retry-After），
        // 而不是把所有请求都拖成几十秒 —— 过载时"拒掉一部分"比"全体超时"好。
        if (q.jobs.size() >= rt_.queueLimit) return false;
        q.jobs.push(std::move(job));
    }
    q.cv.notify_one();
    return true;
}

bool Db::submitHash(std::function<void()> job) {
    return submitSql(hashQueue_, std::move(job));
}

void Db::workerLoop(JobQueue& q) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(q.mutex);
            q.cv.wait(lk, [&q] { return q.stopping || !q.jobs.empty(); });
            if (q.stopping && q.jobs.empty()) return;
            job = std::move(q.jobs.front());
            q.jobs.pop();
        }

        try {
            job();
        } catch (const std::exception& e) {
            // 兜底：DAO 内部已经捕获异常；这里防止意外异常杀死 worker。
            logFatal(std::string("[db] worker exception (DAO 未自处理): ") + e.what());
        } catch (...) {
            logFatal("[db] worker unknown exception (DAO 未自处理)");
        }
    }
}
/*************P0-2/P0-4：分队列投递 + 队列深度上限（背压）***************/

// 兼容投递口：SQL 队列，**软上限、绝不丢任务**（原因见 db.h 的注释）。
void Db::submit(std::function<void()> job) {
    submitAlways(std::move(job));
}

void Db::submitAlways(std::function<void()> job) {
    std::size_t depth;
    {
        std::lock_guard<std::mutex> lk(sqlQueue_.mutex);
        if (sqlQueue_.stopping) return;      // 进程正在退出，没有人再消费结果了
        depth = sqlQueue_.jobs.size();
        sqlQueue_.jobs.push(std::move(job));
    }
    sqlQueue_.cv.notify_one();

    if (depth >= rt_.queueLimit) {
        // 只告警不丢弃。日志里持续出现就说明入口层的阈值该收紧了
        // （HTTP_MAX_INFLIGHT / TCP_MAX_OUTSTANDING / AUTH_RATE_PER_SEC）。
        // 限频交给 logFatal（每秒 20 条，超出只累计并附 "+N suppressed"），
        // 这里不再自己写一套计数——两套限频同时存在时，真正该看到的那几条
        // 反而会被自己压掉。
        logFatal("[db] sql queue over soft limit: depth=" + std::to_string(depth) +
                 " cap=" + std::to_string(rt_.queueLimit));
    }
}

std::size_t Db::sqlQueueDepth() {
    std::lock_guard<std::mutex> lk(sqlQueue_.mutex);
    return sqlQueue_.jobs.size();
}

std::size_t Db::hashQueueDepth() {
    std::lock_guard<std::mutex> lk(hashQueue_.mutex);
    return hashQueue_.jobs.size();
}

void Db::postToIo(std::function<void()> fn) {
    // 结果必须回到 io_context 线程再碰 session/socket，避免多线程数据竞争。
    boost::asio::post(io_, std::move(fn));
}

// ============================================================================
// P1-1：好友关系缓存（LRU + TTL + 失效版本号）
// ----------------------------------------------------------------------------
// 只缓存"关系判定所需的原始 status"，不缓存结论，这样 blocked/notFriends 的
// 文案判断与 SQL 版本完全一致。命中条件：未过期。
//
// 为什么必须有 relEpoch_（评审发现的真实竞态）：
//   worker X（sendChecked A→B）缓存未命中 → SELECT 得到 (1,1)；
//   就在 X 还没写回缓存时，worker Y 完成了"B 删除/拉黑 A"并提交 →
//   Y 调 relationCacheErase(A,B)，可此时缓存里根本没有这一条，erase 是空操作；
//   然后 X 才 relationCachePut(A,B,1,1) —— 一条**已经过期**的"是好友"记录
//   就这样活满了整个 TTL（默认 10s），正好命中"拉黑必须即时生效"这条产品红线。
// 修法：任何 erase 都把全局版本号 +1；读取时记下版本，写回时版本已变就放弃缓存。
// 代价只是"少缓存一条"，而错缓存的代价是"该拦的消息发出去了"。
// ============================================================================
std::uint64_t Db::relationCacheEpoch() const {
    return relEpoch_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// core-dump 修复 ⑪：结构自检（调用方必须已持 relMu_）
// ---------------------------------------------------------------------------
// 为什么要有这一步：本轮的栈顶是
//     std::__detail::_List_node_base::_M_unhook()   ← 写 prev->_M_next 时打到 0x33
// 全工程只有 relLru_ 这一个 std::list，所以被踩坏的就是它的节点。
// 在这种状态下**继续**拿一个 iterator 去 `list::erase()`，就是再走一次同样的解引用：
// 崩溃点会从"运气好落在 Put/Get"变成"任意一次删除好友/发消息"。
// 所以每次进锁先做 O(1) 的两张容器大小比对：
//   * 相等   → 正常走；
//   * 不等   → 缓存结构已被破坏（某条路径只动了一张表，或有外部野写踩了节点）。
//     此时把整张 LRU **换掉而不是清掉**：list::clear() 要顺着链表走一遍并逐个 free，
//     链已经不可信，那本身就是另一次踩内存；swap 是 O(1) 只换哨兵，不碰任何节点。
//     换下来的那张表**故意不释放**（有界泄漏，只在已损坏时发生），
//     用"泄漏几十 KB"换"不 SEGV"，对一台 2GB 的机器也是划算的。
//     relMap_ 是 unordered_map，节点之间没有链，可以正常 clear。
bool Db::relationCacheSaneLocked(const char* where) {
    if (relMap_.size() == relLru_.size()) return true;

    logFatal("[db] relation cache inconsistent at " + std::string(where) +
             ": map=" + std::to_string(relMap_.size()) +
             " lru=" + std::to_string(relLru_.size()) +
             "; dropping the whole cache instead of erasing by iterator");

    // 故意泄漏：不要对已损坏的链表做任何遍历/释放。
    static RelationLruList* const garbage = new RelationLruList();  // 单线程到这里为止还有锁，安全
    garbage->swap(relLru_);
    relMap_.clear();
    return false;
}

// ---------------------------------------------------------------------------
// core-dump 修复 ⑩：删除 LRU 条目的唯一出口（调用方必须已持 relMu_）
// ---------------------------------------------------------------------------
// iterator 存在 entry 里，删的时候只用它一次并把 lruLinked 立刻置假：
// 同一个 entry 被删两次不可能（第二次 find 已经找不到了），
// "表里有 key、list 里没节点"也不可能 —— 因为只有这一个出口会 erase 节点。
void Db::relationCacheDropLocked(RelationMapIter it) {
    if (it->second.lruLinked) {
        relLru_.erase(it->second.lruIt);
        it->second.lruLinked = false;
    }
    relMap_.erase(it);
}

bool Db::relationCacheGet(std::uint64_t a, std::uint64_t b,
                          int& forwardStatus, int& reverseStatus) {
    if (rt_.relationCacheTtlMs <= 0) return false;
    const RelationKey key{std::min(a, b), std::max(a, b)};

    std::lock_guard<std::mutex> lk(relMu_);
    if (!relationCacheSaneLocked("get")) return false;

    auto it = relMap_.find(key);
    if (it == relMap_.end()) return false;
    if (std::chrono::steady_clock::now() >= it->second.expire) {
        relationCacheDropLocked(it);           // 过期条目：两张表一起走唯一出口
        return false;
    }
    forwardStatus = it->second.forwardStatus;
    reverseStatus = it->second.reverseStatus;

    // LRU 提到队首。注意：splice **不会**使被移动元素的 iterator 失效
    //（标准保证），所以这里**不需要**再回写 entry.lruIt ——
    // 旧版那句 `lit->second = relLru_.begin();` 就是在手动补一次同步，
    // 而"手动补同步"正是这类结构最容易出错的地方（修复⑩要消掉的就是它）。
    if (it->second.lruLinked)
        relLru_.splice(relLru_.begin(), relLru_, it->second.lruIt);
    return true;
}

void Db::relationCachePut(std::uint64_t a, std::uint64_t b,
                          int forwardStatus, int reverseStatus,
                          std::uint64_t epochAtRead) {
    if (rt_.relationCacheTtlMs <= 0) return;
    // 读之后发生过任何一次失效 → 这次读到的结果已经不可信，直接不缓存。
    if (relEpoch_.load(std::memory_order_acquire) != epochAtRead) return;

    const RelationKey key{std::min(a, b), std::max(a, b)};
    const auto ttl = std::chrono::milliseconds(rt_.relationCacheTtlMs);

    std::lock_guard<std::mutex> lk(relMu_);
    if (!relationCacheSaneLocked("put")) return;
    // 拿到锁后再查一次版本：等锁期间可能正好有人 erase。
    if (relEpoch_.load(std::memory_order_acquire) != epochAtRead) return;

    auto it = relMap_.find(key);
    if (it != relMap_.end()) {
        it->second.forwardStatus = forwardStatus;
        it->second.reverseStatus = reverseStatus;
        it->second.expire = std::chrono::steady_clock::now() + ttl;
        if (it->second.lruLinked)
            relLru_.splice(relLru_.begin(), relLru_, it->second.lruIt);
        return;
    }

    // 满了先淘汰尾部。victim 的 iterator 就在它的 entry 里，走同一个出口删。
    while (relMap_.size() >= rt_.relationCacheMax && !relLru_.empty()) {
        const RelationKey victim = relLru_.back();
        relLru_.pop_back();                      // 节点已摘掉，别再 erase 一次
        auto vit = relMap_.find(victim);
        if (vit != relMap_.end()) { vit->second.lruLinked = false; relMap_.erase(vit); }
    }

    relLru_.push_front(key);
    RelationEntry e;
    e.forwardStatus = forwardStatus;
    e.reverseStatus = reverseStatus;
    e.expire        = std::chrono::steady_clock::now() + ttl;
    e.lruIt         = relLru_.begin();           // 先填好 iterator 再入表
    e.lruLinked     = true;
    relMap_.emplace(key, e);
}

void Db::relationCacheErase(std::uint64_t a, std::uint64_t b) {
    // 先递增全局失效版本号，让"读到旧值后才写回"的并发读放弃缓存
    // （详见 relationCachePut 的竞态说明）。即使本次 key 不在缓存里，
    // 这个递增也必须发生。
    relEpoch_.fetch_add(1, std::memory_order_release);

    const RelationKey key{std::min(a, b), std::max(a, b)};
    std::lock_guard<std::mutex> lk(relMu_);
    if (!relationCacheSaneLocked("erase")) return;
    auto it = relMap_.find(key);
    if (it != relMap_.end()) relationCacheDropLocked(it);
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
// DAO 2：注册（bcrypt 哈希 + 插入 users）
// ----------------------------------------------------------------------------
// /*************P0-1/P0-2：两段式，哈希不再占用数据库连接与 SQL 队列***************/
// 原来"借连接 -> 算 bcrypt -> INSERT -> 还连接"是一个任务：
//   * 一条 DB 连接被一次哈希白白占住 100+ms；
//   * 更要命的是这个任务排在 SQL 队列里，100 个并发注册会把所有 SQL worker
//     变成"哈希工人"，发消息/查好友这些不相干的请求跟着一起排队。
// 现在拆成两步：hashPool 只算哈希（不碰连接），算完再把 INSERT 投给 sqlPool。
// 队列容量不足时立刻回 kBusyError，让上层能马上拿到 503 而不是干等超时。
// /*************P0-1/P0-2：两段式，哈希不再占用数据库连接与 SQL 队列***************/
//
// 其余要点：
//   * 成本因子来自 rt_.bcryptCost（P0-1，默认 10，可用 BCRYPT_COST 调）；
//     哈希串自带 $2a$<cost>$ 前缀，所以降成本只影响新注册用户，
//     老用户登录仍按其原始 cost 校验，不会被新策略放大。
//   * 不再"先 SELECT 查重名"（P0-6 省 1 个网络往返）：直接 INSERT，
//     由 users.uk_users_username 唯一键兜底，1062 就是"用户名已存在"。
// ============================================================================
void Db::registerUser(const std::string& username, const std::string& password,
                      RegisterCb cb) {
    if (!submitHash([this, username, password, cb]() {
        // ---- 第 1 步：纯 CPU，算哈希（hashPool，不持有连接）----
        std::string hash;
        try {
            hash = BCrypt::generateHash(password, rt_.bcryptCost);
        } catch (const std::exception& e) {
            std::string err = std::string("hash failed: ") + e.what();
            postToIo([cb, err]() { cb(false, err, 0); });
            return;
        }

        // ---- 第 2 步：写库（sqlPool）----
        if (!submitSql(sqlQueue_, [this, username, hash, cb]() {
            bool ok = false;
            std::string err;
            std::uint64_t userId = 0;

            try {
                PooledConn pc(*pool_);
                if (!pc) throw std::runtime_error("db pool unavailable");

                std::unique_ptr<sql::PreparedStatement> st(
                    pc->prepareStatement(
                        "INSERT INTO users(username,password_hash,nickname) VALUES(?,?,?)"));
                st->setString(1, username);
                st->setString(2, hash);                 // 只存哈希，禁止明文
                st->setString(3, username);             // 昵称缺省用用户名
                try {
                    st->executeUpdate();
                    userId = lastInsertId(pc.get());    // 同一连接上取自增ID
                    ok = (userId != 0);
                    if (!ok) err = "failed to create user";
                } catch (sql::SQLException& e) {
                    // 1062 = 唯一键冲突：并发注册同一用户名时由数据库兜底。
                    if (e.getErrorCode() == 1062) err = "username already exists";
                    else err = std::string("db error: ") + e.what();
                }
            } catch (const std::exception& e) {
                err = std::string("db error: ") + e.what();
            }

            postToIo([cb, ok, err, userId]() { cb(ok, err, userId); });
        })) {
            postToIo([cb]() { cb(false, kBusyError, 0); });
        }
    })) {
        postToIo([cb]() { cb(false, kBusyError, 0); });
    }
}

// ============================================================================
// DAO 3：HTTP 登录（按 username 查用户 + bcrypt verify 校验密码）
// ----------------------------------------------------------------------------
// P0-2：同样拆成"读库(sqlPool) -> 校验(hashPool)"两段。
// 读到的连接在校验之前就归还，长哈希不再占着连接；
// 校验任务也不再和 SQL 任务抢同一组 worker。
// 注意：cost 由哈希串自己决定（validatePassword 读 $2a$<cost>$），因此降成本
// 不会让老用户的登录变"更快但不安全"，也不会校验失败。
// ============================================================================
void Db::loginUser(const std::string& username, const std::string& password,
                   LoginUserCb cb) {
    if (!submitSql(sqlQueue_, [this, username, password, cb]() {
        bool found   = false;
        bool disabled = false;
        std::string err;
        std::string hash;
        std::uint64_t id = 0;
        std::string name = username;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::unique_ptr<sql::PreparedStatement> st(
                pc->prepareStatement(
                    "SELECT id, username, password_hash, status FROM users WHERE username=?"));
            st->setString(1, username);
            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());

            if (!rs->next()) {
                err = "user not found";                     // 用户不存在
            } else {
                id     = rs->getUInt64(1);
                name   = sstr(rs->getString(2));
                hash   = sstr(rs->getString(3));
                const int status = rs->getInt(4);
                if (status != 1) disabled = true;           // 被禁用则拒绝
                else             found    = true;
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        if (!err.empty()) {                                 // 查询本身失败，不必再校验
            postToIo([cb, err, name]() { cb(false, err, 0, name); });
            return;
        }
        if (disabled) {
            postToIo([cb, name]() { cb(false, "account disabled", 0, name); });
            return;
        }
        if (!found) {
            postToIo([cb, err, name]() { cb(false, err.empty() ? "bad credential" : err, 0, name); });
            return;
        }

        // ---- 第 2 步：CPU 校验（hashPool）----
        if (!submitHash([this, password, hash, id, name, cb]() {
            bool ok = false;
            std::string verr;
            try {
                ok = BCrypt::validatePassword(password, hash);
                if (!ok) verr = "bad credential";
            } catch (const std::exception& e) {
                verr = std::string("hash failed: ") + e.what();
            }
            postToIo([cb, ok, verr, id, name]() { cb(ok, verr, id, name); });
        })) {
            postToIo([cb, name]() { cb(false, kBusyError, 0, name); });
        }
    })) {
        postToIo([cb, username]() { cb(false, kBusyError, 0, username); });
    }
}

// ============================================================================
// 单聊发送的事务体（DAO 4 与 DAO 4' 共用）
// ----------------------------------------------------------------------------
// /*************P0-6 + P1-4：把一条消息的 DB 往返数压下来***************/
// 原来一条消息在一个事务里要走 10 次网络往返：
//   SELECT 1 FROM users      （接收者存在性）
//   INSERT conversations     （取/建会话）      + SELECT LAST_INSERT_ID()
//   INSERT IGNORE conversation_members
//   SELECT ... 去重
//   UPDATE last_seq          （分配 seq）       + SELECT LAST_INSERT_ID()
//   INSERT messages                              + SELECT LAST_INSERT_ID()
//   INSERT offline_messages
//   UPDATE conversation_members（发送者游标）
//   SELECT ... FROM messages WHERE id=?          （回读整行）
// 现在：
//   * "取 msg_id" 与 "回读整行" 合并为一条 SELECT ... WHERE id=LAST_INSERT_ID()；
//   * checkRecipient=false（关系已在同事务内校验过）时跳过接收者存在性查询：
//     friends 行对 users.id 有外键，关系成立即意味着接收者存在；
//   * offlineCursor=true（P1-4）时不再写 offline_messages 行级队列，
//     离线补推改由 conversation_members.last_delivered_seq 游标推导。
// 合计 10 次 → 8 次（关系校验 + 游标模式的 sendChecked 路径）。
// /*************P0-6 + P1-4：把一条消息的 DB 往返数压下来***************/
//
// 调用约定：必须已经处在事务里（由 runTransactionWithRetry 负责开事务/提交/回滚）。
// 填充 out；去重命中时 out.duplicate=true 并直接返回。
// ============================================================================
static void sendSingleTxn(sql::Connection* conn,
                          std::uint64_t sender, std::uint64_t to, int type,
                          const std::string& content, std::uint64_t clientMsgId,
                          bool checkRecipient, bool offlineCursor,
                          StoredMessage& out) {
    // 0) 接收者必须存在（同时覆盖"非法用户"的错误处理）。
    //    关系与落库在同一事务内校验过时可以跳过：见函数头说明。
    if (checkRecipient) {
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
            return;         // 事务体结束，由 helper 的调用方负责提交
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

    // 5) 插入消息 + 6) 用同一条语句把服务端生成的 msg_id 和整行读回来（P0-6）。
    //    必须紧跟着 INSERT 执行：再往后的 INSERT IGNORE 不会改 LAST_INSERT_ID，
    //    但把回读放在这里最直观，也不会被后面的语句污染。
    std::uint64_t msgId = 0;
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
    {
        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
            "SELECT id,conversation_id,seq,sender_id,type,content,"
            "CAST(UNIX_TIMESTAMP(created_at)*1000 AS UNSIGNED) "
            "FROM messages WHERE id=LAST_INSERT_ID()"));
        std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
        if (!rs->next()) throw std::runtime_error("message inserted but not found");
        out = readStored(*rs);
        out.to = to;
        msgId = out.msg_id;
    }

    // 7) 离线队列。
    //    /*************P1-4：两种模式***************/
    //    行级队列模式（默认，OFFLINE_CURSOR=0）：无条件写一行，接收者 ACK 后删除。
    //      好处是多端各自确认、语义直观；代价是每条消息多一次 INSERT，
    //      在线用户还要在 ACK 时再来一次 DELETE（写放大 + 二级索引维护）。
    //    游标模式（OFFLINE_CURSOR=1）：什么都不写，登录时用
    //      "seq > conversation_members.last_delivered_seq" 直接算出未收消息。
    //      写入侧少一条 INSERT、ACK 侧少一条 DELETE，表也不再无限膨胀。
    //    代价：游标是"账号级"而不是"设备级"，多端登录时先 ACK 的那台会把
    //      另一台的未读也一起推进。需要多端各自未读时请保持行级队列模式。
    if (!offlineCursor) {
        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
            "INSERT IGNORE INTO offline_messages(user_id,message_id,conversation_id,seq) "
            "VALUES(?,?,?,?)"));
        st->setUInt64(1, to);
        st->setUInt64(2, msgId);
        st->setUInt64(3, conv);
        st->setUInt64(4, seq);
        st->executeUpdate();
    }
    /*************P1-4：两种模式***************/

    // 8) 发送者自己的游标推进，避免把自己的消息算成未读
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
}

// ============================================================================
// DAO 4：单聊发送（不做关系校验；保留给离线补推、后台脚本与回归测试）
// ----------------------------------------------------------------------------
// 热路径请走 DAO 4'（sendChecked）。本函数与原来行为一致，只是内部改用
// sendSingleTxn，并额外接入了 P0-2 的 SQL 队列背压反馈。
// ============================================================================
void Db::sendMessage(std::uint64_t sender, std::uint64_t to, int type,
                     const std::string& content, std::uint64_t clientMsgId,
                     SendCb cb) {
    const bool offlineCursor = rt_.offlineCursor;

    if (!submitSql(sqlQueue_, [this, sender, to, type, content, clientMsgId,
                               offlineCursor, cb]() {
        bool ok = false;
        std::string err;
        StoredMessage out;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案********************/
            // 事务级死锁重试（1213/1205/1020）仍然由 runTransactionWithRetry 兜底。
            runTransactionWithRetry(conn, [&]() {
                ok = false;
                sendSingleTxn(conn, sender, to, type, content, clientMsgId,
                              /*checkRecipient=*/true, offlineCursor, out);
                ok = true;
            });
            /*************避免数据库死锁的解决方案********************/
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out]() { cb(ok, err, out); });
    })) {
        postToIo([cb]() { cb(false, kBusyError, StoredMessage{}); });
    }
}

// ============================================================================
// DAO 4'：发消息热路径——关系校验 + 落库合并成一个任务（P1-2）
// ----------------------------------------------------------------------------
// /*************P1-1 + P1-2：为什么必须合起来***************/
// 原来 do_send 是 areFriends(job A) -> sendMessage(job B)：
//   * 两次排队、两次借还连接、两次 strand 切换，content/StoredMessage 各拷一遍；
//   * 注册风暴期间 B 排在 A 后面，等于把队头阻塞乘了二；
//   * A 与 B 之间还有 TOCTOU：校验完的瞬间对方可以把你删掉/拉黑。
// 现在一个任务里干完。关系判定分两条路径，**一致性强度不同，必须说清楚**：
//   * 缓存未命中（主路径）：SELECT 关系 + 落库在**同一个事务**里，
//     对方在这两条语句之间删好友/拉黑，事务会因为校验失败而整体回滚 ——
//     TOCTOU 窗口是真的被消掉了，不是"变窄"。
//   * 缓存命中（快路径）：不查库，因此存在最长 RELATION_CACHE_TTL 的
//     弱一致窗口。单进程部署下增删好友/拉黑都会显式失效，窗口几乎为 0；
//     如果业务要求"拉黑/删除后一条消息都不能再进"，把 TTL 设为 0 走主路径。
// /*************P1-1 + P1-2：为什么必须合起来***************/
//
// 缓存的一致性边界（重要，别在错误的部署形态下打开）：
//   * 单进程部署：增删好友/拉黑都在本进程的 Db 里显式失效，几乎无窗口；
//   * "读到旧值后才写回"的竞态由 relEpoch_ 版本号挡住（见 relationCachePut）；
//   * 多进程/多实例部署时本地缓存收不到彼此的失效通知，窗口最长等于 TTL ——
//     要跨实例强一致就得换成带广播失效的集中式缓存，届时再收紧这里的策略。
void Db::sendChecked(std::uint64_t sender, std::uint64_t to, int type,
                     const std::string& content, std::uint64_t clientMsgId,
                     SendCheckedCb cb) {
    const bool offlineCursor = rt_.offlineCursor;

    if (!submitSql(sqlQueue_, [this, sender, to, type, content, clientMsgId,
                               offlineCursor, cb]() {
        bool ok = false;
        std::string err;
        StoredMessage out;
        bool notFriends = false;
        bool blocked = false;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            // ---- 1) 关系判定 ----
            //   缓存命中：直接用，不查库（代价 = 最长 TTL 的弱一致窗口）；
            //   缓存未命中：**把关系 SELECT 放进落库事务里**，这样"校验 + 写入"是
            //   原子的——对方在这两条语句之间删好友/拉黑，事务会因为校验失败而回滚。
            //   （如果 SELECT 留在事务外，就仍然留着原来那个 TOCTOU 窗口，
            //     只是变窄；那 P1-2 的"消除"就没有兑现，注释也就成了误导。）
            int forward = -1;   // sender -> to 行的 status
            int reverse = -1;   // to -> sender 行的 status
            const std::uint64_t epoch = relationCacheEpoch();
            const bool cached = relationCacheGet(sender, to, forward, reverse);

            // 在事务内读关系并判定；不成立就抛 BizError 让整个事务回滚。
            auto checkRelationInTxn = [&]() {
                forward = -1;
                reverse = -1;
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT user_id, status FROM friends "
                    "WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)"));
                st->setUInt64(1, sender);
                st->setUInt64(2, to);
                st->setUInt64(3, to);
                st->setUInt64(4, sender);

                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) {
                    const std::uint64_t owner = rs->getUInt64(1);
                    const int status = rs->getInt(2);
                    if (owner == sender) forward = status; else reverse = status;
                }
                /****************双向 status=1 才是好友；任一 status=2 即被拉黑****************/
                if (!(forward == 1 && reverse == 1))
                    throw BizError((forward == 2 || reverse == 2)
                                       ? "blocked by recipient" : "not friends");
                /****************双向status=1才是好友；任一status=2即被拉黑****************/
            };

            if (cached) {
                const bool areFriends = (forward == 1 && reverse == 1);
                blocked = (forward == 2 || reverse == 2);
                if (!areFriends) {
                    notFriends = true;
                    err = blocked ? "blocked by recipient" : "not friends";
                } else {
                    // 接收者存在性由 friends 行的外键保证（关系成立 ⇒ 两个用户都存在）
                    runTransactionWithRetry(conn, [&]() {
                        ok = false;
                        sendSingleTxn(conn, sender, to, type, content, clientMsgId,
                                      /*checkRecipient=*/false, offlineCursor, out);
                        ok = true;
                    });
                }
            } else {
                try {
                    runTransactionWithRetry(conn, [&]() {
                        ok = false;
                        checkRelationInTxn();            // 事务内校验
                        sendSingleTxn(conn, sender, to, type, content, clientMsgId,
                                      /*checkRecipient=*/false, offlineCursor, out);
                        ok = true;
                    });
                    // 事务提交成功后才回填缓存；epoch 变化说明期间有过失效 → 自动放弃
                    relationCachePut(sender, to, forward, reverse, epoch);
                } catch (const BizError& e) {
                    notFriends = true;
                    err        = e.what();
                    blocked    = (err == "blocked by recipient");
                }
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out, notFriends, blocked]() {
            cb(ok, err, out, notFriends, blocked);
        });
    })) {
        postToIo([cb]() { cb(false, kBusyError, StoredMessage{}, false, false); });
    }
}

// ============================================================================
// 离线补推查询（DAO 3 与 fetchLoginSync 共用）
// ----------------------------------------------------------------------------
// /*************P1-4：两种数据源，返回列完全一致***************/
//   行级队列（OFFLINE_CURSOR=0，默认）：offline_messages 里一行就是一条待推消息，
//     客户端 ACK 后由 updateReceipt 删除；多端各自确认，语义最直观。
//   游标（OFFLINE_CURSOR=1）：不写任何队列表，直接用
//     messages.seq > conversation_members.last_delivered_seq 算出"这个账号还没
//     收到的消息"。写侧少一条 INSERT、ACK 侧少一条 DELETE，表也不再随消息量膨胀。
// 返回列（readStored 依赖前 7 列顺序）：
//   id, conversation_id, seq, sender_id, type, content, ts(ms), 是否群会话, 群ID
// /*************P1-4：两种数据源，返回列完全一致***************/
static std::string offlineFetchSql(bool cursor, int limit) {
    const std::string cols =
        "SELECT m.id,m.conversation_id,m.seq,m.sender_id,m.type,m.content,"
        "CAST(UNIX_TIMESTAMP(m.created_at)*1000 AS UNSIGNED), "
        "(c.type=2), COALESCE(g.id,0) ";

    if (cursor) {
        return cols +
            "FROM conversation_members cm "
            "JOIN conversations c ON c.id=cm.conversation_id "
            "JOIN messages m ON m.conversation_id=cm.conversation_id "
            "            AND m.seq > cm.last_delivered_seq "
            "LEFT JOIN `groups` g ON g.conversation_id=cm.conversation_id "
            "WHERE cm.user_id=? "
            "ORDER BY m.conversation_id, m.seq "
            "LIMIT " + std::to_string(limit);
    }
    return cols +
        "FROM offline_messages om "
        "JOIN messages m ON m.id=om.message_id "
        "JOIN conversations c ON c.id=m.conversation_id "
        "LEFT JOIN `groups` g ON g.conversation_id=m.conversation_id "
        "WHERE om.user_id=? "
        "ORDER BY om.conversation_id, om.seq "
        "LIMIT " + std::to_string(limit);
}

// 把一行离线消息读成 StoredMessage（列顺序见 offlineFetchSql）。
static StoredMessage readOfflineRow(sql::ResultSet& rs) {
    StoredMessage m = readStored(rs);
    m.is_group = (rs.getInt(8) != 0);
    if (m.is_group) m.to = rs.getUInt64(9);   // 群消息的 to 填 groupId
    return m;
}

// ============================================================================
// DAO 3：拉取离线消息
// ============================================================================
void Db::fetchOffline(std::uint64_t userId, int limit, MsgListCb cb) {
    const bool cursor = rt_.offlineCursor;
    if (!submitSql(sqlQueue_, [this, userId, limit, cursor, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<StoredMessage> msgs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            if (limit <= 0)   limit = 200;
            if (limit > 500)  limit = 500;

            std::unique_ptr<sql::PreparedStatement> st(
                pc->prepareStatement(offlineFetchSql(cursor, limit)));
            st->setUInt64(1, userId);

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) msgs.push_back(readOfflineRow(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, msgs = std::move(msgs)]() mutable {
            cb(ok, err, std::move(msgs));
        });
    })) {
        postToIo([cb]() { cb(false, kBusyError, std::vector<StoredMessage>{}); });
    }
}

// ============================================================================
// DAO 4：历史分页（keyset，按 seq 倒序取 N 条，再翻转成升序返回）
// ============================================================================
void Db::fetchHistory(std::uint64_t userId, std::uint64_t peerId,
                      std::uint64_t beforeSeq, int limit, MsgListCb cb) {
    if (!submitSql(sqlQueue_, [this, userId, peerId, beforeSeq, limit, cb]() mutable {
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
    })) {
        postToIo([cb]() {
            cb(false, kBusyError, std::vector<StoredMessage>{});
        });
    }
}

// ============================================================================
// DAO 5：投递 / 已读回执（行级队列模式下顺带删除已投递的离线消息）
// ============================================================================
void Db::updateReceipt(std::uint64_t userId, std::uint64_t conversationId,
                       std::uint64_t deliveredSeq, std::uint64_t readSeq,
                       StatusCb cb) {
    const bool cursor = rt_.offlineCursor;      // P1-4：游标模式跳过 DELETE
    if (!submitSql(sqlQueue_, [this, userId, conversationId, deliveredSeq, readSeq,
                               cursor, cb]() {
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
                /*************P1-4：游标模式下这条 DELETE 是纯浪费***************/
                // 行级队列模式：ACK 之后把已投递的离线行删掉，表才不会越长越大。
                // 游标模式：压根没写这些行，UPDATE 游标本身就是"确认"动作。
                if (!cursor) {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM offline_messages "
                        "WHERE user_id=? AND conversation_id=? AND seq<=?"));
                    st->setUInt64(1, userId);
                    st->setUInt64(2, conversationId);
                    st->setUInt64(3, deliveredSeq);
                    st->executeUpdate();
                }
                /*************P1-4：游标模式下这条 DELETE 是纯浪费***************/
            });
            /*************避免数据库死锁的解决方案********************/
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    })) {
        // 队列已满：丢掉一次 ACK 不致命。游标用的是 GREATEST()，
        // 客户端下一条 ACK 会带着更大的 seq 把这段补回来。
        postToIo([cb]() { cb(false, kBusyError); });
    }
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
    if (!submitSql(sqlQueue_, [this, requesterId, targetId, message, cb]() {
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
    })) {
        postToIo([cb]() { cb(false, kBusyError, FriendRequestInfo{}); });
    }
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
    if (!submitSql(sqlQueue_, [this, userId, requestId, accept, cb]() {
        bool ok = false;
        std::string err;
        FriendRequestInfo out;
        std::uint64_t requesterId = 0;   // 事务里取回，提交成功后用于失效关系缓存

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案********************/
            // P0-2：整个事务重试，遇到死锁(1213)/锁等待超时(1205)自动重来。
            // 这里的 SELECT ... FOR UPDATE 命中已存在的主键行，只加行锁不加 gap lock；
            // 真正需要兜底的是通过申请时 friends 双向写入可能产生的瞬时死锁。
            runTransactionWithRetry(conn, [&]() {
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

            /*************P1-1：通过申请 = 关系成立，立刻失效缓存***************/
            // 缓存 key 是 (小uid,大uid)，一条 entry 同时覆盖双向，所以这里一次
            // erase 就够。失效放在事务提交之后：回滚的路径不能改缓存。
            // 拒绝（accept=false）只动 friend_requests.status，不改 friends 行，
            // 因此无需失效。
            if (accept && requesterId != 0) relationCacheErase(userId, requesterId);
            /*************P1-1：通过申请 = 关系成立，立刻失效缓存***************/
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out]() { cb(ok, err, out); });
    })) {
        postToIo([cb]() { cb(false, kBusyError, FriendRequestInfo{}); });
    }
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
    if (!submitSql(sqlQueue_, [this, userId, cb]() {
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
    })) {
        postToIo([cb]() {
            cb(false, kBusyError, std::vector<FriendInfo>{});
        });
    }
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
    if (!submitSql(sqlQueue_, [this, userId, friendId, cb]() {
        bool ok = false;
        std::string err;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            sql::Connection* conn = pc.get();

            /*************避免数据库死锁的解决方案（第三版：固定加锁顺序）***************/
            // P0-2 的事务重试是**兜底**，不是目标：每次 1213/1205 都要回滚整个事务、
            // 再在 worker 线程里 sleep 退避，压测阶段4（100 个用户同时删同一个 mainUid）
            // 就是把这条路径压满的场景。
            //
            // 原来每条语句写成 `WHERE (a=? AND b=?) OR (c=? AND d=?)`：一次语句要锁
            // **两个不相邻的主键行**，优化器给的访问顺序不受我们控制，于是
            // "A 删 B" 和 "B 删 A" 这两个并发事务很可能以相反顺序抢同两行 → 1213。
            // 现在拆成两条**主键定位的单行** DELETE，并且显式按 (user_id, friend_id)
            // 升序执行；friend_requests 同理按 (requester_id, target_id) 升序。
            // 全局所有事务都用同一个顺序，循环等待就不成立，重试退化成真正的偶发。
            //
            // 语义完全不变：两张表的双向行都删干净（userId==friendId 时两条语句
            // 命中同一行，第二条自然是 0 行，幂等）。
            auto deletePair = [&](const char* table,
                                  const char* colA, const char* colB,
                                  std::uint64_t x, std::uint64_t y) {
                // 先删"键值较小"的那一行，保证任意两个并发事务的加锁顺序一致
                const std::uint64_t firstA  = std::min(x, y), firstB  = std::max(x, y);
                const std::uint64_t scndA   = std::max(x, y), scndB   = std::min(x, y);

                for (int pass = 0; pass < 2; ++pass) {
                    const std::uint64_t a = (pass == 0) ? firstA : scndA;
                    const std::uint64_t b = (pass == 0) ? firstB : scndB;
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        std::string("DELETE FROM ") + table +
                        " WHERE " + colA + "=? AND " + colB + "=?"));
                    st->setUInt64(1, a);
                    st->setUInt64(2, b);
                    st->executeUpdate();
                }
            };

            runTransactionWithRetry(conn, [&]() {
                deletePair("friends", "user_id", "friend_id", userId, friendId);
                deletePair("friend_requests", "requester_id", "target_id", userId, friendId);
            });
            /*************避免数据库死锁的解决方案（第三版：固定加锁顺序）***************/
            ok = true;
            /*************P1-1：好友关系已解除，失效缓存（双向一个 key）***************/
            relationCacheErase(userId, friendId);
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    })) {
        postToIo([cb]() { cb(false, kBusyError); });
    }
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
    if (!submitSql(sqlQueue_, [this, userId, friendId, blocked, cb]() {
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
            /*************P1-1：拉黑状态变化同样是关系变化***************/
            relationCacheErase(userId, friendId);
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err]() { cb(ok, err); });
    })) {
        postToIo([cb]() { cb(false, kBusyError); });
    }
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

// ============================================================================
// /*************群聊功能：群 DAO 辅助函数与实现（BE-GROUP-01~08）***************/
// ============================================================================
namespace {

// ---- 群资料的标准 SELECT ---------------------------------------------------
// 列顺序（readGroupFull 依赖）：
//   1 id, 2 conversation_id, 3 name, 4 announcement, 5 intro, 6 avatar_url,
//   7 owner_id, 8 status, 9 member_limit, 10 member_count, 11 my_role, 12 created_ts
// member_count 用相关子查询：MVP 群规模小（<=200），COUNT(*) 走
// idx_group_members_group_role 前缀，代价可接受；上规模后可冗余计数列。
// my_role 不在表里，由调用方按“查询者是否是成员”以字面量或 JOIN 提供。
const char* kGroupSelectNoRole =
    "SELECT g.id,g.conversation_id,g.name,g.announcement,g.intro,g.avatar_url,"
    "g.owner_id,g.status,g.member_limit,"
    " (SELECT COUNT(*) FROM group_members m2 WHERE m2.group_id=g.id),"
    " 0,"
    " CAST(UNIX_TIMESTAMP(g.created_at)*1000 AS UNSIGNED)"
    " FROM `groups` g ";

// my_role 不在表里：loadGroup 查询者无关（填 0），loadGroupAsMember 里 JOIN 出来。

// 成员列表 SELECT 前缀。列顺序（readGroupMember 依赖）：
//   1 user_id, 2 username, 3 nickname, 4 avatar_url, 5 group_nickname,
//   6 role, 7 joined_ts
const char* kGroupMemberSelect =
    "SELECT gm.user_id,u.username,u.nickname,u.avatar_url,gm.group_nickname,gm.role,"
    "CAST(UNIX_TIMESTAMP(gm.joined_at)*1000 AS UNSIGNED) "
    "FROM group_members gm JOIN users u ON u.id=gm.user_id ";

// 入群申请 SELECT 前缀。列顺序（readGroupRequest 依赖）：
//   1 id, 2 group_id, 3 group_name, 4 requester_id, 5 requester_username,
//   6 requester_nickname, 7 handler_id, 8 message, 9 status, 10 created_ts,
//   11 owner_id（群主，新申请实时推送用）
const char* kGroupRequestSelect =
    "SELECT gr.id,gr.group_id,g.name,gr.requester_id,ru.username,ru.nickname,"
    "gr.handler_id,gr.message,gr.status,"
    "CAST(UNIX_TIMESTAMP(gr.created_at)*1000 AS UNSIGNED), g.owner_id "
    "FROM group_requests gr JOIN `groups` g ON g.id=gr.group_id "
    "LEFT JOIN users ru ON ru.id=gr.requester_id ";

// groups.type=2 会话的固定值。
constexpr int kConvTypeGroup = 2;

// SQL 参数占位符拼接小工具：生成 IN (?,?) 的括号部分。
std::string inPlaceholders(int n) {
    std::string s = "(";
    for (int i = 0; i < n; ++i) { if (i) s += ","; s += "?"; }
    s += ")";
    return s;
}

// 读取“群资料”行（列顺序见 kGroupSelectNoRole）。
GroupInfo readGroupFull(sql::ResultSet& rs) {
    GroupInfo g;
    g.id              = rs.getUInt64(1);
    g.conversation_id = rs.getUInt64(2);
    g.name            = sstr(rs.getString(3));
    g.announcement    = sstr(rs.getString(4));
    g.intro           = sstr(rs.getString(5));
    g.avatar_url      = sstr(rs.getString(6));
    g.owner_id        = rs.getUInt64(7);
    g.status          = rs.getInt(8);
    g.member_limit    = rs.getInt(9);
    g.member_count    = rs.getInt(10);
    g.my_role         = rs.getInt(11);
    g.created_ts      = rs.getUInt64(12);
    return g;
}

GroupMemberInfo readGroupMember(sql::ResultSet& rs) {
    GroupMemberInfo m;
    m.user_id        = rs.getUInt64(1);
    m.username       = sstr(rs.getString(2));
    m.nickname       = sstr(rs.getString(3));
    m.avatar_url     = sstr(rs.getString(4));
    m.group_nickname = sstr(rs.getString(5));
    m.role           = rs.getInt(6);
    m.joined_ts      = rs.getUInt64(7);
    return m;
}

GroupRequestInfo readGroupRequest(sql::ResultSet& rs) {
    GroupRequestInfo r;
    r.id                 = rs.getUInt64(1);
    r.group_id           = rs.getUInt64(2);
    r.group_name         = sstr(rs.getString(3));
    r.requester_id       = rs.getUInt64(4);
    r.requester_username = sstr(rs.getString(5));
    r.requester_nickname = sstr(rs.getString(6));
    r.handler_id         = rs.getUInt64(7);
    r.message            = sstr(rs.getString(8));
    r.status             = rs.getInt(9);
    r.created_ts         = rs.getUInt64(10);
    r.owner_id           = rs.getUInt64(11);   // /*************群聊功能：新申请实时推给群主***************/
    return r;
}

// 查群资料（不校验查询者身份，my_role=0）。找不到抛 BizError。
GroupInfo loadGroup(sql::Connection* conn, std::uint64_t groupId) {
    std::string sql = std::string(kGroupSelectNoRole) + "WHERE g.id=?";
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
    st->setUInt64(1, groupId);
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    if (!rs->next()) throw BizError("group not found");
    return readGroupFull(*rs);
}

// 查群资料（带查询者角色，同时完成“必须是成员”的校验）。
// JOIN group_members gm 使“非成员”直接查不到行 -> 抛 BizError。
GroupInfo loadGroupAsMember(sql::Connection* conn, std::uint64_t groupId,
                            std::uint64_t userId) {
    const std::string sql =
        "SELECT g.id,g.conversation_id,g.name,g.announcement,g.intro,g.avatar_url,"
        "g.owner_id,g.status,g.member_limit,"
        " (SELECT COUNT(*) FROM group_members m2 WHERE m2.group_id=g.id),"
        " gm.role,"
        " CAST(UNIX_TIMESTAMP(g.created_at)*1000 AS UNSIGNED)"
        " FROM `groups` g JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
        "WHERE g.id=?";
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
    st->setUInt64(1, userId);
    st->setUInt64(2, groupId);
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    if (!rs->next()) throw BizError("not a group member");
    return readGroupFull(*rs);
}

// 查成员列表。
std::vector<GroupMemberInfo> loadGroupMembers(sql::Connection* conn,
                                              std::uint64_t groupId) {
    std::string sql = std::string(kGroupMemberSelect) +
        "WHERE gm.group_id=? ORDER BY gm.role ASC, gm.user_id ASC";
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
    st->setUInt64(1, groupId);
    std::vector<GroupMemberInfo> out;
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    while (rs->next()) out.push_back(readGroupMember(*rs));
    return out;
}

// 查单个成员的角色；不存在返回 0。
int groupMemberRole(sql::Connection* conn, std::uint64_t groupId,
                    std::uint64_t userId, bool forUpdate) {
    std::string sql = "SELECT role FROM group_members WHERE group_id=? AND user_id=?";
    if (forUpdate) sql += " FOR UPDATE";
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
    st->setUInt64(1, groupId);
    st->setUInt64(2, userId);
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    if (!rs->next()) return 0;
    return rs->getInt(1);
}

// 群新成员加入后：为“全部群成员”补建 conversation_members 游标行。
// 游标初始化为当前 last_seq：新成员不把历史消息算成未读/离线。
// INSERT IGNORE 保证重复调用幂等。
void backfillGroupConvMembers(sql::Connection* conn, std::uint64_t groupId,
                              std::uint64_t convId) {
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
        "INSERT IGNORE INTO conversation_members"
        "(conversation_id,user_id,last_delivered_seq,last_read_seq) "
        "SELECT c.id,gm.user_id,c.last_seq,c.last_seq "
        "FROM conversations c JOIN group_members gm ON gm.group_id=? "
        "WHERE c.id=?"));
    st->setUInt64(1, groupId);
    st->setUInt64(2, convId);
    st->executeUpdate();
}

// 批量插入群成员。注意：group_members 有 3 列，但 role 对新邀请/新加入者
// 固定为 3=普通成员，直接写进 SQL 字面量，每行只绑 2 个参数。
void insertGroupMembers(sql::Connection* conn, std::uint64_t groupId,
                        const std::vector<std::uint64_t>& userIds) {
    if (userIds.empty()) return;
    std::string sql = "INSERT IGNORE INTO group_members(group_id,user_id,role) VALUES";
    for (std::size_t i = 0; i < userIds.size(); ++i) {
        if (i) sql += ",";
        sql += "(?,?,3)";
    }
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
    int idx = 1;
    for (auto uid : userIds) {
        st->setUInt64(idx++, groupId);
        st->setUInt64(idx++, uid);
    }
    st->executeUpdate();
}

// 一条入群申请的完整信息读取（含群名/申请人资料）。
GroupRequestInfo loadGroupRequest(sql::Connection* conn, std::uint64_t requestId) {
    std::string sql = std::string(kGroupRequestSelect) + "WHERE gr.id=?";
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
    st->setUInt64(1, requestId);
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    if (!rs->next()) throw BizError("group request not found");
    return readGroupRequest(*rs);
}

// 校验一批 userId 都是“正常状态的用户”。
void ensureActiveUsers(sql::Connection* conn,
                       const std::vector<std::uint64_t>& ids) {
    if (ids.empty()) return;
    std::string sql = "SELECT COUNT(*) FROM users WHERE status=1 AND id IN" +
        inPlaceholders(static_cast<int>(ids.size()));
    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
    for (std::size_t i = 0; i < ids.size(); ++i)
        st->setUInt64(static_cast<int>(i) + 1, ids[i]);
    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
    rs->next();
    if (static_cast<std::size_t>(rs->getUInt64(1)) != ids.size())
        throw BizError("some member user not found");
}

// 群资料写操作成功后统一组装返回值：group（带 actor 视角角色）+ members。
void fillGroupOpResult(sql::Connection* conn, std::uint64_t groupId,
                       std::uint64_t actor, GroupOpResult& res) {
    try {
        res.group = loadGroupAsMember(conn, groupId, actor);
    } catch (const BizError&) {
        // actor 可能已不是成员（例如刚退群/解散）：退回“无视角”加载。
        res.group = loadGroup(conn, groupId);
    }
    res.members = loadGroupMembers(conn, groupId);
    res.group.member_count = static_cast<int>(res.members.size());
}

} // namespace

// ============================================================================
// DAO 6'：登录补推一次性取回（P1-3）
// ----------------------------------------------------------------------------
// 原来一次 TCP 登录要发 5~7 个独立 DB 任务（离线消息、待处理申请、申请结果、
// 标记已通知、入群待审批、入群结果、标记已通知），每个任务都要排队 + 借还连接。
// 重连风暴时"每次登录 7 个任务"把 SQL 队列长度直接乘 7，这正是压测里
// "登录之后所有接口一起变慢"的放大器。本函数在一个任务、一条连接上取完。
//
// 刻意保留的两点原有语义：
//   1) 这里不做"标记已通知"的 UPDATE。原实现是推送成功之后才标记；
//      如果改成取的时候就标记，进程崩溃或连接中途断开就会丢掉事件 ——
//      宁可重复推送也不能丢。所以仍由 session 在推完后按需调用
//      markResultsNotified / markGroupResultsNotified（列表为空时一次都不发）。
//   2) 离线消息多取一条用来判断 offlineMore，让客户端按需续拉，
//      而不是像原来一次 200 条：100 个用户同时登录 × 200 条 ≈ 5MB，
//      在 3Mbps 链路上要传 13 秒，登录风暴会顺带把带宽打满。
// SQL 与原来的 fetchOffline / fetchPendingIncoming / fetchUnnotifiedResults /
// fetchPendingGroupRequestsForApprover / fetchUnnotifiedGroupResults 逐字一致，
// 只是挪到同一个任务里顺序执行，排序与限流语义不变。
// ============================================================================
void Db::fetchLoginSync(std::uint64_t userId, int offlineLimit, LoginSyncCb cb) {
    const bool cursor = rt_.offlineCursor;
    if (!submitSql(sqlQueue_, [this, userId, offlineLimit, cursor, cb]() {
        bool ok = false;
        std::string err;
        LoginSync out;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            // ---- 1) 离线消息：多取一条用于判断"还有更多" ----
            int limit = offlineLimit > 0 ? offlineLimit : 50;
            if (limit > 200) limit = 200;
            {
                std::unique_ptr<sql::PreparedStatement> st(
                    conn->prepareStatement(offlineFetchSql(cursor, limit + 1)));
                st->setUInt64(1, userId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) {
                    if (static_cast<int>(out.offline.size()) >= limit) {
                        out.offlineMore = true;      // 丢弃这多取的一条，下次登录/翻页再拿
                        break;
                    }
                    out.offline.push_back(readOfflineRow(*rs));
                }
            }

            // ---- 2) 待我处理的好友申请 ----
            {
                const std::string sql = std::string(kRequestSelect) +
                    "WHERE fr.target_id=? AND fr.status=0 "
                    "ORDER BY fr.created_at ASC LIMIT 100";
                std::unique_ptr<sql::PreparedStatement> st(
                    conn->prepareStatement(sql));
                st->setUInt64(1, userId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next())
                    out.pendingRequests.push_back(readFriendRequestInfo(*rs));
            }

            // ---- 3) 我发出的、已处理但还没通知我的申请结果 ----
            {
                const std::string sql = std::string(kRequestSelect) +
                    "WHERE fr.requester_id=? AND fr.status IN (1,2) "
                    "  AND fr.result_notified=0 "
                    "ORDER BY fr.updated_at ASC LIMIT 100";
                std::unique_ptr<sql::PreparedStatement> st(
                    conn->prepareStatement(sql));
                st->setUInt64(1, userId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next())
                    out.requestResults.push_back(readFriendRequestInfo(*rs));
            }

            // ---- 4) 群事件：先花一条极小的查询判断"要不要查群" ----
            //  绝大多数用户不在任何群里，原来那两次群查询对他们是纯浪费。
            bool inSomeGroup = false;
            {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT 1 FROM group_members WHERE user_id=? LIMIT 1"));
                st->setUInt64(1, userId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                inSomeGroup = rs->next();
            }

            if (inSomeGroup) {
                // 4a) 待我审批的入群申请（我是群主/管理员的群的 pending）
                {
                    const std::string sql = std::string(kGroupRequestSelect) +
                        "JOIN group_members gm ON gm.group_id=gr.group_id "
                        "AND gm.user_id=? AND gm.role<=2 "
                        "WHERE gr.status=0 AND g.status=0 "
                        "ORDER BY gr.created_at ASC LIMIT 100";
                    std::unique_ptr<sql::PreparedStatement> st(
                        conn->prepareStatement(sql));
                    st->setUInt64(1, userId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    while (rs->next())
                        out.pendingGroupRequests.push_back(readGroupRequest(*rs));
                }
                // 4b) 我发出的、已处理但还没通知我的入群结果
                {
                    const std::string sql = std::string(kGroupRequestSelect) +
                        "WHERE gr.requester_id=? AND gr.status IN (1,2) "
                        "AND gr.result_notified=0 "
                        "ORDER BY gr.updated_at ASC LIMIT 100";
                    std::unique_ptr<sql::PreparedStatement> st(
                        conn->prepareStatement(sql));
                    st->setUInt64(1, userId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    while (rs->next())
                        out.groupResults.push_back(readGroupRequest(*rs));
                }
            }

            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out = std::move(out)]() mutable {
            cb(ok, err, std::move(out));
        });
    })) {
        postToIo([cb]() { cb(false, kBusyError, LoginSync{}); });
    }
}

// ============================================================================
// 批量写好友关系的两个 DAO（P1-7）
// ----------------------------------------------------------------------------
// 为什么"批量"是性能项而不是便利性项：
//   给 N 个人发申请这件事，用单条接口 = N 个事务、N 次连接借还，而且 N 个事务
//   都在同一个 requester 的索引区间上抢锁（fixme.md 分析过的 gap lock 风暴
//   就是这个形状）。压测里"100 个好友申请"阶段 p50 已经到 1 秒，就是这个原因。
//   批量把它压成 1 个事务 + 一次多行语句，并把加锁顺序固定为按目标 id 升序，
//   并发批量操作之间不会因为交错加锁而死锁。
//
// 与单条接口的语义差异（重要）：
//   单条接口是"要么成功要么报一个错"；批量接口是"尽力而为"：
//     · 满足条件的目标 → 建立/重置申请，进 created；
//     · 不满足条件的目标 → 进 skipped，附带原因（与单条接口的错误文案一致）。
//   这样调用方不必因为一个好友已经是好友而整批失败。
// ============================================================================
namespace {

// 生成 n 个 "?,?,?" 占位符（用于 IN (...) 列表）。
std::string inPlaceholders(std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (i) s += ",";
        s += "?";
    }
    return s;
}

} // namespace

void Db::createFriendRequestsBatch(std::uint64_t requesterId,
                                   std::vector<std::uint64_t> targetIds,
                                   const std::string& message,
                                   RequestBatchCb cb) {
    // 空列表不必打扰数据库（IN () 是非法 SQL），直接回成功 + 空结果。
    if (targetIds.empty()) {
        postToIo([cb]() {
            cb(true, std::string(), std::vector<FriendRequestInfo>{},
               std::vector<std::pair<std::uint64_t, std::string>>{});
        });
        return;
    }

    // 去重 + 升序：升序既是"固定加锁顺序"，也让返回顺序稳定可预期。
    std::sort(targetIds.begin(), targetIds.end());
    targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());

    if (!submitSql(sqlQueue_, [this, requesterId, targetIds, message, cb]() {
        bool ok = false;
        std::string err;
        std::vector<FriendRequestInfo> created;
        std::vector<std::pair<std::uint64_t, std::string>> skipped;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            const std::string ph = inPlaceholders(targetIds.size());

            // ---- 1) 目标存在性 ----
            std::vector<std::uint64_t> existing;
            {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT id FROM users WHERE status=1 AND id IN (" + ph + ")"));
                for (std::size_t i = 0; i < targetIds.size(); ++i)
                    st->setUInt64(i + 1, targetIds[i]);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) existing.push_back(rs->getUInt64(1));
            }
            std::unordered_set<std::uint64_t> existSet(existing.begin(), existing.end());

            // ---- 2) 现有关系（双向）----
            // myView[target]   = 我对他的行 status（user_id=我, friend_id=他）
            // theirView[target]= 他对我的行 status（user_id=他, friend_id=我）
            std::unordered_map<std::uint64_t, int> myView, theirView;
            {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT user_id, friend_id, status FROM friends "
                    "WHERE (user_id=? AND friend_id IN (" + ph + ")) "
                    "   OR (user_id IN (" + ph + ") AND friend_id=?)"));
                std::size_t p = 1;
                st->setUInt64(p++, requesterId);
                for (auto t : targetIds) st->setUInt64(p++, t);
                for (auto t : targetIds) st->setUInt64(p++, t);
                st->setUInt64(p++, requesterId);

                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) {
                    const std::uint64_t owner  = rs->getUInt64(1);
                    const std::uint64_t other  = rs->getUInt64(2);
                    const int status           = rs->getInt(3);
                    if (owner == requesterId) myView[other]    = status;
                    else                      theirView[other] = status;
                }
            }

            // ---- 3) 已有申请（同向 + 反向 pending）----
            std::unordered_map<std::uint64_t, int> sameDir;   // target -> 我的申请状态
            std::unordered_set<std::uint64_t> reversePending; // 对方已向我发了 pending
            {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT target_id, status FROM friend_requests "
                    "WHERE requester_id=? AND target_id IN (" + ph + ")"));
                std::size_t p = 1;
                st->setUInt64(p++, requesterId);
                for (auto t : targetIds) st->setUInt64(p++, t);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) sameDir[rs->getUInt64(1)] = rs->getInt(2);
            }
            {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT requester_id FROM friend_requests "
                    "WHERE target_id=? AND status=0 AND requester_id IN (" + ph + ")"));
                std::size_t p = 1;
                st->setUInt64(p++, requesterId);
                for (auto t : targetIds) st->setUInt64(p++, t);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) reversePending.insert(rs->getUInt64(1));
            }

            // ---- 4) 分流：新建 / 重置 / 跳过 ----
            std::vector<std::uint64_t> toInsert, toReset;
            for (auto t : targetIds) {
                if (t == requesterId) { skipped.emplace_back(t, "cannot add yourself"); continue; }
                if (!existSet.count(t)) { skipped.emplace_back(t, "target user not found"); continue; }
                const int mine  = myView.count(t) ? myView[t] : -1;
                const int other = theirView.count(t) ? theirView[t] : -1;
                if (mine == 1 || other == 1) { skipped.emplace_back(t, "already friends"); continue; }
                if (other == 2) { skipped.emplace_back(t, "blocked by target user"); continue; }
                if (reversePending.count(t)) {
                    skipped.emplace_back(t, "target already sent you a pending request");
                    continue;
                }
                const int cur = sameDir.count(t) ? sameDir[t] : -1;
                if (cur == 0) { skipped.emplace_back(t, "request already pending"); continue; }
                if (cur == 1) { skipped.emplace_back(t, "already friends"); continue; }
                if (cur == 2 || cur == 3) { toReset.push_back(t); continue; }  // 被拒/取消后允许重申
                toInsert.push_back(t);
            }

            // ---- 5) 写入：一次多行 INSERT + 一次批量 UPDATE ----
            if (!toInsert.empty() || !toReset.empty()) {
                runTransactionWithRetry(conn, [&]() {
                    if (!toInsert.empty()) {
                        std::string sql =
                            "INSERT IGNORE INTO friend_requests"
                            "(requester_id,target_id,message,status,result_notified) VALUES";
                        for (std::size_t i = 0; i < toInsert.size(); ++i) {
                            sql += i ? ",(?,?,?,0,0)" : "(?,?,?,0,0)";
                        }
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                        std::size_t p = 1;
                        for (auto t : toInsert) {
                            st->setUInt64(p++, requesterId);
                            st->setUInt64(p++, t);
                            st->setString(p++, message);
                        }
                        st->executeUpdate();
                    }
                    if (!toReset.empty()) {
                        const std::string ph2 = inPlaceholders(toReset.size());
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "UPDATE friend_requests "
                            "SET message=?, status=0, result_notified=0, "
                            "    created_at=CURRENT_TIMESTAMP(3) "
                            "WHERE requester_id=? AND status IN (2,3) "
                            "  AND target_id IN (" + ph2 + ")"));
                        std::size_t p = 1;
                        st->setString(p++, message);
                        st->setUInt64(p++, requesterId);
                        for (auto t : toReset) st->setUInt64(p++, t);
                        st->executeUpdate();
                    }
                });
            }

            // ---- 6) 读回本次真正处于 pending 的申请（含双方用户名）----
            std::vector<std::uint64_t> fresh;
            fresh.insert(fresh.end(), toInsert.begin(), toInsert.end());
            fresh.insert(fresh.end(), toReset.begin(), toReset.end());
            if (!fresh.empty()) {
                std::sort(fresh.begin(), fresh.end());
                const std::string ph3 = inPlaceholders(fresh.size());
                const std::string sql = std::string(kRequestSelect) +
                    "WHERE fr.requester_id=? AND fr.status=0 "
                    "  AND fr.target_id IN (" + ph3 + ") "
                    "ORDER BY fr.id ASC";
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                std::size_t p = 1;
                st->setUInt64(p++, requesterId);
                for (auto t : fresh) st->setUInt64(p++, t);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) created.push_back(readFriendRequestInfo(*rs));

                /*************INSERT IGNORE / UPDATE 守卫都可能"什么都没做"***************/
                // 并发场景：同一个 (requester,target) 对，另一路请求（比如单条接口）
                // 在我们 SELECT 之后、INSERT 之前已经把行建好了 → INSERT IGNORE 静默跳过；
                // 或者某个 toReset 目标在申请被并发通过后才执行 UPDATE（status IN (2,3)
                // 守卫导致 0 行更新）。这两种情况下该目标既不在 created、也不在 skipped，
                // 客户端就会以为"这个目标凭空消失了"。
                // 所以把差集单独查一次真实状态，按状态补进 skipped，保证
                //   created ∪ skipped == 请求里的全部目标（去重后）。
                std::unordered_set<std::uint64_t> doneTargets;
                for (const auto& r : created) doneTargets.insert(r.target_id);

                std::vector<std::uint64_t> missing;
                for (auto t : fresh)
                    if (!doneTargets.count(t)) missing.push_back(t);

                if (!missing.empty()) {
                    const std::string ph4 = inPlaceholders(missing.size());
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT target_id, status FROM friend_requests "
                        "WHERE requester_id=? AND target_id IN (" + ph4 + ")"));
                    std::size_t p = 1;
                    st->setUInt64(p++, requesterId);
                    for (auto t : missing) st->setUInt64(p++, t);

                    std::unordered_map<std::uint64_t, int> nowStatus;
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    while (rs->next()) nowStatus[rs->getUInt64(1)] = rs->getInt(2);

                    for (auto t : missing) {
                        const auto it = nowStatus.find(t);
                        const char* reason =
                            (it == nowStatus.end()) ? "request state changed concurrently"
                            : (it->second == 0) ? "request already pending"
                            : (it->second == 1) ? "already friends"
                            : "request state changed concurrently";
                        skipped.emplace_back(t, reason);
                    }
                }
                /*************INSERT IGNORE / UPDATE 守卫都可能"什么都没做"***************/
            }

            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, created = std::move(created),
                  skipped = std::move(skipped)]() mutable {
            cb(ok, err, std::move(created), std::move(skipped));
        });
    })) {
        postToIo([cb]() {
            cb(false, kBusyError, std::vector<FriendRequestInfo>{},
               std::vector<std::pair<std::uint64_t, std::string>>{});
        });
    }
}

void Db::deleteFriendsBatch(std::uint64_t userId,
                            std::vector<std::uint64_t> friendIds,
                            RemovedFriendsCb cb) {
    std::sort(friendIds.begin(), friendIds.end());
    friendIds.erase(std::unique(friendIds.begin(), friendIds.end()), friendIds.end());

    if (!submitSql(sqlQueue_, [this, userId, friendIds, cb]() {
        bool ok = false;
        std::string err;
        std::vector<std::uint64_t> removed;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            if (!friendIds.empty()) {
                const std::string ph = inPlaceholders(friendIds.size());

                /*************为什么要先查一次"哪些确实是我的好友"***************/
                // DELETE 的 affected rows 是**双向两行**，拿它除以 2 当"删了几个人"
                // 是错的：单向拉黑行（status=2）只有一行、已经不存在的目标会 0 行。
                // 所以要的语义是"从我的好友列表里消失了多少人"，那就直接读
                // user_id=我 AND status=1 的行，删完再把这个集合原样交给上层，
                // 由 HTTP 层逐个推送 FriendListChanged（与单条删除的行为对齐）。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT friend_id FROM friends "
                        "WHERE user_id=? AND status=1 AND friend_id IN (" + ph + ")"));
                    std::size_t p = 1;
                    st->setUInt64(p++, userId);
                    for (auto f : friendIds) st->setUInt64(p++, f);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    while (rs->next()) removed.push_back(rs->getUInt64(1));
                }

                runTransactionWithRetry(conn, [&]() {
                    {
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "DELETE FROM friends "
                            "WHERE (user_id=? AND friend_id IN (" + ph + ")) "
                            "   OR (user_id IN (" + ph + ") AND friend_id=?)"));
                        std::size_t p = 1;
                        st->setUInt64(p++, userId);
                        for (auto f : friendIds) st->setUInt64(p++, f);
                        for (auto f : friendIds) st->setUInt64(p++, f);
                        st->setUInt64(p++, userId);
                        st->executeUpdate();
                    }
                    {
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "DELETE FROM friend_requests "
                            "WHERE (requester_id=? AND target_id IN (" + ph + ")) "
                            "   OR (requester_id IN (" + ph + ") AND target_id=?)"));
                        std::size_t p = 1;
                        st->setUInt64(p++, userId);
                        for (auto f : friendIds) st->setUInt64(p++, f);
                        for (auto f : friendIds) st->setUInt64(p++, f);
                        st->setUInt64(p++, userId);
                        st->executeUpdate();
                    }
                });
                // P1-1：批量解除了关系，逐条失效缓存。
                // 这里按"请求的全部 id"失效而不是只按 removed：拉黑行（status=2）
                // 同样会被这条 DELETE 清掉，但它不在 removed 里，
                // 只失效 removed 会让缓存里留下一条"还是好友"的旧记录。
                for (auto f : friendIds) relationCacheErase(userId, f);
            }
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, removed = std::move(removed)]() mutable {
            cb(ok, err, std::move(removed));
        });
    })) {
        postToIo([cb]() { cb(false, kBusyError, std::vector<std::uint64_t>{}); });
    }
}
// ============================================================================
// 群 DAO 1：建群（BE-GROUP-02）
// ----------------------------------------------------------------------------
// 事务顺序（任何一步失败整体回滚，不会留下“有群没会话/有会话没群”的脏数据）：
//   1) 参数与成员校验（初始成员必须是发起人双向好友、账号正常）；
//   2) INSERT conversations(type=2)  -> 群会话（seq 生成器在这里）；
//   3) INSERT `groups`               -> 群资料，回填 conversation_id；
//   4) INSERT group_members          -> 发起人 role=1，初始成员 role=3；
//   5) backfill conversation_members -> 全员游标行（初始化为 last_seq=0）。
// /*************群聊功能：建群***************/
void Db::createGroup(std::uint64_t owner, const std::string& name,
                     const std::vector<std::uint64_t>& memberIds, GroupOpCb cb) {
    submit([this, owner, name, memberIds, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            // ---- 参数校验（纯内存，先做，省一次 DB 往返） ------------------
            if (name.empty() || name.size() > 128)
                throw BizError("invalid group name");
            std::vector<std::uint64_t> ms = memberIds;
            ms.erase(std::remove(ms.begin(), ms.end(), owner), ms.end());
            std::sort(ms.begin(), ms.end());
            ms.erase(std::unique(ms.begin(), ms.end()), ms.end());
            if (ms.empty())
                throw BizError("at least one initial member required");
            if (ms.size() + 1 > 200)
                throw BizError("too many initial members");

            /*************避免数据库死锁的解决方案********************/
            // 群事务统一交给 runTransactionWithRetry 兜底（1213/1205 自动重试）。
            runTransactionWithRetry(conn, [&]() {
                // 初始成员必须是正常用户。
                ensureActiveUsers(conn, ms);

                // MVP 约束：初始成员必须是发起人的好友（BE-GROUP-02）。
                {
                    std::string sql =
                        "SELECT COUNT(*) FROM friends f JOIN users u "
                        "ON u.id=f.friend_id AND u.status=1 "
                        "WHERE f.user_id=? AND f.status=1 AND f.friend_id IN" +
                        inPlaceholders(static_cast<int>(ms.size()));
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                    st->setUInt64(1, owner);
                    for (std::size_t i = 0; i < ms.size(); ++i)
                        st->setUInt64(static_cast<int>(i) + 2, ms[i]);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    rs->next();
                    if (static_cast<std::size_t>(rs->getUInt64(1)) != ms.size())
                        throw BizError("initial members must be your friends");
                }

                // 2) 群会话：type=2、owner=发起人；dedup_key 保持 NULL（群聊不适用）。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT INTO conversations(type,title,owner_id) VALUES(?,?,?)"));
                    st->setInt(1, kConvTypeGroup);
                    st->setString(2, name);
                    st->setUInt64(3, owner);
                    st->executeUpdate();
                }
                const std::uint64_t conv = lastInsertId(conn);

                // 3) 群资料。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT INTO `groups`(conversation_id,name,owner_id) VALUES(?,?,?)"));
                    st->setUInt64(1, conv);
                    st->setString(2, name);
                    st->setUInt64(3, owner);
                    st->executeUpdate();
                }
                const std::uint64_t groupId = lastInsertId(conn);

                // 4) 发起人 role=1（群主），初始成员 role=3。
                {
                    std::string sql =
                        "INSERT INTO group_members(group_id,user_id,role) VALUES(?,?,1)" +
                        std::string(ms.empty() ? "" : "," +
                            [&]{
                                std::string s;
                                for (std::size_t i = 0; i < ms.size(); ++i) {
                                    if (i) s += ",";
                                    s += "(?,?,3)";
                                }
                                return s;
                            }());
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                    int idx = 1;
                    st->setUInt64(idx++, groupId);
                    st->setUInt64(idx++, owner);
                    for (auto uid : ms) {
                        st->setUInt64(idx++, groupId);
                        st->setUInt64(idx++, uid);
                    }
                    st->executeUpdate();
                }

                // 5) 会话成员游标行。
                backfillGroupConvMembers(conn, groupId, conv);

                res.group.id = groupId;
                res.group.conversation_id = conv;
            });
            /*************避免数据库死锁的解决方案********************/

            // 事务提交后读回完整信息（成员数/角色/时间戳以数据库为准）。
            fillGroupOpResult(conn, res.group.id, owner, res);
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 2：我的群列表（BE-GROUP-05）
// /*************群聊功能：我的群列表***************/
void Db::listMyGroups(std::uint64_t userId, GroupListCb cb) {
    submit([this, userId, cb]() {
        bool ok = false;
        std::string err;
        std::vector<GroupInfo> groups;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            const std::string sql =
                "SELECT g.id,g.conversation_id,g.name,g.announcement,g.intro,g.avatar_url,"
                "g.owner_id,g.status,g.member_limit,"
                " (SELECT COUNT(*) FROM group_members m2 WHERE m2.group_id=g.id),"
                " gm.role,"
                " CAST(UNIX_TIMESTAMP(g.created_at)*1000 AS UNSIGNED)"
                " FROM `groups` g JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                "WHERE g.status=0 ORDER BY g.id ASC";
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            st->setUInt64(1, userId);
            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) groups.push_back(readGroupFull(*rs));
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, groups = std::move(groups)]() mutable {
            cb(ok, err, std::move(groups));
        });
    });
}

// ============================================================================
// 群 DAO 3：群详情 + 成员列表（BE-GROUP-05）
// /*************群聊功能：群详情***************/
void Db::getGroupDetail(std::uint64_t groupId, std::uint64_t userId, GroupOpCb cb) {
    submit([this, groupId, userId, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            res.group = loadGroupAsMember(conn, groupId, userId);
            if (res.group.status != 0) throw BizError("group dismissed");
            res.members = loadGroupMembers(conn, groupId);
            res.group.member_count = static_cast<int>(res.members.size());
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 4：修改群资料（BE-GROUP-05）
// ----------------------------------------------------------------------------
// 规则：空字符串=不修改该字段（MVP 约定；如需“清空公告”这类操作，
// 可把协议改成显式字段存在性判断，代价是 JSON 解析复杂一点）。
// 权限：群主/管理员（role<=2）。
// /*************群聊功能：修改群资料***************/
void Db::updateGroupInfo(std::uint64_t groupId, std::uint64_t actor,
                         const std::string& name, const std::string& announcement,
                         const std::string& intro, const std::string& avatarUrl,
                         GroupOpCb cb) {
    submit([this, groupId, actor, name, announcement, intro, avatarUrl, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            std::vector<std::pair<std::string, std::string>> sets;   // 列名->新值
            if (!name.empty())        sets.emplace_back("name", name);
            if (!announcement.empty()) sets.emplace_back("announcement", announcement);
            if (!intro.empty())       sets.emplace_back("intro", intro);
            if (!avatarUrl.empty())   sets.emplace_back("avatar_url", avatarUrl);
            if (name.size() > 128 || announcement.size() > 512 ||
                intro.size() > 255 || avatarUrl.size() > 255)
                throw BizError("field too long");
            if (sets.empty()) throw BizError("no fields to update");

            runTransactionWithRetry(conn, [&]() {
                // 锁群行 + 校验权限：FOR UPDATE 防“改资料”和“转让/解散”并发交错。
                std::uint64_t conv = 0;
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT g.conversation_id, gm.role FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("not a group member or dismissed");
                    if (rs->getInt(2) > 2) throw BizError("no permission");
                    conv = rs->getUInt64(1);
                }

                // 动态拼 SET（列名来自白名单常量，值走占位符，不存在注入）。
                std::string sql = "UPDATE `groups` SET ";
                for (std::size_t i = 0; i < sets.size(); ++i) {
                    if (i) sql += ",";
                    sql += sets[i].first + "=?";
                }
                sql += " WHERE id=?";
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                int idx = 1;
                for (auto& kv : sets) st->setString(idx++, kv.second);
                st->setUInt64(idx, groupId);
                st->executeUpdate();

                // 会话标题与群名保持同步（历史响应/单聊式 UI 会读 title）。
                if (!name.empty()) {
                    std::unique_ptr<sql::PreparedStatement> st2(conn->prepareStatement(
                        "UPDATE conversations SET title=? WHERE id=?"));
                    st2->setString(1, name);
                    st2->setUInt64(2, conv);
                    st2->executeUpdate();
                }
            });

            fillGroupOpResult(conn, groupId, actor, res);
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 5：解散群（BE-GROUP-02）
// ----------------------------------------------------------------------------
// 软删（status=1）+ 清成员，消息历史保留在 messages（解散后禁止再收发）。
// 返回“解散前”的成员列表，HTTP 层据此向全群广播 GroupDismissed。
// 同时清掉该群会话的离线消息，保证解散即时生效：
//   被解散的群不应再有“上线补推”的残留消息。
// /*************群聊功能：解散群***************/
void Db::dismissGroup(std::uint64_t groupId, std::uint64_t actor, GroupOpCb cb) {
    submit([this, groupId, actor, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            std::uint64_t conv = 0;
            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT g.conversation_id, gm.role FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("group not found or dismissed");
                    if (rs->getInt(2) != 1) throw BizError("only owner can dismiss");
                    conv = rs->getUInt64(1);
                }
                // 解散前先取成员列表（事件广播用）。
                res.members = loadGroupMembers(conn, groupId);

                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE `groups` SET status=1 WHERE id=?"));
                    st->setUInt64(1, groupId);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM group_members WHERE group_id=?"));
                    st->setUInt64(1, groupId);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM conversation_members WHERE conversation_id=?"));
                    st->setUInt64(1, conv);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM offline_messages WHERE conversation_id=?"));
                    st->setUInt64(1, conv);
                    st->executeUpdate();
                }
            });

            res.group = loadGroup(conn, groupId);
            res.group.my_role = 0;
            res.group.member_count = 0;
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 6：退群（BE-GROUP-02）
// 群主必须先转让；退群即时生效：删群成员行 + 会话游标行 + 该用户在此群的离线消息。
// /*************群聊功能：退群***************/
void Db::quitGroup(std::uint64_t groupId, std::uint64_t actor, GroupOpCb cb) {
    submit([this, groupId, actor, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            std::uint64_t conv = 0;
            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT g.conversation_id, gm.role FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("not a group member");
                    if (rs->getInt(2) == 1) throw BizError("owner must transfer first");
                    conv = rs->getUInt64(1);
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM group_members WHERE group_id=? AND user_id=?"));
                    st->setUInt64(1, groupId);
                    st->setUInt64(2, actor);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM conversation_members WHERE conversation_id=? AND user_id=?"));
                    st->setUInt64(1, conv);
                    st->setUInt64(2, actor);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM offline_messages WHERE conversation_id=? AND user_id=?"));
                    st->setUInt64(1, conv);
                    st->setUInt64(2, actor);
                    st->executeUpdate();
                }
            });

            fillGroupOpResult(conn, groupId, actor, res);   // actor 已不在成员表 -> 走 loadGroup 分支
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 7：转让群主（BE-GROUP-02/04）
// 同一事务保证“旧群主降、新群主升、群资料改 owner_id”三件事原子生效；
// 旧群主降为管理员（role=2），与主流 IM 行为一致。
// /*************群聊功能：转让群主***************/
void Db::transferGroup(std::uint64_t groupId, std::uint64_t actor,
                       std::uint64_t newOwner, GroupOpCb cb) {
    submit([this, groupId, actor, newOwner, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT gm.role FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("group not found or dismissed");
                    if (rs->getInt(1) != 1) throw BizError("only owner can transfer");
                }
                if (newOwner == actor) throw BizError("cannot transfer to yourself");
                const int targetRole = groupMemberRole(conn, groupId, newOwner, true);
                if (targetRole == 0) throw BizError("target is not a group member");

                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE group_members SET role=2 WHERE group_id=? AND user_id=?"));
                    st->setUInt64(1, groupId);
                    st->setUInt64(2, actor);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE group_members SET role=1 WHERE group_id=? AND user_id=?"));
                    st->setUInt64(1, groupId);
                    st->setUInt64(2, newOwner);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE `groups` SET owner_id=? WHERE id=?"));
                    st->setUInt64(1, newOwner);
                    st->setUInt64(2, groupId);
                    st->executeUpdate();
                }
            });

            fillGroupOpResult(conn, groupId, actor, res);
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 8：邀请成员（BE-GROUP-03）
// MVP 规则：群主/管理员可直接把“正常用户”拉入群（免确认，腾讯 Work 群行为）；
// 已在群里的 id 静默跳过（幂等）；人数受 member_limit 约束。
// 若被邀请人此前有“待处理申请”，审批通过后一并清理，防止重复通知。
// /*************群聊功能：邀请成员***************/
void Db::inviteMembers(std::uint64_t groupId, std::uint64_t actor,
                       const std::vector<std::uint64_t>& memberIds, GroupOpCb cb) {
    submit([this, groupId, actor, memberIds, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            std::vector<std::uint64_t> want = memberIds;
            want.erase(std::remove(want.begin(), want.end(), actor), want.end());
            std::sort(want.begin(), want.end());
            want.erase(std::unique(want.begin(), want.end()), want.end());
            if (want.empty()) throw BizError("no valid members to invite");

            std::vector<std::uint64_t> added;
            runTransactionWithRetry(conn, [&]() {
                std::uint64_t conv = 0;
                int limit = 0;
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT g.conversation_id, g.member_limit, gm.role FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("not a group owner/admin");
                    if (rs->getInt(3) > 2) throw BizError("no permission to invite");
                    conv = rs->getUInt64(1);
                    limit = static_cast<int>(rs->getUInt64(2));
                }

                // 过滤掉已在群里的成员（幂等）。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT user_id FROM group_members WHERE group_id=?"));
                    st->setUInt64(1, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    std::vector<std::uint64_t> exists;
                    while (rs->next()) exists.push_back(rs->getUInt64(1));
                    added.clear();
                    for (auto uid : want)
                        if (std::find(exists.begin(), exists.end(), uid) == exists.end())
                            added.push_back(uid);
                }
                if (added.empty()) throw BizError("all invitees already in group");

                ensureActiveUsers(conn, added);

                // 人数上限校验（当前成员数在锁内统计，避免并发超限）。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT COUNT(*) FROM group_members WHERE group_id=?"));
                    st->setUInt64(1, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    rs->next();
                    if (static_cast<int>(rs->getUInt64(1)) +
                        static_cast<int>(added.size()) > limit)
                        throw BizError("member limit exceeded");
                }

                insertGroupMembers(conn, groupId, added);
                backfillGroupConvMembers(conn, groupId, conv);

                // 被邀请人如果之前申请过这个群，把其待处理申请置为“已通过”，
                // 避免管理员之后又收到同一个人的重复申请通知。added 规模小，逐条更新。
                for (auto uid : added) {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE group_requests SET status=1, handler_id=? "
                        "WHERE group_id=? AND requester_id=? AND status=0"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    st->setUInt64(3, uid);
                    st->executeUpdate();
                }
            });

            fillGroupOpResult(conn, groupId, actor, res);
            res.group.my_role = groupMemberRole(conn, groupId, actor, false);
            res.affected_ids  = added;      // 本次真正新增的成员，HTTP 层据此推 added 事件
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 9：移出成员（BE-GROUP-04）
// 规则：群主/管理员可移“普通成员”；不能移群主/管理员（管理员也不能移其他管理员）；
// 本人被移时其在本群的离线消息一并删除，保证“即时收不到后续与积压消息”。
// /*************群聊功能：移出成员***************/
void Db::removeMember(std::uint64_t groupId, std::uint64_t actor,
                      std::uint64_t target, GroupOpCb cb) {
    submit([this, groupId, actor, target, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            runTransactionWithRetry(conn, [&]() {
                std::uint64_t conv = 0;
                int actorRole = 0;
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT g.conversation_id, gm.role FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("not a group owner/admin");
                    actorRole = rs->getInt(2);
                    conv = rs->getUInt64(1);
                }
                if (actorRole > 2) throw BizError("no permission");
                if (target == actor) throw BizError("use quit to leave group");

                const int targetRole = groupMemberRole(conn, groupId, target, true);
                if (targetRole == 0) throw BizError("target is not a group member");
                if (targetRole <= 2 && actorRole != 1)
                    throw BizError("only owner can remove admin");
                if (targetRole == 1) throw BizError("cannot remove group owner");

                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM group_members WHERE group_id=? AND user_id=?"));
                    st->setUInt64(1, groupId);
                    st->setUInt64(2, target);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM conversation_members WHERE conversation_id=? AND user_id=?"));
                    st->setUInt64(1, conv);
                    st->setUInt64(2, target);
                    st->executeUpdate();
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "DELETE FROM offline_messages WHERE conversation_id=? AND user_id=?"));
                    st->setUInt64(1, conv);
                    st->setUInt64(2, target);
                    st->executeUpdate();
                }
            });

            fillGroupOpResult(conn, groupId, actor, res);
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res, target]() {
            (void)target;
            cb(ok, err, res);
        });
    });
}

// ============================================================================
// 群 DAO 10：设置/取消管理员（BE-GROUP-04，仅群主）
// /*************群聊功能：任命管理员***************/
void Db::setAdmin(std::uint64_t groupId, std::uint64_t actor,
                  std::uint64_t target, bool admin, GroupOpCb cb) {
    submit([this, groupId, actor, target, admin, cb]() {
        bool ok = false;
        std::string err;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT gm.role FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("group not found or dismissed");
                    if (rs->getInt(1) != 1) throw BizError("only owner can set admin");
                }
                if (target == actor) throw BizError("cannot change your own role");
                const int targetRole = groupMemberRole(conn, groupId, target, true);
                if (targetRole == 0) throw BizError("target is not a group member");
                if (targetRole == 1) throw BizError("cannot change owner role");
                if (admin && targetRole != 3) throw BizError("target already admin");
                if (!admin && targetRole != 2) throw BizError("target is not admin");

                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE group_members SET role=? WHERE group_id=? AND user_id=?"));
                    st->setInt(1, admin ? 2 : 3);
                    st->setUInt64(2, groupId);
                    st->setUInt64(3, target);
                    st->executeUpdate();
                }
            });

            fillGroupOpResult(conn, groupId, actor, res);
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, res]() { cb(ok, err, res); });
    });
}

// ============================================================================
// 群 DAO 11：申请入群（BE-GROUP-03）
// MVP：任何人都能按 groupId 申请；申请后等群主/管理员审批。
// 已在群里 / 已有待处理申请的人会被拒绝（幂等提示）。
// /*************群聊功能：申请入群***************/
void Db::createGroupRequest(std::uint64_t groupId, std::uint64_t requester,
                            const std::string& message, GroupReqCb cb) {
    submit([this, groupId, requester, message, cb]() {
        bool ok = false;
        std::string err;
        GroupRequestInfo out;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            if (message.size() > 255) throw BizError("message too long");

            std::uint64_t reqId = 0;
            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT 1 FROM `groups` WHERE id=? AND status=0"));
                    st->setUInt64(1, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("group not found or dismissed");
                }
                if (groupMemberRole(conn, groupId, requester, false) != 0)
                    throw BizError("already in group");
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT id FROM group_requests "
                        "WHERE group_id=? AND requester_id=? AND status=0"));
                    st->setUInt64(1, groupId);
                    st->setUInt64(2, requester);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (rs->next()) throw BizError("request already pending");
                }
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT INTO group_requests(group_id,requester_id,message) "
                        "VALUES(?,?,?)"));
                    st->setUInt64(1, groupId);
                    st->setUInt64(2, requester);
                    st->setString(3, message);
                    st->executeUpdate();
                }
                reqId = lastInsertId(conn);
            });

            out = loadGroupRequest(conn, reqId);
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
// 群 DAO 12：审批入群申请（BE-GROUP-03/04）
// ----------------------------------------------------------------------------
// 关键点：
//   * SELECT ... FOR UPDATE 锁申请行，防同一申请被并发审批两次；
//   * 审批人必须属于该群且 role<=2（群主/管理员）；
//   * 通过时同事务写 group_members + 游标行（复用邀请的 backfill）；
//   * result_notified 置 0：申请人离线时由登录补偿推送（与好友一致）。
// /*************群聊功能：审批入群申请***************/
void Db::handleGroupRequest(std::uint64_t requestId, std::uint64_t actor,
                            bool accept, GroupReqOpCb cb) {
    submit([this, requestId, actor, accept, cb]() {
        bool ok = false;
        std::string err;
        GroupRequestInfo req;
        GroupOpResult res;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            std::uint64_t groupId = 0, conv = 0, requester = 0;
            runTransactionWithRetry(conn, [&]() {
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT gr.group_id, gr.requester_id, g.conversation_id, gm.role "
                        "FROM group_requests gr "
                        "JOIN `groups` g ON g.id=gr.group_id "
                        "JOIN group_members gm ON gm.group_id=gr.group_id AND gm.user_id=? "
                        "WHERE gr.id=? AND gr.status=0 AND g.status=0 FOR UPDATE"));
                    st->setUInt64(1, actor);
                    st->setUInt64(2, requestId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("request not found or already handled");
                    if (rs->getInt(4) > 2) throw BizError("no permission to handle");
                    groupId    = rs->getUInt64(1);
                    requester  = rs->getUInt64(2);
                    conv       = rs->getUInt64(3);
                }

                if (accept) {
                    // 人数上限校验（锁内统计）。
                    int limit = 0;
                    {
                        std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                            "SELECT member_limit, "
                            " (SELECT COUNT(*) FROM group_members WHERE group_id=?) "
                            "FROM `groups` WHERE id=?"));
                        st->setUInt64(1, groupId);
                        st->setUInt64(2, groupId);
                        std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                        rs->next();
                        limit = static_cast<int>(rs->getUInt64(1));
                        if (static_cast<int>(rs->getUInt64(2)) + 1 > limit)
                            throw BizError("member limit exceeded");
                    }
                    insertGroupMembers(conn, groupId, {requester});
                    backfillGroupConvMembers(conn, groupId, conv);
                }

                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE group_requests SET status=?, handler_id=?, result_notified=0 "
                        "WHERE id=?"));
                    st->setInt(1, accept ? 1 : 2);
                    st->setUInt64(2, actor);
                    st->setUInt64(3, requestId);
                    st->executeUpdate();
                }
            });

            req = loadGroupRequest(conn, requestId);
            fillGroupOpResult(conn, groupId, actor, res);
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, req, res]() { cb(ok, err, req, res); });
    });
}

// ============================================================================
// 群 DAO 13：入群申请列表（BE-GROUP-03）
// incoming：发给我审批的（我是群主/管理员的群，只看待处理）；
// outgoing：我发出的；all：两者。
// /*************群聊功能：入群申请列表***************/
void Db::listGroupRequests(std::uint64_t userId, const std::string& direction,
                           GroupReqListCb cb) {
    submit([this, userId, direction, cb]() {
        bool ok = false;
        std::string err;
        std::vector<GroupRequestInfo> reqs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::string sql = kGroupRequestSelect;
            if (direction == "outgoing") {
                sql += "WHERE gr.requester_id=? ORDER BY gr.created_at DESC LIMIT 200";
            } else if (direction == "all") {
                sql += "LEFT JOIN group_members gm2 "
                       "ON gm2.group_id=gr.group_id AND gm2.user_id=? AND gm2.role<=2 "
                       "WHERE (gr.requester_id=? OR (gm2.user_id IS NOT NULL AND gr.status=0)) "
                       "ORDER BY gr.created_at DESC LIMIT 200";
            } else {
                // incoming（默认）：我管理的群里的待处理申请。
                sql += "JOIN group_members gm ON gm.group_id=gr.group_id "
                       "AND gm.user_id=? AND gm.role<=2 "
                       "WHERE gr.status=0 ORDER BY gr.created_at DESC LIMIT 200";
            }

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            if (direction == "outgoing") {
                st->setUInt64(1, userId);
            } else if (direction == "all") {
                st->setUInt64(1, userId);
                st->setUInt64(2, userId);
            } else {
                st->setUInt64(1, userId);
            }

            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) reqs.push_back(readGroupRequest(*rs));
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
// 群 DAO 14：群消息写扩散（BE-GROUP-07）
// ----------------------------------------------------------------------------
// 与单聊 sendMessage 同构，差别在：
//   1) conversation_id 由 groupId 查 groups 得到（不建会话、不做 dedup_key）；
//   2) 发送资格 = “是群成员且群未解散”（成员行解散/退群时被删，天然即时生效）；
//   3) 离线队列 INSERT ... SELECT 一条 SQL 写给除发送者外的全部成员；
//   4) 回调带回成员 userId 列表，io 层遍历 SessionManager 直推在线成员。
// 写扩散 vs 读扩散：本期群小（<=200），写扩散实现最简单、读路径零成本；
// 将来上万成员时，可只存消息本体不写 offline_messages，改为按
// conversation_members.last_delivered_seq 补拉（读扩散），DAO 接口不变。
// /*************群聊功能：群消息写扩散***************/
void Db::sendGroupMessage(std::uint64_t sender, std::uint64_t groupId, int type,
                          const std::string& content, std::uint64_t clientMsgId,
                          GroupSendCb cb) {
    if (!submitSql(sqlQueue_, [this, sender, groupId, type, content, clientMsgId, cb]() {
        bool ok = false;
        std::string err;
        StoredMessage out;
        std::vector<std::uint64_t> memberIds;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            std::uint64_t msgId = 0;
            runTransactionWithRetry(conn, [&]() {
                ok = false;
                msgId = 0;

                // 0) 发送资格：成员 + 群未解散；顺带拿到群会话 id。
                std::uint64_t conv = 0;
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT g.conversation_id FROM `groups` g "
                        "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                        "WHERE g.id=? AND g.status=0"));
                    st->setUInt64(1, sender);
                    st->setUInt64(2, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    if (!rs->next()) throw BizError("not a group member or group dismissed");
                    conv = rs->getUInt64(1);
                }

                // 1) 去重：同一会话 + 发送者 + client_msg_id 已存在则返回旧消息。
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
                        out.to = groupId;
                        out.is_group = true;
                        out.duplicate = true;
                        ok = true;
                        return;
                    }
                }

                // 2) 原子分配会话内单调 seq（LAST_INSERT_ID(expr) 技巧，同单聊）。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "UPDATE conversations SET last_seq=LAST_INSERT_ID(last_seq+1) WHERE id=?"));
                    st->setUInt64(1, conv);
                    st->executeUpdate();
                }
                const std::uint64_t seq = lastInsertId(conn);

                // 3) 插入消息本体（msg_id/ts 由数据库生成）。
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

                // 4) 写扩散（离线部分）。
                /*************P1-4：群消息的写扩散成本随成员数线性增长***************/
                // 行级队列模式：一条 INSERT ... SELECT 给除发送者外全体成员写
                //   offline_messages —— 100 人群就是每条消息 100 行 + 后续 100 次删除。
                // 游标模式：什么都不写。成员登录/翻页时按
                //   messages.seq > conversation_members.last_delivered_seq 计算未收，
                //   写放大与成员数无关（这才是"群聊写扩散"真正的规模问题）。
                if (!rt_.offlineCursor) {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "INSERT IGNORE INTO offline_messages(user_id,message_id,conversation_id,seq) "
                        "SELECT gm.user_id,?,?,? FROM group_members gm "
                        "WHERE gm.group_id=? AND gm.user_id<>?"));
                    st->setUInt64(1, msgId);
                    st->setUInt64(2, conv);
                    st->setUInt64(3, seq);
                    st->setUInt64(4, groupId);
                    st->setUInt64(5, sender);
                    st->executeUpdate();
                }
                /*************P1-4：群消息的写扩散成本随成员数线性增长***************/

                // 5) 发送者自己游标推进，避免把自己的消息算未读。
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

                // 6) 成员列表（在线直推用）。
                {
                    std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                        "SELECT user_id FROM group_members WHERE group_id=?"));
                    st->setUInt64(1, groupId);
                    std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                    memberIds.clear();
                    while (rs->next()) memberIds.push_back(rs->getUInt64(1));
                }
            });

            // 7) 事务提交成功后读回服务端生成的 ts/msg_id（与单聊一致）：
            //    正常发送路径事务体不置 ok，用 !ok 判定“需要读回”；
            //    去重命中时事务体已置 ok=true 且 out.duplicate=true，跳过读回。
            if (!ok) {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT id,conversation_id,seq,sender_id,type,content,"
                    "CAST(UNIX_TIMESTAMP(created_at)*1000 AS UNSIGNED) "
                    "FROM messages WHERE id=?"));
                st->setUInt64(1, msgId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                if (rs->next()) {
                    out = readStored(*rs);
                    out.to = groupId;
                    out.is_group = true;
                    ok = true;
                } else {
                    err = "message inserted but not found";
                }
            }
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, out, memberIds]() { cb(ok, err, out, memberIds); });
    })) {
        postToIo([cb]() {
            cb(false, kBusyError, StoredMessage{}, std::vector<std::uint64_t>{});
        });
    }
}

// ============================================================================
// 群 DAO 15：群历史分页（BE-GROUP-08 之外的分页能力）
// ----------------------------------------------------------------------------
// 已退群/被移出/解散后的处理策略：
//   fetchGroupHistory 先校验“当前仍是成员且群未解散”，不满足直接拒绝。
//   理由：群历史属于群内数据，成员资格消失后服务端不再授权；
//   客户端本地缓存不受影响（如需“解散后只读历史”，可放宽为
//   “groups.status IN (0,1) + 历史成员表”，本期不做）。
// /*************群聊功能：群历史分页***************/
void Db::fetchGroupHistory(std::uint64_t groupId, std::uint64_t userId,
                           std::uint64_t beforeSeq, int limit, MsgListCb cb) {
    if (!submitSql(sqlQueue_, [this, groupId, userId, beforeSeq, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<StoredMessage> msgs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            sql::Connection* conn = pc.get();

            // 1) 成员资格 + 会话 id。
            std::uint64_t conv = 0;
            {
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(
                    "SELECT g.conversation_id FROM `groups` g "
                    "JOIN group_members gm ON gm.group_id=g.id AND gm.user_id=? "
                    "WHERE g.id=? AND g.status=0"));
                st->setUInt64(1, userId);
                st->setUInt64(2, groupId);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                if (!rs->next()) throw BizError("not a group member or group dismissed");
                conv = rs->getUInt64(1);
            }

            // 2) keyset 分页（与单聊一致：倒序取 N 条再翻转成升序）。
            if (limit <= 0)  limit = 20;
            if (limit > 100) limit = 100;
            {
                const std::string sql =
                    "SELECT m.id,m.conversation_id,m.seq,m.sender_id,m.type,m.content,"
                    "CAST(UNIX_TIMESTAMP(m.created_at)*1000 AS UNSIGNED) "
                    "FROM messages m "
                    "WHERE m.conversation_id=? AND (?=0 OR m.seq<?) "
                    "ORDER BY m.seq DESC "
                    "LIMIT " + std::to_string(limit);
                std::unique_ptr<sql::PreparedStatement> st(conn->prepareStatement(sql));
                st->setUInt64(1, conv);
                st->setUInt64(2, beforeSeq);
                st->setUInt64(3, beforeSeq);
                std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
                while (rs->next()) {
                    StoredMessage m = readStored(*rs);
                    m.to = groupId;
                    m.is_group = true;
                    msgs.push_back(std::move(m));
                }
                std::reverse(msgs.begin(), msgs.end());
            }
            ok = true;
        } catch (const BizError& e) {
            err = e.what();
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }

        postToIo([cb, ok, err, msgs = std::move(msgs)]() mutable {
            cb(ok, err, std::move(msgs));
        });
    })) {
        postToIo([cb]() {
            cb(false, kBusyError, std::vector<StoredMessage>{});
        });
    }
}

// ============================================================================
// 群 DAO 16：登录补推——待我审批的入群申请（BE-GROUP-06）
// /*************群聊功能：审批人登录补推***************/
void Db::fetchPendingGroupRequestsForApprover(std::uint64_t userId, int limit,
                                              GroupReqListCb cb) {
    submit([this, userId, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<GroupRequestInfo> reqs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            if (limit <= 0)  limit = 100;
            if (limit > 200) limit = 200;

            std::string sql = std::string(kGroupRequestSelect) +
                "JOIN group_members gm ON gm.group_id=gr.group_id "
                "AND gm.user_id=? AND gm.role<=2 "
                "WHERE gr.status=0 AND g.status=0 "
                "ORDER BY gr.created_at ASC LIMIT " + std::to_string(limit);
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            st->setUInt64(1, userId);
            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) reqs.push_back(readGroupRequest(*rs));
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
// 群 DAO 17：登录补推——未通知的入群申请结果（BE-GROUP-06）
// /*************群聊功能：申请人登录补推***************/
void Db::fetchUnnotifiedGroupResults(std::uint64_t userId, int limit,
                                     GroupReqListCb cb) {
    submit([this, userId, limit, cb]() mutable {
        bool ok = false;
        std::string err;
        std::vector<GroupRequestInfo> reqs;

        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            if (limit <= 0)  limit = 100;
            if (limit > 200) limit = 200;

            std::string sql = std::string(kGroupRequestSelect) +
                "WHERE gr.requester_id=? AND gr.status IN (1,2) "
                "AND gr.result_notified=0 "
                "ORDER BY gr.updated_at ASC LIMIT " + std::to_string(limit);
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(sql));
            st->setUInt64(1, userId);
            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            while (rs->next()) reqs.push_back(readGroupRequest(*rs));
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
// 群 DAO 18：标记入群申请结果已通知（BE-GROUP-06，批量/单条）
// /*************群聊功能：申请结果通知标记***************/
void Db::markGroupResultsNotified(std::uint64_t userId, StatusCb cb) {
    submit([this, userId, cb]() {
        bool ok = false;
        std::string err;
        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "UPDATE group_requests SET result_notified=1 "
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

void Db::markGroupRequestNotified(std::uint64_t requestId, StatusCb cb) {
    submit([this, requestId, cb]() {
        bool ok = false;
        std::string err;
        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "UPDATE group_requests SET result_notified=1 "
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
/*************群聊功能：群 DAO 实现结束***************/

// ============================================================================
// /*************图片文件功能（七牛云直传）：files DAO***************/
// ----------------------------------------------------------------------------
// 分工回顾（与规划一致）：
//   * 上传凭证签发（upload-token）**不碰数据库**——签发记录是无状态票据
//     （object_store.cpp:makeTicket），服务器重启也不丢；
//   * commit 才是 files 表唯一的一次 INSERT，且 size/qetag 以七牛 stat
//     返回为准（调用方把 stat 结果填进 FileRecord 再传进来）；
//   * TCP 发送校验走 getFileByUuid（热路径，用硬上限投递口 submitSql：
//     队列满立即回 kBusyError，客户端能收到明确的 Error 帧而不是干等）；
//   * 删除/配额是低频操作，用软上限 submit（回调签名多样、无法统一回忙，
//     原因见 db.h 的 submit 注释——同一个纪律，不在这里重新发明）。
//
// 标准 SELECT 列顺序（readFileRow 依赖，改动必须同步）：
//   id, uuid, uploader_id, name, size, mime, qetag, object_key, bucket, zone, status
// ============================================================================
namespace {

FileRecord readFileRow(sql::ResultSet& rs) {
    FileRecord rec;
    rec.found      = true;
    rec.id         = rs.getUInt64(1);
    rec.uuid       = sstr(rs.getString(2));
    rec.uploader_id = rs.getUInt64(3);
    rec.name       = sstr(rs.getString(4));
    rec.size       = static_cast<std::int64_t>(rs.getUInt64(5));
    rec.mime       = sstr(rs.getString(6));
    rec.qetag      = sstr(rs.getString(7));
    rec.object_key = sstr(rs.getString(8));
    rec.bucket     = sstr(rs.getString(9));
    rec.zone       = rs.getInt(10);
    rec.status     = rs.getInt(11);
    return rec;
}

const char* kFileSelectCols =
    "SELECT id,uuid,uploader_id,name,size,mime,qetag,object_key,bucket,zone,status ";

} // namespace

// 按 uuid 查一条文件记录。uuid 不区分"不存在"与"已删除"：
// 回调统一 found=false，由上层决定文案（发送校验要拒绝、下载要 404，语义天然不同）。
void Db::getFileByUuid(const std::string& uuid, FileMetaCb cb) {
    if (!submitSql(sqlQueue_, [this, uuid, cb]() {
        bool ok = false;
        std::string err;
        FileRecord rec;
        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                std::string(kFileSelectCols) + "FROM files WHERE uuid=?"));
            st->setString(1, uuid);
            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            if (rs->next()) rec = readFileRow(*rs);
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }
        postToIo([cb, ok, err, rec]() { cb(ok, err, rec); });
    })) {
        postToIo([cb]() { cb(false, kBusyError, FileRecord{}); });
    }
}

// commit 登记：七牛 stat 成功后，用 stat 权威值插入 files。
// 唯一键 (uuid) 兜底并发重复 commit：第二次 INSERT 触发 1062，
// 当作"已登记"幂等成功并回读原行 id——与注册用户名重复的处理同一手法。
void Db::commitFile(const FileRecord& rec, FileIdCb cb) {
    fileCrumb("db.commitFile.submit");
    if (!submitSql(sqlQueue_, [this, rec, cb]() {
        bool ok = false;
        std::string err;
        std::uint64_t fileId = 0;
        try {
            fileCrumb("db.commitFile.task-begin");
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");

            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "INSERT INTO files(uuid,uploader_id,name,size,mime,qetag,object_key,bucket,zone,status) "
                "VALUES(?,?,?,?,?,?,?,?,?,1) "
                "ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)"));
            st->setString(1, rec.uuid);
            st->setUInt64(2, rec.uploader_id);
            st->setString(3, rec.name);
            st->setUInt64(4, static_cast<std::uint64_t>(rec.size));
            st->setString(5, rec.mime);
            st->setString(6, rec.qetag);
            st->setString(7, rec.object_key);
            st->setString(8, rec.bucket);
            st->setInt(9, rec.zone);
            st->executeUpdate();

            // ON DUPLICATE 时 LAST_INSERT_ID(id) 借道回传原行 id（同会话消息
            // 的 conv id 分配技巧一脉相承；连接器 1.1.9 没有 getLastInsertId()，
            // 原因见 lastInsertId() 的注释）。
            fileId = lastInsertId(pc.get());
            ok = true;
            fileCrumb("db.commitFile.task-done");
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }
        postToIo([cb, ok, err, fileId]() { cb(ok, err, fileId); });
    })) {
        postToIo([cb]() { cb(false, kBusyError, 0); });
    }
}

// 标记删除（仅上传者本人、仅 status=1）。0 行=不是你的文件/已删/不存在，
// 统一 ok=false + "not found"，不区分原因（防探测别人有多少文件）。
// 物理删除七牛对象由 HTTP 层在事务外发起（remove 是外网调用，不能占 SQL 事务）。
void Db::deleteFileByUuid(const std::string& uuid, std::uint64_t uploader, FileMetaCb cb) {
    submit([this, uuid, uploader, cb]() {
        bool ok = false;
        std::string err;
        FileRecord rec;
        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "UPDATE files SET status=2 WHERE uuid=? AND uploader_id=? AND status=1"));
            st->setString(1, uuid);
            st->setUInt64(2, uploader);
            if (st->executeUpdate() == 0) {
                err = "not found";   // 没更新到行：不是你的、已删或不存在（口径统一，防探测）
            } else {
                // 带回整行：HTTP 层需要 object_key 去调七牛物理删除。
                // UPDATE 与 SELECT 之间同行被并发再删不影响本用途（key 不变）。
                std::unique_ptr<sql::PreparedStatement> qs(pc->prepareStatement(
                    std::string(kFileSelectCols) + "FROM files WHERE uuid=?"));
                qs->setString(1, uuid);
                std::unique_ptr<sql::ResultSet> rs(qs->executeQuery());
                if (rs->next()) rec = readFileRow(*rs);
                ok = true;
            }
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }
        postToIo([cb, ok, err, rec]() { cb(ok, err, rec); });
    });
}

// 当日已用配额（字节）。按 status=1 行的 size 求和：
// 已知口径——虚报 commit 失败/直传后从未 commit 的对象不计入，
// 这是"无状态票据"设计的代价（不写签发表就没法统计未 commit 的申报量），
// 孤儿对象由告警策略兜底（见规划"已知边界"）。
void Db::fileQuotaToday(std::uint64_t uploader, BytesCb cb) {
    submit([this, uploader, cb]() {
        bool ok = false;
        std::string err;
        long long bytes = 0;
        try {
            PooledConn pc(*pool_);
            if (!pc) throw std::runtime_error("db pool unavailable");
            std::unique_ptr<sql::PreparedStatement> st(pc->prepareStatement(
                "SELECT COALESCE(SUM(size),0) FROM files "
                "WHERE uploader_id=? AND status=1 AND created_at >= CURDATE()"));
            st->setUInt64(1, uploader);
            std::unique_ptr<sql::ResultSet> rs(st->executeQuery());
            if (rs->next()) {
                // SUM(BIGINT) 返回 DECIMAL：getString 再转，比 getUInt64 兼容性好
                // （Connector 对 DECIMAL 列直接 getUInt64 在部分版本会抛）。
                const std::string v = sstr(rs->getString(1));
                try { bytes = std::stoll(v); } catch (...) { bytes = 0; }
            }
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("db error: ") + e.what();
        }
        postToIo([cb, ok, err, bytes]() { cb(ok, err, bytes); });
    });
}
/*************图片文件功能（七牛云直传）：files DAO 结束***************/

} // namespace kc
