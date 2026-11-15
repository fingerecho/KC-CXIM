#include "ImsTypes.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include <cstdio>

std::string SerializeImsMessage(const ImsMessage& msg)
{
    std::string body = "{";
    body += "\"from\":" + std::to_string(msg.from);
    body += ",\"to\":" + std::to_string(msg.to);
    body += ",\"type\":" + std::to_string(msg.type);
    body += ",\"msg_id\":" + std::to_string(msg.msg_id);
    body += ",\"ts\":" + std::to_string(msg.ts);
    body += ",\"conversation_id\":" + std::to_string(msg.conversation_id);
    body += ",\"seq\":" + std::to_string(msg.seq);
    body += ",\"content\":" + ImsJsonEscape(msg.content);
    body += "}";
    return body;
}

bool ParseImsMessage(const std::string& body, ImsMessage& out, std::string& err)
{
    ImsJson root;
    if (!ImsJson::Parse(body, root) || !root.IsObject()) {
        err = "json parse error";
        return false;
    }
    out = ImsMessage();
    out.from = root["from"].AsUInt64();
    out.to = root["to"].AsUInt64();
    out.type = root["type"].AsInt();
    out.msg_id = root["msg_id"].AsUInt64();
    out.ts = root["ts"].AsUInt64();
    out.conversation_id = root["conversation_id"].AsUInt64();
    out.seq = root["seq"].AsUInt64();
    out.content = root["content"].AsStringOr(std::string());
    return true;
}

bool ParseImsHistory(const std::string& content, std::vector<ImsHistoryMessage>& out, std::string& err)
{
    ImsJson root;
    if (!ImsJson::Parse(content, root) || !root.IsArray()) {
        err = "history json is not array";
        return false;
    }
    for (size_t i = 0; i < root.Size(); ++i) {
        const ImsJson item = root.At(i);
        if (!item.IsObject()) {
            continue;
        }
        ImsHistoryMessage m;
        m.msg_id = item["msg_id"].AsUInt64();
        m.conversation_id = item["conversation_id"].AsUInt64();
        m.seq = item["seq"].AsUInt64();
        m.from = item["from"].AsUInt64();
        m.type = item["type"].AsInt(1);
        m.ts = item["ts"].AsUInt64();
        m.content = item["content"].AsStringOr(std::string());
        out.push_back(m);
    }
    return true;
}

std::string SearchUser::DisplayName() const
{
    if (!nickname.empty()) {
        return nickname;
    }
    return username;
}

std::string FriendInfo::DisplayName() const
{
    if (!remark.empty()) {
        return remark;
    }
    if (!nickname.empty()) {
        return nickname;
    }
    return username;
}

uint64_t FriendRequestInfo::PeerId(uint64_t selfId) const
{
    return (requesterId == selfId) ? targetId : requesterId;
}

std::string FriendRequestInfo::PeerName(uint64_t selfId) const
{
    if (requesterId == selfId) {
        return targetNickname.empty() ? targetUsername : targetNickname;
    }
    return requesterNickname.empty() ? requesterUsername : requesterNickname;
}

bool ParseImsFriendEvent(const std::string& content, ImsFriendEvent& out)
{
    ImsJson root;
    if (!ImsJson::Parse(content, root) || !root.IsObject()) {
        return false;
    }
    out.event = root["event"].AsStringOr("new");
    out.action = root["action"].AsStringOr(std::string());
    out.requestId = root["request_id"].AsUInt64();
    out.requesterId = root["requester_id"].AsUInt64();
    out.targetId = root["target_id"].AsUInt64();
    out.friendId = root["friend_id"].AsUInt64();
    out.requesterName = root["requester_nickname"].AsStringOr(root["requester_username"].AsStringOr(std::string()));
    if (out.requesterName.empty()) {
        out.requesterName = root["requester_username"].AsStringOr(std::string());
    }
    out.targetName = root["target_nickname"].AsStringOr(root["target_username"].AsStringOr(std::string()));
    if (out.targetName.empty()) {
        out.targetName = root["target_username"].AsStringOr(std::string());
    }
    out.remark = root["remark"].AsStringOr(std::string());
    out.message = root["message"].AsStringOr(std::string());
    out.status = root["status"].AsInt();
    out.ts = root["ts"].AsUInt64();
    return true;
}

bool ParseSearchUsers(const ImsJson& data, std::vector<SearchUser>& out)
{
    const ImsJson users = data["users"];
    if (!users.IsArray()) {
        return false;
    }
    for (size_t i = 0; i < users.Size(); ++i) {
        const ImsJson item = users.At(i);
        SearchUser user;
        user.userId = item["userId"].AsUInt64();
        user.username = item["username"].AsStringOr(std::string());
        user.nickname = item["nickname"].AsStringOr(std::string());
        user.avatarUrl = item["avatarUrl"].AsStringOr(std::string());
        out.push_back(user);
    }
    return true;
}

bool ParseFriendList(const ImsJson& data, std::vector<FriendInfo>& out)
{
    const ImsJson friends = data["friends"];
    if (!friends.IsArray()) {
        return false;
    }
    for (size_t i = 0; i < friends.Size(); ++i) {
        const ImsJson item = friends.At(i);
        FriendInfo friendInfo;
        friendInfo.userId = item["userId"].AsUInt64();
        friendInfo.username = item["username"].AsStringOr(std::string());
        friendInfo.nickname = item["nickname"].AsStringOr(std::string());
        friendInfo.avatarUrl = item["avatarUrl"].AsStringOr(std::string());
        friendInfo.remark = item["remark"].AsStringOr(std::string());
        friendInfo.status = item["status"].AsInt(1);
        out.push_back(friendInfo);
    }
    return true;
}

bool ParseOneFriendRequest(const ImsJson& requestJson, FriendRequestInfo& out)
{
    if (!requestJson.IsObject()) {
        return false;
    }
    out.requestId = requestJson["requestId"].AsUInt64();
    out.requesterId = requestJson["requesterId"].AsUInt64();
    out.targetId = requestJson["targetId"].AsUInt64();
    out.requesterUsername = requestJson["requesterUsername"].AsStringOr(std::string());
    out.requesterNickname = requestJson["requesterNickname"].AsStringOr(std::string());
    out.targetUsername = requestJson["targetUsername"].AsStringOr(std::string());
    out.targetNickname = requestJson["targetNickname"].AsStringOr(std::string());
    out.message = requestJson["message"].AsStringOr(std::string());
    out.status = requestJson["status"].AsInt();
    out.createdTs = requestJson["createdTs"].AsUInt64();
    return true;
}

bool ParseFriendRequests(const ImsJson& data, std::vector<FriendRequestInfo>& out)
{
    const ImsJson requests = data["requests"];
    if (!requests.IsArray()) {
        return false;
    }
    for (size_t i = 0; i < requests.Size(); ++i) {
        FriendRequestInfo info;
        if (ParseOneFriendRequest(requests.At(i), info)) {
            out.push_back(info);
        }
    }
    return true;
}

std::string UrlEncode(const std::string& value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (((uc >= 'A') && (uc <= 'Z')) || ((uc >= 'a') && (uc <= 'z')) ||
            ((uc >= '0') && (uc <= '9')) || (uc == '-') || (uc == '_') ||
            (uc == '.') || (uc == '~')) {
            result += ch;
        }
        else {
            result += '%';
            result += hex[uc >> 4];
            result += hex[uc & 0x0F];
        }
    }
    return result;
}

#endif // CXIM_IMS_CORE_SDK_VERSION
