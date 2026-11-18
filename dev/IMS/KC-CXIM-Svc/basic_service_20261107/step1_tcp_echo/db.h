#pragma once
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_config.h"
#include "friend_types.h"
#include "group_types.h"      // /*************群聊功能：群领域模型（GroupInfo 等）***************/
#include "message.h"

namespace sql { class Connection; }   // 不在头文件里暴露 Connector 细节

// ============================================================================
// Db：数据库访问层（连接池 + 双工作线程池 + DAO + 关系缓存）
// ----------------------------------------------------------------------------
// 为什么要线程池？
//   MariaDB Connector/C++ 是"同步阻塞"API。如果直接在 io_context 线程里
//   执行 SQL，一条慢查询会把所有网络收发卡住。因此：
//     1) 网络线程(io_context) 只负责收发和解析；
//     2) 需要查/写库时，把任务丢进本类的 worker 线程池；
//     3) worker 从连接池借一条连接执行 SQL；
//     4) 结果通过 boost::asio::post 回到 io_context，再回调业务方。
//
// /*************P0-2：CPU 型任务与阻塞型任务分池（消除队头阻塞）***************/
// 原来只有"一条 FIFO + 一组 worker"：注册/登录里的 bcrypt（纯 CPU，几十毫秒）
// 和所有 SQL（阻塞 IO）挤在同一条队列里。100 个并发注册会把 worker 全部占住，
// 于是"发消息""查好友列表"这些根本不碰 bcrypt 的请求也要排在 100 个哈希任务
// 之后 —— 典型队头阻塞。现在拆成两个独立队列 + 两组线程：
//   · sqlPool ：借连接执行 SQL，线程数 = DB_WORKER_COUNT；
//   · hashPool：只算 bcrypt，不持有任何数据库连接，线程数 = DB_HASH_WORKER_COUNT
//               （默认按核数收敛，避免 8 个线程抢 2 个核、把 mariadbd 饿死）。
// 注册/登录因此被拆成两段："算哈希"(hashPool) -> "写/读库"(sqlPool)，
// 哈希期间不再长时间霸占一条 DB 连接。
// /*************P0-2：CPU 型任务与阻塞型任务分池（消除队头阻塞）***************/
//
// 连接池：
//   用一个固定大小的空闲队列 + 互斥锁/条件变量实现。借出的连接用
//   PooledConnection(RAII) 包装，析构时自动归还，异常路径也不会泄漏。
//   /*************P0-6：健康检查降频***************/
//   不再"每次借出/归还都 isValid() ping 一次"（那会给每个 DB 任务凭空加上
//   2 个网络往返）。改为：空闲超过 kConnPingIdle 的连接才 ping；归还时只有
//   真正开过事务的连接才重置 autocommit。坏连接靠 SQL 抛错 + 重连兜底。
//   /*************P0-6：健康检查降频***************/
// ============================================================================

namespace kc {

class ConnectionPool;   // 定义在 db.cpp 内部

// ============================================================================
// DbRuntime：Db 的运行期参数（全部可用环境变量覆盖，见 DbRuntime::fromEnv）
// ----------------------------------------------------------------------------
// 相关环境变量：
//   BCRYPT_COST        bcrypt 成本因子（默认 10，允许区间 8~15；P0-1）
//   DB_WORKER_COUNT    SQL worker 线程数（默认 = 核数*2，上限 8）
//   DB_POOL_SIZE       连接池上限（默认与 SQL worker 数一致；P0-7）
//   DB_HASH_WORKER_COUNT 哈希 worker 线程数（默认 = 核数；P0-1/P0-2）
//   DB_QUEUE_LIMIT     单条任务队列的最大深度（默认 2000；P0-4 背压）
//   OFFLINE_CURSOR     1=用游标补推，不再写 offline_messages 行级队列（P1-4）
//   RELATION_CACHE_TTL 关系缓存毫秒（默认 10000；0=关闭缓存；P1-1）
//   RELATION_CACHE_MAX 关系缓存条目上限（默认 20000）
// ============================================================================
struct DbRuntime {
    int         bcryptCost       = 10;      // P0-1：原为写死的 12
    std::size_t sqlWorkers       = 4;       // P0-2/P0-7
    std::size_t hashWorkers      = 2;       // P0-1/P0-2：默认=核数
    std::size_t poolSize         = 4;       // P0-7：与 sqlWorkers 对齐才有意义
    std::size_t queueLimit       = 2000;    // P0-4：超过即快速失败（503）
    bool        offlineCursor    = false;   // P1-4
    long long   relationCacheTtlMs = 10000; // P1-1
    std::size_t relationCacheMax   = 20000; // P1-1

