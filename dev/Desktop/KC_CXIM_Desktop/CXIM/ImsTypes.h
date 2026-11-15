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

/** URL 查询串编码（application/x-www-form-urlencoded）
*/
std::string UrlEncode(const std::string& value);

#endif // CXIM_IMS_CORE_SDK_VERSION

#endif // CXIM_IMS_TYPES_H_
