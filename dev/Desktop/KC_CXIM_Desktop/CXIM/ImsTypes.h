#ifndef CXIM_IMS_TYPES_H_
#define CXIM_IMS_TYPES_H_

#include "CXIMAppConfig.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include "ImsJson.h"

#include <cstdint>
#include <string>
#include <vector>

/** TCP 消息类型，与服务端 step1_tcp_echo/message.h 的 MsgType 保持一致
*/
enum class ImsMsgType : int {
    Text                = 1,
    Image               = 2,
    Heartbeat           = 3,
    Login               = 100,
    LoginAck            = 101,
    Error               = 102,
    HistoryReq          = 110,
    HistoryResp         = 111,
    Ack                 = 112,
    SendAck             = 113,
    FriendRequest       = 130,
    FriendRequestResult = 131,
    FriendListChanged   = 132,

    /** 群聊下行事件（type=140~144，客户端只收不发，与 130~132 同理）
     *  对照服务端 message.h 的 MsgType / group_types.h 的 serialize_group_*
     */
    GroupMemberChanged  = 140,   // 群成员变化：added/removed/quit/roleChanged/transferred
    GroupDismissed      = 141,   // 群被解散
    GroupInfoChanged    = 142,   // 群资料变化（群名/公告/简介/头像）
    GroupRequest        = 143,   // 入群申请（event=new|pending，推给群主/管理员）
    GroupRequestResult  = 144,   // 入群申请结果（event=accepted|rejected，推给申请人）
};

/** 一条 TCP 消息，字段与服务端 message.h 的 Message 保持一致
*/
struct ImsMessage
{
    uint64_t from = 0;
    uint64_t to = 0;
    int type = 0;
    uint64_t msg_id = 0;
    uint64_t ts = 0;
    uint64_t conversation_id = 0;
    uint64_t seq = 0;
    std::string content;
    /** true=群聊（to 为 groupId，走群消息逻辑）；false=单聊（缺省，向后兼容旧协议）
     */
    bool is_group = false;
};

std::string SerializeImsMessage(const ImsMessage& msg);
bool ParseImsMessage(const std::string& body, ImsMessage& out, std::string& err);

/** 历史消息条目（HistoryResp.content 里的 JSON 数组元素）
*/
struct ImsHistoryMessage
{
    uint64_t msg_id = 0;
    uint64_t conversation_id = 0;
    uint64_t seq = 0;
    uint64_t from = 0;
    int type = 1;
    uint64_t ts = 0;
    std::string content;
    /** 历史项同样带 is_group（群会话消息为 true），离线/分页消息可识别群聊
     */
    bool is_group = false;
};

bool ParseImsHistory(const std::string& content, std::vector<ImsHistoryMessage>& out, std::string& err);

/** 用户搜索条目：GET /api/users/search
*/
struct SearchUser
{
    uint64_t userId = 0;
    std::string username;
    std::string nickname;
    std::string avatarUrl;

    std::string DisplayName() const;
};

/** 好友条目：GET /api/friends
*/
struct FriendInfo
{
    uint64_t userId = 0;
    std::string username;
    std::string nickname;
    std::string avatarUrl;
    std::string remark;
    int status = 1;

    std::string DisplayName() const;
};

/** 好友申请条目：GET /api/friends/requests、POST /api/friends/requests
*/
struct FriendRequestInfo
{
    uint64_t requestId = 0;
    uint64_t requesterId = 0;
    uint64_t targetId = 0;
    std::string requesterUsername;
    std::string requesterNickname;
    std::string targetUsername;
    std::string targetNickname;
    std::string message;
    int status = 0;
    uint64_t createdTs = 0;

    /** 对于当前登录用户 selfId 而言，对方（申请人或被申请人）的 userId */
    uint64_t PeerId(uint64_t selfId) const;
    std::string PeerName(uint64_t selfId) const;
};

/** 好友事件（type=130/131/132）content 里的 JSON 字段集合
*/
struct ImsFriendEvent
{
    std::string event;      // new / pending / accepted / rejected / friend_list_changed
    std::string action;     // 仅 132：added / deleted / updated / blocked / unblocked
    uint64_t requestId = 0;
    uint64_t requesterId = 0;
    uint64_t targetId = 0;
    uint64_t friendId = 0;
    std::string requesterName;
    std::string targetName;
    std::string remark;
    std::string message;
    int status = 0;
    uint64_t ts = 0;
};

bool ParseImsFriendEvent(const std::string& content, ImsFriendEvent& out);

