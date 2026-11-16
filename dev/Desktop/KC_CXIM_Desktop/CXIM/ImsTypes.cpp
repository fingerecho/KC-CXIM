#include "ImsTypes.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include <cstdio>

namespace {

/** 兼容 bool 与 int(0/1) 两种写法的可选布尔读取，缺省返回 defaultValue
 */
bool ReadOptionalBool(const ImsJson& obj, const char* key, bool defaultValue)
{
    if (!obj.Has(key)) {
        return defaultValue;
    }
    const ImsJson v = obj[key];
    if (v.GetType() == ImsJson::Type::Bool) {
        return v.AsBool();
    }
    if (v.IsNumber()) {
        return v.AsUInt64() != 0;
    }
    return defaultValue;
}

} // namespace

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
    // 与服务端 message_codec.h 对应：始终写出 is_group（true/false）
    body += ",\"is_group\":";
    body += msg.is_group ? "true" : "false";
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
    // 可选 is_group（兼容 bool 与 int 0/1，缺省 false=单聊）
    out.is_group = ReadOptionalBool(root, "is_group", false);
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
        // 历史项同样解析 is_group（群会话消息为 true）
        m.is_group = ReadOptionalBool(item, "is_group", false);
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

// ---------------------------------------------------------------------------
// 群聊：领域模型展示名
// ---------------------------------------------------------------------------
std::string ImsGroupMember::DisplayName() const
{
    if (!groupNickname.empty()) {
        return groupNickname;
    }
    if (!nickname.empty()) {
        return nickname;
    }
    return username;
}

std::string ImsGroupRequestInfo::RequesterName() const
{
    return requesterNickname.empty() ? requesterUsername : requesterNickname;
}

// ---------------------------------------------------------------------------
// 群事件（type=140/141/142）：content 为下划线风格 JSON 字符串（二次解析）
//   140 {event:"group_member_changed", action, group_id, user_id, role, operator_id}
//   141 {event:"group_dismissed", group_id, operator_id, name}
//   142 {event:"group_info_changed", group_id, operator_id, name}
// ---------------------------------------------------------------------------
bool ParseImsGroupEvent(const std::string& content, ImsGroupEvent& out)
{
    ImsJson root;
    if (!ImsJson::Parse(content, root) || !root.IsObject()) {
        return false;
    }
    out.event = root["event"].AsStringOr(std::string());
    out.action = root["action"].AsStringOr(std::string());
    out.groupId = root["group_id"].AsUInt64();
    out.userId = root["user_id"].AsUInt64();
    out.role = root["role"].AsInt();
    out.operatorId = root["operator_id"].AsUInt64();
    out.name = root["name"].AsStringOr(std::string());
    return true;
}