    // 从环境变量读取；非法值回落到默认。纯 CPU 计算，启动时调用一次。
    static DbRuntime fromEnv();
};

class Db {
public:
    /*************P0-4：过载约定***************/
    // 过载时 DAO 统一用它作为 err 文案，HTTP 层据此回 503 + Retry-After，
    // TCP 层用它回 Error 帧。上层判重只需要比较这个字符串。
    static const char* kBusyError;

    // 观测用（P2-1）：两条队列各自的在途深度。入口层的过载判断也读它。
    std::size_t sqlQueueDepth();
    std::size_t hashQueueDepth();
    /*************P0-4：过载约定***************/

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
    // BE-SEND-01：发送前的关系校验不再是独立 DAO ——
    // 它已并入 Db::sendChecked（P1-2），并在其中使用 P1-1 的关系缓存。
    // 独立的 areFriends() 会让同一段 SQL 存在两份实现、迟早漂移，故删除。

    /*************P0-2/P0-7：运行期参数取代原来的 pool_size/worker_count***************/
    // rt.poolSize     连接池上限（建议与 rt.sqlWorkers 一致：同时被占用的连接
    //                 不可能多于 SQL worker 数，配大了只会误导读代码的人）；
    // rt.sqlWorkers   执行 SQL 的线程数；
    // rt.hashWorkers  执行 bcrypt 的线程数（= 核数，超订不会提高吞吐）。
    Db(boost::asio::io_context& io, const DbConfig& cfg, const DbRuntime& rt);
    /*************P0-2/P0-7：运行期参数取代原来的 pool_size/worker_count***************/
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
    //   6) 给接收者写离线队列（OFFLINE_CURSOR=1 时跳过，见 P1-4）；
    //   7) 更新发送者已读游标。
    void sendMessage(std::uint64_t sender, std::uint64_t to, int type,
                     const std::string& content, std::uint64_t clientMsgId,
                     SendCb cb);

    /*************P1-2：关系校验 + 落库合并成一个任务（原为两段串行回调）***************/
    // 单聊发送的热路径入口。与 areFriends()+sendMessage() 分离调用的差别：
    //   * 少一次"线程池往返 + strand 往返 + 大对象拷贝"（原来 content/StoredMessage
    //     在两层 lambda 里各拷贝一次）；
    //   * 关系校验与消息落库在同一事务内完成，消除"校验通过后对方被删/拉黑"的
    //     TOCTOU 窗口（命中 P1-1 缓存时仍有最长 TTL 的弱一致窗口，已接受并记录）；
    //   * 关系判定命中缓存时，整条链路只剩一次落库事务。
    // notFriends/blocked 用于让上层给出区分开的错误文案。
    using SendCheckedCb = std::function<void(bool ok, const std::string& err,
                                             const StoredMessage& msg,
                                             bool notFriends, bool blocked)>;
    void sendChecked(std::uint64_t sender, std::uint64_t to, int type,
                     const std::string& content, std::uint64_t clientMsgId,
                     SendCheckedCb cb);
    /*************P1-2：关系校验 + 落库合并成一个任务***************/

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


    /*************P1-3 之后这几个"单类补查"DAO 的定位***************/
    // 在线登录链路已经改走 fetchLoginSync（一次任务取完），因此
    // fetchPendingIncoming / fetchUnnotifiedResults 以及上面的两个群补查
    // 目前没有任何生产调用。保留它们的原因不是"以后可能用"，而是：
    //   * 它们是纯读接口，不参与关系缓存的失效，留着不会产生一致性风险；
    //   * verify_*.sh 之类的排查脚本需要一个能单独看"某类事件"的入口。
    // 反过来，写路径的旧入口（areFriends）已经删除——同一段判定 SQL 留两份
    // 实现迟早漂移，而漂移的后果是"缓存说你是好友、库说不是"这种难查问题。
    void fetchPendingIncoming(std::uint64_t userId, int limit, RequestListCb cb);

    // BE-FRIEND-06：登录时补推“已处理但申请人还没收到结果”的申请。
    void fetchUnnotifiedResults(std::uint64_t userId, int limit, RequestListCb cb);

    // BE-FRIEND-06：把补推过的结果标记为已通知，避免每次登录重复推送。
    void markResultsNotified(std::uint64_t userId, StatusCb cb);

    // BE-FRIEND-06：申请人在线、结果已实时推送时，精确标记这一条申请已通知。
    // 与 markResultsNotified() 的区别：这里只动指定的申请，不会误伤其他记录。
    void markRequestResultNotified(std::uint64_t requestId, StatusCb cb);

