#pragma once
#include <boost/asio/io_context.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "db_config.h"
#include "message.h"

namespace sql { class Connection; }   // 不在头文件里暴露 Connector 细节

// ============================================================================
// Db：数据库访问层（连接池 + 工作线程池 + DAO）
// ----------------------------------------------------------------------------
// 为什么要线程池？
//   MariaDB Connector/C++ 是"同步阻塞"API。如果直接在 io_context 线程里
//   执行 SQL，一条慢查询会把所有网络收发卡住。因此：
//     1) 网络线程(io_context) 只负责收发和解析；
//     2) 需要查/写库时，把任务丢进本类的 worker 线程池；
//     3) worker 从连接池借一条连接执行 SQL；
//     4) 结果通过 boost::asio::post 回到 io_context，再回调业务方。
//
// 连接池：
//   用一个固定大小的空闲队列 + 互斥锁/条件变量实现。借出的连接用
//   PooledConnection(RAII) 包装，析构时自动归还，异常路径也不会泄漏。
//   每次借出会做 isValid() 健康检查，坏连接自动重建。
// ============================================================================

namespace kc {

class ConnectionPool;   // 定义在 db.cpp 内部

class Db {
public:
    using LoginCb   = std::function<void(bool ok, const std::string& err)>;
    using SendCb    = std::function<void(bool ok, const std::string& err,
                                         const StoredMessage& msg)>;
    using MsgListCb = std::function<void(bool ok, const std::string& err,
                                         std::vector<StoredMessage> msgs)>;
    using StatusCb  = std::function<void(bool ok, const std::string& err)>;

    // pool_size：连接池大小（建议 4~8）；worker_count：DB 工作线程数（建议 2~4）。
    Db(boost::asio::io_context& io, const DbConfig& cfg,
       std::size_t pool_size = 4, std::size_t worker_count = 2);
    ~Db();

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    // 登录校验：查 users.password_hash 与 credential 是否一致。
    void login(std::uint64_t userId, const std::string& credential, LoginCb cb);

    // 单聊发送：同一事务内
    //   1) 取/建会话；2) 建成员；3) 去重检查；4) 分配 seq；5) 插消息；
    //   6) 给接收者写离线队列；7) 更新发送者已读游标。
    void sendMessage(std::uint64_t sender, std::uint64_t to, int type,
                     const std::string& content, std::uint64_t clientMsgId,
                     SendCb cb);

    // 拉取离线消息（按会话 + seq 升序）。
    void fetchOffline(std::uint64_t userId, int limit, MsgListCb cb);

    // 历史分页：单聊会话由 (requester, peer) 推导；seq < beforeSeq，0 表示最新。
    void fetchHistory(std::uint64_t userId, std::uint64_t peerId,
                      std::uint64_t beforeSeq, int limit, MsgListCb cb);

    // 更新投递/已读游标，并删除已投递的离线消息。
    void updateReceipt(std::uint64_t userId, std::uint64_t conversationId,
                       std::uint64_t deliveredSeq, std::uint64_t readSeq,
                       StatusCb cb);

private:
    void submit(std::function<void()> job);       // 投递到 worker
    void workerLoop();                            // worker 主循环
    void postToIo(std::function<void()> fn);      // 回到 io_context 线程

    boost::asio::io_context&        io_;
    std::unique_ptr<ConnectionPool> pool_;

    std::mutex                       jobs_mutex_;
    std::condition_variable          jobs_cv_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread>         workers_;
    bool                             stopping_ = false;
};

} // namespace kc