// ---------------------------------------------------------------------------
// 入群申请事件（type=143/144）：外层 event + 小驼峰申请对象（复用 write_request_json）。
// 服务端 serialize_group_request_event 把申请对象“内嵌”在 event 对象里；
// 为兼容内嵌子对象 / 同级扁平两种写法，先解析整段，若根节点没有 requestId
// 再取第一个对象型成员作为内嵌申请对象。
// ---------------------------------------------------------------------------
bool ParseImsGroupRequestEvent(const std::string& content, std::string& event, ImsGroupRequestInfo& out)
{
    ImsJson root;
    if (!ImsJson::Parse(content, root) || !root.IsObject()) {
        return false;
    }
    event = root["event"].AsStringOr(std::string());

    const ImsJson* requestNode = &root;
    ImsJson nested;
    if (!root.Has("requestId")) {
        // 尝试从内嵌子对象取（例如 request/data/或任意 object 成员）
        const char* candidates[] = { "request", "data", "req" };
        bool found = false;
        for (const char* key : candidates) {
            nested = root[key];
            if (nested.IsObject() && nested.Has("requestId")) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        requestNode = &nested;
    }
    return ParseOneGroupRequest(*requestNode, out);
}

// ---------------------------------------------------------------------------
// HTTP data 解析
// ---------------------------------------------------------------------------
bool ParseGroupInfo(const ImsJson& json, ImsGroupInfo& out)
{
    if (!json.IsObject()) {
        return false;
    }
    out = ImsGroupInfo();
    out.groupId = json["groupId"].AsUInt64();
    out.conversationId = json["conversationId"].AsUInt64();
    out.name = json["name"].AsStringOr(std::string());
    out.announcement = json["announcement"].AsStringOr(std::string());
    out.intro = json["intro"].AsStringOr(std::string());
    out.avatarUrl = json["avatarUrl"].AsStringOr(std::string());
    out.ownerId = json["ownerId"].AsUInt64();
    out.status = json["status"].AsInt();
    out.memberLimit = json["memberLimit"].AsInt();
    out.memberCount = json["memberCount"].AsInt();
    out.myRole = json["myRole"].AsInt();
    out.createdTs = json["createdTs"].AsUInt64();
    return true;
}

bool ParseGroupList(const ImsJson& data, std::vector<ImsGroupInfo>& out)
{
    const ImsJson groups = data["groups"];
    if (!groups.IsArray()) {
        return false;
    }
    for (size_t i = 0; i < groups.Size(); ++i) {
        ImsGroupInfo info;
        if (ParseGroupInfo(groups.At(i), info)) {
            out.push_back(info);
        }
    }
    return true;
}

bool ParseGroupMembers(const ImsJson& data, std::vector<ImsGroupMember>& out)
{
    const ImsJson members = data["members"];
    if (!members.IsArray()) {
        return false;
    }
    for (size_t i = 0; i < members.Size(); ++i) {
        const ImsJson item = members.At(i);
        ImsGroupMember m;
        m.userId = item["userId"].AsUInt64();
        m.username = item["username"].AsStringOr(std::string());
        m.nickname = item["nickname"].AsStringOr(std::string());
        m.avatarUrl = item["avatarUrl"].AsStringOr(std::string());
        m.groupNickname = item["groupNickname"].AsStringOr(std::string());
        m.role = item["role"].AsInt(3);
        m.joinedTs = item["joinedTs"].AsUInt64();
        out.push_back(m);
    }
    return true;
}

bool ParseOneGroupRequest(const ImsJson& json, ImsGroupRequestInfo& out)
{
    if (!json.IsObject()) {
        return false;
    }
    out = ImsGroupRequestInfo();
    out.requestId = json["requestId"].AsUInt64();
    out.groupId = json["groupId"].AsUInt64();
    out.groupName = json["groupName"].AsStringOr(std::string());
    out.ownerId = json["ownerId"].AsUInt64();
    out.requesterId = json["requesterId"].AsUInt64();
    out.requesterUsername = json["requesterUsername"].AsStringOr(std::string());
    out.requesterNickname = json["requesterNickname"].AsStringOr(std::string());
    out.handlerId = json["handlerId"].AsUInt64();
    out.message = json["message"].AsStringOr(std::string());
    out.status = json["status"].AsInt();
    out.createdTs = json["createdTs"].AsUInt64();
    return true;
}

bool ParseGroupRequests(const ImsJson& data, std::vector<ImsGroupRequestInfo>& out)
{
    const ImsJson requests = data["requests"];
    if (!requests.IsArray()) {
        return false;
    }
    for (size_t i = 0; i < requests.Size(); ++i) {
        ImsGroupRequestInfo info;
        if (ParseOneGroupRequest(requests.At(i), info)) {
            out.push_back(info);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 最小自测入口：用服务端实际下发的样例字符串验证解析（不参与 GUI 构建）
// ---------------------------------------------------------------------------
int ImsGroupParseSelfTest()
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char* name) {
        if (!ok) {
            ++failures;
            std::printf("[FAIL] %s\n", name);
        }
        else {
            std::printf("[PASS] %s\n", name);
        }
    };

    // 1) is_group 双向 + 兼容 int 写法
    {
        ImsMessage m;
        m.from = 1001; m.to = 7; m.type = 1; m.msg_id = 5; m.content = "hi"; m.is_group = true;
        const std::string j = SerializeImsMessage(m);
        check(j.find("\"is_group\":true") != std::string::npos, "SerializeImsMessage 写出 is_group=true");
        ImsMessage p; std::string err;
        check(ParseImsMessage(j, p, err) && p.is_group, "ParseImsMessage 读回 is_group=true");
        ImsMessage pi;
        check(ParseImsMessage("{\"from\":1,\"type\":1,\"content\":\"x\",\"is_group\":1}", pi, err) && pi.is_group,
              "ParseImsMessage 兼容 int is_group=1");
        ImsMessage pd;
        check(ParseImsMessage("{\"from\":1,\"type\":1,\"content\":\"x\"}", pd, err) && !pd.is_group,
              "ParseImsMessage 缺省 is_group=false");
    }

    // 2) type=140 群成员变化（下划线风格）
    {
        ImsGroupEvent e;
        const bool ok = ParseImsGroupEvent(
            "{\"event\":\"group_member_changed\",\"action\":\"added\",\"group_id\":5,"
            "\"user_id\":3587,\"role\":3,\"operator_id\":3586}", e);
        check(ok && e.event == "group_member_changed" && e.action == "added" &&
                  e.groupId == 5 && e.userId == 3587 && e.role == 3 && e.operatorId == 3586,
              "ParseImsGroupEvent 140 added");
    }

    // 3) type=141 解散
    {
        ImsGroupEvent e;
        const bool ok = ParseImsGroupEvent(
            "{\"event\":\"group_dismissed\",\"group_id\":8,\"operator_id\":3593,\"name\":\"q_x\"}", e);
        check(ok && e.event == "group_dismissed" && e.groupId == 8 && e.name == "q_x",
              "ParseImsGroupEvent 141 dismissed");
    }

    // 4) type=142 资料变更
    {
        ImsGroupEvent e;
        const bool ok = ParseImsGroupEvent(
            "{\"event\":\"group_info_changed\",\"group_id\":8,\"operator_id\":1001,\"name\":\"新群名\"}", e);
        check(ok && e.event == "group_info_changed" && e.groupId == 8 && e.name == "新群名",
              "ParseImsGroupEvent 142 info_changed");
    }

    // 5) type=143 入群申请（内嵌小驼峰，扁平写法）
    {
        std::string event; ImsGroupRequestInfo info;
        const bool ok = ParseImsGroupRequestEvent(
            "{\"event\":\"new\",\"requestId\":6,\"groupId\":10,\"groupName\":\"z_g\",\"ownerId\":3602,"
            "\"requesterId\":3603,\"requesterUsername\":\"zD_1\",\"requesterNickname\":\"zD_1\","
            "\"handlerId\":0,\"message\":\"add me\",\"status\":0,\"createdTs\":1794818825198}", event, info);
        check(ok && event == "new" && info.requestId == 6 && info.groupId == 10 &&
                  info.requesterId == 3603 && info.message == "add me" && info.status == 0,
              "ParseImsGroupRequestEvent 143 new (flat)");
    }

    // 6) type=143 入群申请（内嵌子对象写法）
    {
        std::string event; ImsGroupRequestInfo info;
        const bool ok = ParseImsGroupRequestEvent(
            "{\"event\":\"pending\",\"request\":{\"requestId\":7,\"groupId\":10,\"requesterId\":3603,"
            "\"requesterNickname\":\"n7\",\"message\":\"m7\",\"status\":0}}", event, info);
        check(ok && event == "pending" && info.requestId == 7 && info.requesterNickname == "n7",
              "ParseImsGroupRequestEvent 143 pending (nested)");
    }

    // 7) type=144 审批结果
    {
        std::string event; ImsGroupRequestInfo info;
        const bool ok = ParseImsGroupRequestEvent(
            "{\"event\":\"accepted\",\"requestId\":6,\"groupId\":10,\"requesterId\":3603,\"status\":1}", event, info);
        check(ok && event == "accepted" && info.requestId == 6 && info.status == 1,
              "ParseImsGroupRequestEvent 144 accepted");
    }

    // 8) HTTP data.group + members[] + groups[] + requests[]
    {
        ImsJson root;
        ImsJson data;
        const std::string detail =
            "{\"code\":0,\"message\":\"ok\",\"data\":{\"group\":{\"groupId\":1,\"conversationId\":5,"
            "\"name\":\"项目讨论组\",\"ownerId\":1001,\"status\":0,\"memberLimit\":200,\"memberCount\":3,"
            "\"myRole\":1,\"createdTs\":1760000000000},\"members\":["
            "{\"userId\":1001,\"username\":\"a\",\"nickname\":\"A\",\"groupNickname\":\"群主A\",\"role\":1,\"joinedTs\":1},"
            "{\"userId\":1002,\"username\":\"b\",\"nickname\":\"B\",\"role\":3,\"joinedTs\":2}]}}";
        check(ImsJson::Parse(detail, root) && root["data"].IsObject(), "HTTP detail 外层解析");
        data = root["data"];
        ImsGroupInfo g;
        check(ParseGroupInfo(data["group"], g) && g.groupId == 1 && g.memberCount == 3 && g.myRole == 1,
              "ParseGroupInfo group");
        std::vector<ImsGroupMember> ms;
        check(ParseGroupMembers(data, ms) && ms.size() == 2 &&
                  ms[0].DisplayName() == "群主A" && ms[1].DisplayName() == "B",
              "ParseGroupMembers + DisplayName 优先级");

        ImsJson lroot;
        check(ImsJson::Parse("{\"data\":{\"groups\":[]}}", lroot), "ParseGroupList 外层解析");
        std::vector<ImsGroupInfo> gs;
        ImsJson l2;
        ImsJson::Parse("{\"data\":{\"groups\":[{\"groupId\":2,\"name\":\"g2\"}]}}", l2);
        check(ParseGroupList(l2["data"], gs) && gs.size() == 1 && gs[0].groupId == 2, "ParseGroupList");

        ImsJson r2;
        ImsJson::Parse("{\"data\":{\"requests\":[{\"requestId\":9,\"groupId\":2,\"requesterId\":3,\"status\":0}]}}", r2);
        std::vector<ImsGroupRequestInfo> rs;
        check(ParseGroupRequests(r2["data"], rs) && rs.size() == 1 && rs[0].requestId == 9, "ParseGroupRequests");
    }

    // 9) 历史数组解析 is_group
    {
        std::vector<ImsHistoryMessage> hist; std::string err;
        const bool ok = ParseImsHistory(
            "[{\"msg_id\":1,\"conversation_id\":5,\"seq\":1,\"from\":1002,\"type\":1,\"ts\":10,\"content\":\"a\",\"is_group\":true},"
            "{\"msg_id\":2,\"conversation_id\":5,\"seq\":2,\"from\":1001,\"type\":1,\"ts\":11,\"content\":\"b\"}]",
            hist, err);
        check(ok && hist.size() == 2 && hist[0].is_group && !hist[1].is_group,
              "ParseImsHistory 解析 is_group");
    }

    std::printf("==== 群协议解析自测完成，失败项: %d ====\n", failures);
    return failures;
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