    // 批量创建好友申请（P1-7）：同一 requester 对多个目标，一个事务、一次多行 INSERT。
    // 语义与单条 createFriendRequest 不同，是"尽力而为"：
    //   成功项进 reqs；被跳过项（非好友校验失败/已存在申请/目标不存在）进 skipped。
    using RequestBatchCb = std::function<void(bool ok, const std::string& err,
                                              std::vector<FriendRequestInfo> reqs,
                                              std::vector<std::pair<std::uint64_t, std::string>> skipped)>;
    void createFriendRequestsBatch(std::uint64_t requesterId,
                                   std::vector<std::uint64_t> targetIds,
                                   const std::string& message, RequestBatchCb cb);

    // 批量删除好友（P1-7）：一个事务内两条多行 DELETE。
    // 回调带回 removed = 本次真正从"我的好友列表"里消失的 id 列表，
    // HTTP 层据此逐个推 FriendListChanged（与单条删除一致），并把它作为
    // 响应里的 deleted 计数——**不能**拿 DELETE 的 affected rows 当人数，
    // 那是双向两行、还可能包含单向拉黑行。
    using RemovedFriendsCb = std::function<void(bool ok, const std::string& err,
                                                std::vector<std::uint64_t> removed)>;
    void deleteFriendsBatch(std::uint64_t userId,
                            std::vector<std::uint64_t> friendIds, RemovedFriendsCb cb);

    /*************P1-3：登录补推一次性取回（原来 5~7 个 DB 任务 -> 1 个）***************/
    struct LoginSync {
        std::vector<StoredMessage>     offline;        // 待补推消息（已按 limit 截断）
        bool                           offlineMore = false;  // 还有更多，需继续拉取
        std::vector<FriendRequestInfo> pendingRequests;      // 待我处理的申请
        std::vector<FriendRequestInfo> requestResults;       // 我发出、尚未通知的结果
        std::vector<GroupRequestInfo>  pendingGroupRequests; // 待我审批的入群申请
        std::vector<GroupRequestInfo>  groupResults;         // 我发出、尚未通知的入群结果
    };
    using LoginSyncCb = std::function<void(bool ok, const std::string& err, LoginSync sync)>;
    // offlineLimit：单次登录补推的离线消息条数（TCP 侧传 session::kLoginOfflineBatch=100）。
    // 内部会多取一条来判定 offlineMore，所以返回的条数最多就是 offlineLimit。
    void fetchLoginSync(std::uint64_t userId, int offlineLimit, LoginSyncCb cb);
    /*************P1-3：登录补推一次性取回***************/

private:
    /*************P0-2/P0-4：双队列 + 队列深度上限***************/
    // 一条队列一个 worker 组；submit* 返回 false 表示队列已满（过载，P0-4）。
    struct JobQueue {
        std::mutex                        mutex;
        std::condition_variable           cv;
        std::queue<std::function<void()>> jobs;
        bool                              stopping = false;
    };

    // 硬上限投递口：队列已满就返回 false，调用方**必须**立刻用回调回 kBusyError。
    // 用它的都是热路径：注册、登录、单聊发送（sendChecked）、群发送、历史翻页、
    // 好友申请/删除（含批量）、登录补推、回执。
    bool submitSql(JobQueue& q, std::function<void()> job);   // 投递 SQL 任务
    bool submitHash(std::function<void()> job);              // 投递 bcrypt 任务

    // /*************软上限投递口：为什么"不能丢任务"***************/
    // 还有一批 DAO 走这条（群资料/成员管理、用户搜索、申请列表、备注、
    // 各类"标记已通知"）。它们的回调签名各不相同，投递口无法统一回一个"忙"错误。
    // 一旦丢任务，后果不只是客户端干等到超时——session 侧已经 reserveJob()
    // 预占的配额是在**回调里**才 releaseJob() 的，回调不跑就会永久漏配额，
    // 那条连接之后会一直报 "server busy"。所以这里超阈值只告警、不丢弃。
    //
    // 代价与配套（重要）：这些任务不受 DB_QUEUE_LIMIT 约束，会和热路径任务挤在
    // 同一条 sqlQueue_ 里，于是"队列深度"这个指标本身不能保证有界。真正的有界性来自两处：
    //   1) 入口层允许的请求数（HTTP 在途上限 + 认证限流 + 队列深度准入，见
    //      http_server.cpp 的 do_read；TCP 连接数上限 + 每连接在途任务上限）；
    //   2) 每个已准入的请求最多贡献常数个任务。
    // 所以队列深度是"准入上限 × 常数"，而不是无界。DB_QUEUE_LIMIT 的作用退化成
    // 热路径的**快速失败阈值**和"该收紧准入参数"的告警信号。
    void submit(std::function<void()> job);                  // 软上限（内部转 submitAlways）
    void submitAlways(std::function<void()> job);            // 直接入队，绝不丢
    void workerLoop(JobQueue& q);                            // worker 主循环
    void postToIo(std::function<void()> fn);                 // 回到 io_context 线程

