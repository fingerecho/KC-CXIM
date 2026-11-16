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
#include "group_types.h"      // /*************群聊功能：群领域模型（GroupInfo 等）***************/
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
    // BE-SEND-01：发送前的关系校验回调，带回“是否好友”和“是否存在拉黑”。
    using RelationCb = std::function<void(bool ok, const std::string& err,
                                          bool areFriends, bool blocked)>;

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

    // BE-SEND-01：发消息前校验 sender 与 to 的关系（双向好友 / 拉黑）。
    // areFriends 仅在双向都存在 status=1 行时为 true；
    // 任一方向 status=2 则 blocked=true（用于区分错误文案）。
    // 陌生人与被对方拉黑都会得到 areFriends=false，从而在落库前被拒绝。
    void areFriends(std::uint64_t sender, std::uint64_t to, RelationCb cb);

    // ========================================================================
    // /*************群聊功能：群 DAO（BE-GROUP-01~08）***************/
    // ------------------------------------------------------------------------
    // 与好友模块一致的分工：
    //   * 所有“写事实”（建群/邀请/移除/转让/解散/审批/改资料）走 HTTP，
    //     在同一事务里维护 groups / group_members / conversation_members，
    //     并通过返回 GroupOpResult（含操作后完整成员列表）供上层广播事件；
    //   * 群消息“写扩散”也在这里完成：一个事务内分配 conversation_id+seq、
    //     插入 messages、给除发送者外的每个成员写 offline_messages，
    //     再把成员 userId 列表带回给 TCP 层，在线成员直接投递。
    //   * 所有 SQL 仍在 worker 线程执行，回调 postToIo 回 io_context。
    // ========================================================================

    // 群操作回调：统一带回 GroupOpResult（group + 操作后 members）。
    using GroupOpCb        = std::function<void(bool ok, const std::string& err,
                                                const GroupOpResult& res)>;
    using GroupListCb      = std::function<void(bool ok, const std::string& err,
                                                std::vector<GroupInfo> groups)>;
    using GroupReqCb       = std::function<void(bool ok, const std::string& err,
                                                const GroupRequestInfo& req)>;
    using GroupReqListCb   = std::function<void(bool ok, const std::string& err,
                                                std::vector<GroupRequestInfo> reqs)>;
    // 群消息写扩散回调：带回 StoredMessage + 全群成员 userId（供在线直推）。
    using GroupSendCb      = std::function<void(bool ok, const std::string& err,
                                                const StoredMessage& msg,
                                                const std::vector<std::uint64_t>& memberIds)>;

    // BE-GROUP-02：建群。要求发起人提供至少一个初始成员；发起人自动成为群主。
    // 同一事务内：建 conversations(type=2) -> 建 groups -> 批量建群成员及会话成员。
    void createGroup(std::uint64_t owner, const std::string& name,
                     const std::vector<std::uint64_t>& memberIds, GroupOpCb cb);

    // BE-GROUP-05：我的群列表（JOIN 出 member_count / my_role）。
    void listMyGroups(std::uint64_t userId, GroupListCb cb);

    // BE-GROUP-05：群详情 + 成员列表。必须是成员才能看。
    void getGroupDetail(std::uint64_t groupId, std::uint64_t userId, GroupOpCb cb);

    // BE-GROUP-05：改群名/公告/简介/头像（仅群主/管理员）。
    void updateGroupInfo(std::uint64_t groupId, std::uint64_t actor,
                         const std::string& name, const std::string& announcement,
                         const std::string& intro, const std::string& avatarUrl,
                         GroupOpCb cb);

    // BE-GROUP-02：解散（仅群主）。软删 status=1，并把成员列表带回以便广播。
    void dismissGroup(std::uint64_t groupId, std::uint64_t actor, GroupOpCb cb);

    // BE-GROUP-02：退群（群主须先转让）。带回剩余成员列表用于广播 quit。
    void quitGroup(std::uint64_t groupId, std::uint64_t actor, GroupOpCb cb);

    // BE-GROUP-02/04：转让群主（仅群主）。旧群主降为管理员，新群主升为 role=1。
    void transferGroup(std::uint64_t groupId, std::uint64_t actor,
                       std::uint64_t newOwner, GroupOpCb cb);

    // BE-GROUP-03：邀请成员（群主/管理员）。已存在的成员会被跳过，不报错。
    void inviteMembers(std::uint64_t groupId, std::uint64_t actor,
                       const std::vector<std::uint64_t>& memberIds, GroupOpCb cb);

    // BE-GROUP-04：移出成员（群主/管理员移普通成员；不能移群主/同级）。
    void removeMember(std::uint64_t groupId, std::uint64_t actor,
                      std::uint64_t target, GroupOpCb cb);

    // BE-GROUP-04：设置/取消管理员（仅群主）。admin=true 把 role=3 升为 2，
    // admin=false 把 role=2 降为 3。
    void setAdmin(std::uint64_t groupId, std::uint64_t actor,
                  std::uint64_t target, bool admin, GroupOpCb cb);

    // BE-GROUP-03：申请入群（任何用户）。若群开启需审批则生成 status=0 记录。
    // MVP：申请写入后由群主/管理员审批；带回申请信息供推送。
    void createGroupRequest(std::uint64_t groupId, std::uint64_t requester,
                            const std::string& message, GroupReqCb cb);

    // BE-GROUP-03：审批入群申请（群主/管理员）。accept 时同事务写入群成员。
    // 一次回调同时带回 GroupRequestInfo（给申请人推结果事件）
    // 和 GroupOpResult（操作后成员列表，给全群推 added 事件）。
    using GroupReqOpCb = std::function<void(bool ok, const std::string& err,
                                            const GroupRequestInfo& req,
                                            const GroupOpResult& res)>;
    void handleGroupRequest(std::uint64_t requestId, std::uint64_t actor,
                            bool accept, GroupReqOpCb cb);

    // BE-GROUP-03：入群申请列表。direction：
    //   incoming=发给我审批的（我是群主/管理员的群），outgoing=我发出的，all=两者。
    void listGroupRequests(std::uint64_t userId, const std::string& direction,
                           GroupReqListCb cb);

    // 群消息写扩散：校验发送者是成员、群未解散，然后一个事务内
    // 分配 seq、插入 messages、给除发送者外所有成员写 offline_messages，
    // 回调带回 StoredMessage + 成员 userId 列表（在线成员由 TCP 层直推）。
    void sendGroupMessage(std::uint64_t sender, std::uint64_t groupId, int type,
                          const std::string& content, std::uint64_t clientMsgId,
                          GroupSendCb cb);

    // 群历史分页：校验请求者仍是成员，再按 conversation_id + seq keyset 分页。
    void fetchGroupHistory(std::uint64_t groupId, std::uint64_t userId,
                           std::uint64_t beforeSeq, int limit, MsgListCb cb);

    // BE-GROUP-06：登录补推——待我审批的入群申请（我是群主/管理员的群的 pending）。
    void fetchPendingGroupRequestsForApprover(std::uint64_t userId, int limit,
                                              GroupReqListCb cb);

    // BE-GROUP-06：登录补推——我发出的、已处理但还没通知我的入群申请结果。
    void fetchUnnotifiedGroupResults(std::uint64_t userId, int limit,
                                     GroupReqListCb cb);
    // 批量标记“我发出的入群申请结果”已通知（登录后补推）。
    void markGroupResultsNotified(std::uint64_t userId, StatusCb cb);
    // 精确标记某条入群申请结果已通知（申请人在线、已实时推送时）。
    void markGroupRequestNotified(std::uint64_t requestId, StatusCb cb);
    // /*************群聊功能：群 DAO***************/


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