/** 从 HTTP 统一响应 {code,message,data} 中解析 data 并填充数组的辅助函数
*/
bool ParseSearchUsers(const ImsJson& data, std::vector<SearchUser>& out);
bool ParseFriendList(const ImsJson& data, std::vector<FriendInfo>& out);
bool ParseFriendRequests(const ImsJson& data, std::vector<FriendRequestInfo>& out);
bool ParseOneFriendRequest(const ImsJson& requestJson, FriendRequestInfo& out);

// ===========================================================================
// 群聊领域模型（字段严格对照服务端 group_types.h 的 write_group_json /
// write_member_json / write_request_json 与 readme §5.13 的 HTTP 响应，小驼峰）
// ===========================================================================

/** 群资料：data.group / data.groups[] 的元素
*/
struct ImsGroupInfo
{
    uint64_t groupId = 0;
    uint64_t conversationId = 0;
    std::string name;
    std::string announcement;
    std::string intro;
    std::string avatarUrl;
    uint64_t ownerId = 0;
    int status = 0;          // 0=正常，1=已解散
    int memberLimit = 0;
    int memberCount = 0;
    int myRole = 0;          // 1=群主，2=管理员，3=成员，0=非成员
    uint64_t createdTs = 0;
};

/** 群成员：data.members[] 的元素
*/
struct ImsGroupMember
{
    uint64_t userId = 0;
    std::string username;
    std::string nickname;
    std::string avatarUrl;
    std::string groupNickname;
    int role = 3;
    uint64_t joinedTs = 0;

    /** 群内昵称 > 昵称 > 用户名 优先 */
    std::string DisplayName() const;
};

/** 入群申请：data.request / data.requests[] 的元素，以及 143/144 事件内嵌对象
*/
struct ImsGroupRequestInfo
{
    uint64_t requestId = 0;
    uint64_t groupId = 0;
    std::string groupName;
    uint64_t ownerId = 0;
    uint64_t requesterId = 0;
    std::string requesterUsername;
    std::string requesterNickname;
    uint64_t handlerId = 0;
    std::string message;
    int status = 0;          // 0=待处理，1=已通过，2=已拒绝，3=已取消
    uint64_t createdTs = 0;

    /** 申请人展示名（昵称 > 用户名） */
    std::string RequesterName() const;
};

/** 群事件（type=140/141/142）content 里的下划线风格字段集合
 *  （对照 group_types.h 的 serialize_group_member_changed / _dismissed / _info_changed）
 */
struct ImsGroupEvent
{
    std::string event;      // group_member_changed / group_dismissed / group_info_changed
    std::string action;     // 仅 140：added / removed / quit / roleChanged / transferred
    uint64_t groupId = 0;
    uint64_t userId = 0;    // 仅 140：被操作的成员
    int role = 0;           // 仅 140：操作后的角色（removed/quit 填 0）
    uint64_t operatorId = 0;
    std::string name;       // 141/142：群名
};

/** type=140/141/142 事件解析（下划线风格字段）
*/
bool ParseImsGroupEvent(const std::string& content, ImsGroupEvent& out);

/** type=143/144 事件解析：外层 event（new/pending/accepted/rejected）+ 内嵌小驼峰申请对象。
 *  内嵌对象既可能是与 event 同级的扁平字段，也可能被包成子对象——两种写法都兼容。
 */
bool ParseImsGroupRequestEvent(const std::string& content, std::string& event, ImsGroupRequestInfo& out);

/** 从 HTTP data 节点解析群相关结构 */
bool ParseGroupInfo(const ImsJson& json, ImsGroupInfo& out);
bool ParseGroupList(const ImsJson& data, std::vector<ImsGroupInfo>& out);
bool ParseGroupMembers(const ImsJson& data, std::vector<ImsGroupMember>& out);
bool ParseOneGroupRequest(const ImsJson& json, ImsGroupRequestInfo& out);
bool ParseGroupRequests(const ImsJson& data, std::vector<ImsGroupRequestInfo>& out);

/** 群解析函数的最小自测入口（用服务端实际下发的样例字符串验证），
 *  返回值 0=全部通过，非 0=失败项数。main 之外的独立程序可直接调用。
 */
int ImsGroupParseSelfTest();

/** URL 查询串编码（application/x-www-form-urlencoded）
*/
std::string UrlEncode(const std::string& value);

#endif // CXIM_IMS_CORE_SDK_VERSION

#endif // CXIM_IMS_TYPES_H_