    /*************P1-1：好友关系缓存（发消息前置校验的热点）***************/
    // key=(小uid,大uid)；value=双向两行的 status + 过期时间。
    // 关系变更极低频（通过申请/删除/拉黑），因此：
    //   * 命中直接返回，省掉"1 个 DB 任务 + 1 条 SQL"；
    //   * 在 handleFriendRequest(accept)/deleteFriend/setFriendBlocked 三处显式失效；
    //   * TTL 到期自动失效，兜住"多实例部署时本地缓存收不到失效通知"的情况。
    using RelationKey     = std::pair<std::uint64_t, std::uint64_t>;  // (min,max)
    /*************core-dump 修复 ⑩：LRU 只留一张索引***************/
    // 本轮栈顶是 libstdc++ 的 `_List_node_base::_M_unhook()`（写 prev/next 时打到
    // 0x33）—— 全工程只有 relLru_ 这一个 std::list，所以踩坏的就是关系缓存的 LRU 节点。
    // 旧写法用**第二张表** relLruIters_ 存 "key -> list iterator"，于是每次增删都必须
    // 同时维护 relMap_ / relLru_ / relLruIters_ 三张表；只要有一条路径少删一张，
    // relLru_.erase(<悬垂 iterator>) 就是这种栈顶。iterator 现在**放进 entry 自己**，
    // "两个索引不同步"这件事在类型上就不存在了；再配一条不变量自检（见 .cpp）。
    using RelationLruList = std::list<RelationKey>;
    using RelationLruIter = RelationLruList::iterator;
    struct RelationEntry {
        int  forwardStatus = -1;         // sender -> to 行的 status（-1=无行）
        int  reverseStatus = -1;         // to -> sender 行的 status
        std::chrono::steady_clock::time_point expire{};
        RelationLruIter lruIt;           // 自己在 relLru_ 里的位置（lruLinked 为真时有效）
        bool            lruLinked = false;
    };
    /*************core-dump 修复 ⑩：LRU 只留一张索引***************/
    struct RelationKeyHash {
        std::size_t operator()(const RelationKey& k) const {
            std::size_t h1 = std::hash<std::uint64_t>{}(k.first);
            std::size_t h2 = std::hash<std::uint64_t>{}(k.second);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };
    bool relationCacheGet(std::uint64_t a, std::uint64_t b,
                          int& forwardStatus, int& reverseStatus);
    // epochAtRead 必须取自"执行那条 SELECT 之前"的 relationCacheEpoch()：
    // 期间只要发生过一次 erase，这次读到的结果就不再写入缓存。
    void relationCachePut(std::uint64_t a, std::uint64_t b,
                          int forwardStatus, int reverseStatus,
                          std::uint64_t epochAtRead);
    void relationCacheErase(std::uint64_t a, std::uint64_t b);
    std::uint64_t relationCacheEpoch() const;

    using RelationMap     = std::unordered_map<RelationKey, RelationEntry, RelationKeyHash>;
    using RelationMapIter = RelationMap::iterator;
    /*************core-dump 修复 ⑪：删除只有一个出口 + 结构自检***************/
    // dropLocked：唯一允许 erase LRU 节点的地方（调用方必须已持 relMu_）。
    // relCacheSaneLocked：每把锁第一件事就是跑一次；两张容器大小不等 = 缓存结构已被
    // 破坏，此时**绝不**再拿任何 iterator 去 erase（那正是本轮 _M_unhook 的现场），
    // 而是 O(1) 换掉整张表、放弃遍历它，并打一条永不被静音的 [fatal]。
    void relationCacheDropLocked(RelationMapIter it);
    bool relationCacheSaneLocked(const char* where);

    boost::asio::io_context&        io_;
    DbRuntime                       rt_;
    std::unique_ptr<ConnectionPool> pool_;

    JobQueue                         sqlQueue_;
    JobQueue                         hashQueue_;
    std::vector<std::thread>         sqlWorkers_;
    std::vector<std::thread>         hashWorkers_;

    std::mutex                                          relMu_;
    // 修复⑩：只剩两张容器，且 iterator 存在 entry 里 —— relMap_ 有的 key，
    // 其 entry.lruIt 一定指向 relLru_ 中的活节点（lruLinked 标记），反之亦然。
    RelationMap                                         relMap_;
    RelationLruList                                     relLru_;      // _front=最近使用
    // 失效版本号：每次 erase 递增。读侧带着它写回，避免"旧读覆盖新失效"。
    std::atomic<std::uint64_t>                          relEpoch_{0};
};

} // namespace kc
