#pragma once
// ============================================================================
// /*************群聊功能：领域模型与事件序列化（BE-GROUP-01~08）***************/
// ----------------------------------------------------------------------------
// group_types.h：群聊模块的领域模型与 TCP 事件 JSON 序列化
// ----------------------------------------------------------------------------
// 为什么单独放一个头文件？（和 friend_types.h 同样的道理）
//   群功能同时被三个地方使用：
//     1) Db 层：群/成员/申请/群消息写扩散 的 SQL；
//     2) HTTP 层：返回群列表、群详情+成员、入群申请；
//     3) TCP 层：把“成员变化 / 解散 / 资料变更 / 入群申请与结果”推给在线用户。
//   把结构体和事件 JSON 的拼装集中在这里，避免三处各写一份、字段漂移。
//
// 设计原则（沿用好友模块）：
//   * 数据库只存“事实”（群、成员、申请状态），事件只是事实变化后的通知；
//   * 推送事件统一用 JSON 字符串放进 Message.content，便于协议扩展；
//   * 所有事件都带 event / action 字段，客户端据此区分同一 type 下的不同语义。
//
// 与好友模块的差异：
//   * 群是“一对多”，一个事件往往要推给整群成员，所以序列化函数里都带 group_id，
//     由 HTTP 层查群成员后逐个推送；
//   * 成员变化用 action 区分 added/removed/quit/roleChanged/transferred，
//     客户端可以增量刷新群列表 / 成员列表。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace kc {

// 群资料。member_count / my_role 是“查询者视角”的冗余字段：
// 列表/详情接口会 JOIN group_members 顺带查出来，省一次请求。
struct GroupInfo {
    std::uint64_t id = 0;                 // groups.id（协议里的 groupId）
    std::uint64_t conversation_id = 0;    // 对应会话 id（type=2）
    std::string   name;
    std::string   announcement;
    std::string   intro;
    std::string   avatar_url;
    std::uint64_t owner_id = 0;
    int           status = 0;             // 0=正常，1=已解散
    int           member_limit = 200;
    int           member_count = 0;       // 查询者视角：当前成员数
    int           my_role = 0;            // 查询者在该群的角色：1/2/3；0=非成员
    std::uint64_t created_ts = 0;         // 毫秒时间戳
};

// 群成员的一行（群详情接口返回）。
struct GroupMemberInfo {
    std::uint64_t user_id = 0;
    std::string   username;
    std::string   nickname;
    std::string   avatar_url;
    std::string   group_nickname;
    int           role = 3;               // 1=群主，2=管理员，3=普通成员
    std::uint64_t joined_ts = 0;
};

// 一条入群申请。
struct GroupRequestInfo {
    std::uint64_t id = 0;                 // group_requests.id
    std::uint64_t group_id = 0;
    std::string   group_name;
    std::uint64_t owner_id = 0;           // 群主（新申请实时推给他；管理员靠登录补推/列表）
    std::uint64_t requester_id = 0;
    std::string   requester_username;
    std::string   requester_nickname;
    std::uint64_t handler_id = 0;         // 审批人（0=未处理）
    std::string   message;
    int           status = 0;             // 0=待处理，1=已通过，2=已拒绝，3=已取消
    std::uint64_t created_ts = 0;
};

// 群“写操作”后的统一返回：既给 HTTP 响应，又给 TCP 推送用。
//   members：操作后的完整成员列表（用于群事件向全群广播 + 群详情响应）
//   affected_ids：本次操作“直接作用”的人（新增/被移除的成员、新群主、申请人），
//                 HTTP 层据此决定 GroupMemberChanged 事件里的 user_id。
struct GroupOpResult {
    GroupInfo                          group;
    std::vector<GroupMemberInfo>       members;
    std::vector<std::uint64_t>         affected_ids;
};

// ---------------------------------------------------------------------------
// JSON 写入小工具：统一写 key:value，减少样板代码。
// ---------------------------------------------------------------------------
namespace gdetail {

inline void putStr(rapidjson::Writer<rapidjson::StringBuffer>& w,
                   const char* k, const std::string& v) {
    w.Key(k); w.String(v.c_str(), static_cast<rapidjson::SizeType>(v.size()));
}
inline void putU64(rapidjson::Writer<rapidjson::StringBuffer>& w,
                   const char* k, std::uint64_t v) {
    w.Key(k); w.Uint64(v);
}
inline void putInt(rapidjson::Writer<rapidjson::StringBuffer>& w,
                   const char* k, int v) {
    w.Key(k); w.Int(v);
}

} // namespace gdetail

// ---------------------------------------------------------------------------
// HTTP 响应片段：把一个群 / 一个成员 / 一条申请写成 JSON（供 http_server 组装）。
// 这些函数只写“对象本体”，不含 {code,message,data} 外层。
// ---------------------------------------------------------------------------
inline void write_group_json(rapidjson::Writer<rapidjson::StringBuffer>& w,
                             const GroupInfo& g) {
    using namespace gdetail;
    w.StartObject();
    putU64(w, "groupId",        g.id);
    putU64(w, "conversationId", g.conversation_id);
    putStr(w, "name",           g.name);
    putStr(w, "announcement",   g.announcement);
    putStr(w, "intro",          g.intro);
    putStr(w, "avatarUrl",      g.avatar_url);
    putU64(w, "ownerId",        g.owner_id);
    putInt(w, "status",         g.status);
    putInt(w, "memberLimit",    g.member_limit);
    putInt(w, "memberCount",    g.member_count);
    putInt(w, "myRole",         g.my_role);
    putU64(w, "createdTs",      g.created_ts);
    w.EndObject();
}

