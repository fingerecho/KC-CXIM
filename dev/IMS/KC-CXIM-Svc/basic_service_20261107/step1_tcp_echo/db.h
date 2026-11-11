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
#include "friend_types.h"
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
    // 注册回调：ok=false 时 err 为原因；成功时带回数据库生成的 userId。
    using RegisterCb = std::function<void(bool ok, const std::string& err,
                                          std::uint64_t userId)>;
    // HTTP 登录回调：成功时带回 userId 与 username，供上层签发 JWT。
    using LoginUserCb = std::function<void(bool ok, const std::string& err,
                                           std::uint64_t userId,
                                           const std::string& username)>;
    using SendCb    = std::function<void(bool ok, const std::string& err,
                                         const StoredMessage& msg)>;
    using MsgListCb = std::function<void(bool ok, const std::string& err,
                                         std::vector<StoredMessage> msgs)>;
    using StatusCb  = std::function<void(bool ok, const std::string& err)>;

    // ---- 好友模块回调 ------------------------------------------------------
    using UserListCb    = std::function<void(bool ok, const std::string& err,
                                             std::vector<UserInfo> users)>;
    using FriendListCb  = std::function<void(bool ok, const std::string& err,
                                             std::vector<FriendInfo> friends)>;
    using RequestCb     = std::function<void(bool ok, const std::string& err,
                                             const FriendRequestInfo& req)>;
    using RequestListCb = std::function<void(bool ok, const std::string& err,
                                             std::vector<FriendRequestInfo> reqs)>;

    // pool_size：连接池大小（建议 4~8）；worker_count：DB 工作线程数（建议 2~4）。
    Db(boost::asio::io_context& io, const DbConfig& cfg,
       std::size_t pool_size = 4, std::size_t worker_count = 2);
    ~Db();

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    // 登录校验：查 users.password_hash 与 credential 是否一致。
    // 注意：这是旧的"把密码/哈希直接当 token"的实现，保留仅为兼容；
    // 新的 HTTP 登录走 loginUser()，TCP 登录改为只验 JWT，不再调用本函数。
    void login(std::uint64_t userId, const std::string& credential, LoginCb cb);

    // 注册：检查用户名是否重复 -> 在 worker 线程里用 bcrypt 计算哈希 -> 插入 users。
    // 整个流程（尤其是 bcrypt 这种重 CPU 操作）都在 DB worker 线程执行，
    // 绝不占用 io_context 线程，避免阻塞所有网络收发。
    void registerUser(const std::string& username, const std::string& password,
                      RegisterCb cb);

    // HTTP 登录：按 username 查 users，在 worker 线程里用 bcrypt verify 校验密码。
    // 成功后返回 userId/username；密码哈希计算同样不占用 io_context 线程。
    void loginUser(const std::string& username, const std::string& password,
                   LoginUserCb cb);

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

    // ========================================================================
    // 好友模块 DAO
    // ------------------------------------------------------------------------
    // 设计说明：
    //   * 搜索、申请、通过/拒绝、列表、备注、删除、拉黑全部走 HTTP；
    //   * 每次“事实变化”完成后，HTTP 层再通过 SessionManager 把事件推给
    //     在线的相关用户；离线用户的申请/结果依靠数据库状态补偿。
    //   * 所有 SQL 仍然在 worker 线程执行，回调通过 postToIo 回到 io_context，
    //     因此业务层（HTTP/TCP）拿到的回调一定是线程安全的。
    // ========================================================================

    // BE-FRIEND-01：按用户名/昵称/用户ID 搜索用户。
    // selfId 会从结果里排除，避免用户搜到自己；只返回公开字段。
    void searchUsers(std::uint64_t selfId, const std::string& keyword,
                     int limit, UserListCb cb);

    // BE-FRIEND-02/03：创建好友申请。
    // 会在一个事务里校验：目标存在、不是自己、未已是好友、未被对方拉黑、
    // 没有反向待处理申请；同一方向的历史拒绝记录会被重置为 pending。
    void createFriendRequest(std::uint64_t requesterId, std::uint64_t targetId,
                             const std::string& message, RequestCb cb);

    // BE-FRIEND-04：处理好友申请（accept=true 通过，false 拒绝）。
    // 必须由被申请人本人处理；通过时在同一事务里写入双向好友关系。
    void handleFriendRequest(std::uint64_t userId, std::uint64_t requestId,
                             bool accept, RequestCb cb);

    // BE-FRIEND-05：申请列表（direction=incoming/outgoing/all）。
    void listFriendRequests(std::uint64_t userId, const std::string& direction,
                            RequestListCb cb);

    // BE-FRIEND-05：好友列表（只返回 status=1 的正式好友）。
    void listFriends(std::uint64_t userId, FriendListCb cb);

    // BE-FRIEND-05：修改“我对好友的备注”，只影响自己这一行关系。
    void updateFriendRemark(std::uint64_t userId, std::uint64_t friendId,
                            const std::string& remark, StatusCb cb);

    // BE-FRIEND-05：删除好友（双向删除，并清理双方待处理申请）。
    void deleteFriend(std::uint64_t userId, std::uint64_t friendId, StatusCb cb);

    // BE-FRIEND-05：拉黑/取消拉黑。
    // 拉黑会删除双方好友行，再写入 (userId,friendId,status=2) 的单向拉黑行；
    // 取消拉黑只删除这条状态为 2 的行。
    void setFriendBlocked(std::uint64_t userId, std::uint64_t friendId,
                          bool blocked, StatusCb cb);

    // BE-FRIEND-06：登录时补推“待处理申请”（离线补偿）。
    void fetchPendingIncoming(std::uint64_t userId, int limit, RequestListCb cb);

    // BE-FRIEND-06：登录时补推“已处理但申请人还没收到结果”的申请。
    void fetchUnnotifiedResults(std::uint64_t userId, int limit, RequestListCb cb);

    // BE-FRIEND-06：把补推过的结果标记为已通知，避免每次登录重复推送。
    void markResultsNotified(std::uint64_t userId, StatusCb cb);

    // BE-FRIEND-06：申请人在线、结果已实时推送时，精确标记这一条申请已通知。
    // 与 markResultsNotified() 的区别：这里只动指定的申请，不会误伤其他记录。
    void markRequestResultNotified(std::uint64_t requestId, StatusCb cb);

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