inline void write_member_json(rapidjson::Writer<rapidjson::StringBuffer>& w,
                              const GroupMemberInfo& m) {
    using namespace gdetail;
    w.StartObject();
    putU64(w, "userId",       m.user_id);
    putStr(w, "username",     m.username);
    putStr(w, "nickname",     m.nickname);
    putStr(w, "avatarUrl",    m.avatar_url);
    putStr(w, "groupNickname", m.group_nickname);
    putInt(w, "role",         m.role);
    putU64(w, "joinedTs",     m.joined_ts);
    w.EndObject();
}

inline void write_request_json(rapidjson::Writer<rapidjson::StringBuffer>& w,
                               const GroupRequestInfo& r) {
    using namespace gdetail;
    w.StartObject();
    putU64(w, "requestId",         r.id);
    putU64(w, "groupId",           r.group_id);
    putStr(w, "groupName",         r.group_name);
    putU64(w, "ownerId",           r.owner_id);
    putU64(w, "requesterId",       r.requester_id);
    putStr(w, "requesterUsername", r.requester_username);
    putStr(w, "requesterNickname", r.requester_nickname);
    putU64(w, "handlerId",         r.handler_id);
    putStr(w, "message",           r.message);
    putInt(w, "status",            r.status);
    putU64(w, "createdTs",         r.created_ts);
    w.EndObject();
}

// ---------------------------------------------------------------------------
// TCP 事件 1：GroupMemberChanged（type=140）
// ---------------------------------------------------------------------------
// 触发：邀请成功(added) / 移出(removed) / 退群(quit) / 任命或取消管理员(roleChanged)
//       / 转让群主(transferred)。
// 方向：群操作者 -> 全群成员（含被操作者本人，方便多端一致刷新）。
// action：added / removed / quit / roleChanged / transferred
// user_id：被操作的那个成员；role：其操作后的角色（未知填 0）。
// ---------------------------------------------------------------------------
inline std::string serialize_group_member_changed(const char* action,
                                                  std::uint64_t group_id,
                                                  std::uint64_t user_id,
                                                  int role,
                                                  std::uint64_t operator_id) {
    using namespace gdetail;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    putStr(w, "event",      "group_member_changed");
    putStr(w, "action",     action ? action : "added");
    putU64(w, "group_id",   group_id);
    putU64(w, "user_id",    user_id);
    putInt(w, "role",       role);
    putU64(w, "operator_id", operator_id);
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// TCP 事件 2：GroupDismissed（type=141）
// 触发：群主解散群。方向：群主 -> 全群成员（解散前）。
// ---------------------------------------------------------------------------
inline std::string serialize_group_dismissed(std::uint64_t group_id,
                                             std::uint64_t operator_id,
                                             const std::string& name) {
    using namespace gdetail;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    putStr(w, "event",      "group_dismissed");
    putU64(w, "group_id",   group_id);
    putU64(w, "operator_id", operator_id);
    putStr(w, "name",       name);
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// TCP 事件 3：GroupInfoChanged（type=142）
// 触发：群主/管理员修改群名/公告/简介/头像。方向：操作者 -> 全群成员。
// 客户端收到后可重新调用 GET /api/groups/{id} 拉取最新资料。
// ---------------------------------------------------------------------------
inline std::string serialize_group_info_changed(std::uint64_t group_id,
                                                std::uint64_t operator_id,
                                                const std::string& name) {
    using namespace gdetail;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    putStr(w, "event",      "group_info_changed");
    putU64(w, "group_id",   group_id);
    putU64(w, "operator_id", operator_id);
    putStr(w, "name",       name);
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// TCP 事件 4：GroupRequest（type=143）
// 触发：有人申请入群 -> 推给群主/管理员（在线直推；离线靠登录补推 pending）。
// event："new" 新申请 / "pending" 登录补推的待处理申请。
// ---------------------------------------------------------------------------
inline std::string serialize_group_request_event(const GroupRequestInfo& r,
                                                 const char* event) {
    using namespace gdetail;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    putStr(w, "event",             event ? event : "new");
    write_request_json(w, r);
    w.EndObject();
    // 注意：write_request_json 已经写了 requestId 等；这里外层再补 event。
    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// TCP 事件 5：GroupRequestResult（type=144）
// 触发：管理员审批入群申请。方向：审批人 -> 申请人。
// event："accepted" / "rejected"。申请人离线时 result_notified 保持 0，
//       登录时补推，逻辑与好友 FriendRequestResult 一致。
// ---------------------------------------------------------------------------
inline std::string serialize_group_request_result_event(const GroupRequestInfo& r,
                                                        const char* event) {
    using namespace gdetail;
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    putStr(w, "event", event ? event : "accepted");
    write_request_json(w, r);
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

} // namespace kc
